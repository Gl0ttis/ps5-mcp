/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Screenshots. The whole screen comes from the console's own screenshot,
 * what a person holding Create gets: the composited picture, system UI
 * included. It costs the ~1.2 s the console wants Create held (a shorter
 * press opens the Create menu instead), plus the time to write the file.
 *
 * A title can instead write its own frame, which is quicker and leaves
 * nothing in the gallery. The grab hook contract:
 *   1. the server creates a request file (by default
 *      /data/homebrew/<TITLE_ID>/ps5-mcp-grab, /app0/ps5-mcp-grab inside the
 *      title);
 *   2. every few frames the title tries to unlink() it; when that succeeds it
 *      writes its next frame (JPEG/PNG) as a new file in a folder of its own
 *      (by default /data/homebrew/<TITLE_ID>/screenshots), 0666;
 *   3. the server sends the newest picture there and deletes it. */
#include "screen.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backend.h"
#include "pad.h"
#include "util.h"

#define NEWEST 4

typedef struct capture
{
    char path[512];
    time_t mtime;
    off_t size;
} capture_t;

static bool is_picture(const char *name)
{
    const char *ext = strrchr(name, '.');
    return ext != NULL && (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0 ||
                           strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".jxr") == 0 ||
                           strcasecmp(ext, ".bmp") == 0);
}

static void walk(const char *folder, int depth, capture_t *newest)
{
    DIR *dir = opendir(folder);
    if (dir == NULL)
        return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL)
    {
        if (e->d_name[0] == '.')
            continue;
        capture_t c;
        snprintf(c.path, sizeof(c.path), "%s/%s", folder, e->d_name);
        struct stat st;
        if (stat(c.path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
        {
            if (depth < 6)
                walk(c.path, depth + 1, newest);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !is_picture(e->d_name))
            continue;
        c.mtime = st.st_mtime;
        c.size = st.st_size;
        for (int i = 0; i < NEWEST; i++)
        {
            /* Newer first; same second: the longer (later) name wins. */
            if (newest[i].path[0] != '\0' &&
                (c.mtime < newest[i].mtime ||
                 (c.mtime == newest[i].mtime && strcmp(c.path, newest[i].path) <= 0)))
                continue;
            memmove(&newest[i + 1], &newest[i], sizeof(capture_t) * (size_t)(NEWEST - 1 - i));
            newest[i] = c;
            break;
        }
    }
    closedir(dir);
}

static void captures_newest(capture_t *newest)
{
    memset(newest, 0, sizeof(capture_t) * NEWEST);
    for (size_t i = 0; i < backend_capture_root_count; i++)
    {
        walk(backend_capture_roots[i], 0, newest);
        if (newest[0].path[0] != '\0')
            return;
    }
}

/* Wait for a picture newer than `before` whose size has stopped changing. */
static bool wait_new(const char *folder, const capture_t *before, int timeout_ms, int poll_ms,
                     capture_t *found)
{
    off_t last_size = -1;
    char last_path[512] = "";
    const uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    while (now_ms() < deadline)
    {
        sleep_ms((unsigned)poll_ms);
        capture_t after[NEWEST];
        if (folder != NULL)
        {
            memset(after, 0, sizeof(after));
            walk(folder, 0, after);
        }
        else
            captures_newest(after);
        if (after[0].path[0] == '\0' || strcmp(after[0].path, before->path) == 0)
            continue;
        if (after[0].size > 0 && after[0].size == last_size && strcmp(after[0].path, last_path) == 0)
        {
            *found = after[0];
            return true;
        }
        last_size = after[0].size;
        snprintf(last_path, sizeof(last_path), "%s", after[0].path);
    }
    return false;
}

bool shot_opts_parse(const cJSON *args, shot_opts_t *o, tool_result_t *r)
{
    memset(o, 0, sizeof(*o));
    const char *mode = arg_str(args, "mode", "system");
    if (strcmp(mode, "system") == 0)
        o->mode = SHOT_SYSTEM;
    else if (strcmp(mode, "title") == 0)
        o->mode = SHOT_TITLE;
    else if (strcmp(mode, "latest") == 0)
        o->mode = SHOT_LATEST;
    else if (strcmp(mode, "native") == 0)
        o->mode = SHOT_NATIVE;
    else
    {
        tr_error(r, "mode must be system, title, latest or native");
        return false;
    }
    o->img.max_width = arg_int(args, "max_width", 1280);
    o->img.max_height = arg_int(args, "max_height", 0);
    o->img.quality = arg_int(args, "quality", 80);
    o->img.png = strcmp(arg_str(args, "format", "jpeg"), "png") == 0;
    o->img.grayscale = arg_bool(args, "grayscale", false);
    const cJSON *crop = arg_get(args, "crop");
    if (cJSON_IsObject(crop))
    {
        o->img.crop_x = arg_int(crop, "x", 0);
        o->img.crop_y = arg_int(crop, "y", 0);
        o->img.crop_w = arg_int(crop, "width", 0);
        o->img.crop_h = arg_int(crop, "height", 0);
        if (o->img.crop_w <= 0 || o->img.crop_h <= 0)
        {
            tr_error(r, "crop needs a positive width and height (source pixels, 3840x2160 on a "
                        "4K console)");
            return false;
        }
    }
    o->keep = arg_bool(args, "keep_on_console", false);
    o->hold_ms = CLAMP(arg_int(args, "create_hold_ms", PAD_CREATE_SCREENSHOT_MS), 1000, 5000);
    o->timeout_ms = CLAMP(arg_int(args, "timeout_ms", o->mode == SHOT_TITLE ? 3000 : 10000), 500,
                          30000);
    if (o->mode == SHOT_TITLE)
    {
        const char *tid = arg_str(args, "title_id", NULL);
        const char *request = arg_str(args, "request_path", NULL);
        const char *folder = arg_str(args, "folder", NULL);
        if (tid != NULL)
        {
            if (strchr(tid, '/') || strstr(tid, ".."))
            {
                tr_error(r, "title_id must be a plain title id such as PPSA01234");
                return false;
            }
            snprintf(o->request, sizeof(o->request), "/data/homebrew/%s/ps5-mcp-grab", tid);
            snprintf(o->folder, sizeof(o->folder), "/data/homebrew/%s/screenshots", tid);
        }
        if (request)
            snprintf(o->request, sizeof(o->request), "%s", request);
        if (folder)
            snprintf(o->folder, sizeof(o->folder), "%s", folder);
        if (o->request[0] != '/' || o->folder[0] != '/')
        {
            tr_error(r, "mode=title needs title_id (homebrew under /data/homebrew/<id>) or "
                        "request_path and folder (absolute paths)");
            return false;
        }
    }
    return true;
}

/* Decode, scale, encode and add to the result; deletes per opts. */
static int deliver(const shot_opts_t *o, const capture_t *c, bool system_capture, tool_result_t *r)
{
    uint8_t *data = NULL;
    size_t len = 0;
    const int rf = read_file(c->path, &data, &len, 256u * 1024u * 1024u);
    if (rf != 0)
    {
        tr_error(r, "cannot read %s: %s", c->path, strerror(-rf));
        return -1;
    }

    const img_kind_t kind = img_sniff(data, len);
    if (kind == IMG_JXR)
    {
        /* Nothing on the console decodes JPEG XR, and a raw 4K JXR is no use
         * to an agent: leave the file for a person to look at. */
        free(data);
        tr_error(r,
                 "the capture is JPEG XR because HDR is on, and it cannot be decoded on the "
                 "console. Turn HDR off (Settings > Screen and Video > Video Output > HDR) for "
                 "agent sessions, or use mode=title. The capture was left at %s.",
                 c->path);
        return -1;
    }

    int deleted = 1;
    if (!o->keep)
        deleted = system_capture ? backend_capture_delete(c->path) : unlink(c->path);

    img_out_t img;
    char err[256];
    if (img_process(data, len, &o->img, &img, err, sizeof(err)) != 0)
    {
        free(data);
        tr_error(r, "%s (%s)", err, c->path);
        return -1;
    }
    free(data);
    char *b64 = base64_encode(img.data, img.len);
    free(img.data);
    if (b64 == NULL)
    {
        tr_error(r, "out of memory encoding the picture");
        return -1;
    }
    tr_image(r, b64, img.mime);
    tr_text(r, "%dx%d (source %dx%d%s), %zu bytes, from %s%s", img.out_w, img.out_h, img.src_w,
            img.src_h, o->img.crop_w ? ", cropped" : "", img.len, c->path,
            o->keep ? " (kept)" : deleted == 0 ? " (deleted from the console)"
                                               : " (could not delete it)");
    return 0;
}

int shot_take(const shot_opts_t *o, tool_result_t *r)
{
    if (o->mode == SHOT_LATEST)
    {
        capture_t newest[NEWEST];
        captures_newest(newest);
        if (newest[0].path[0] == '\0')
        {
            tr_error(r, "no captures under %s", backend_capture_roots[0]);
            return -1;
        }
        shot_opts_t keep = *o;
        keep.keep = true; /* never delete a capture we did not make */
        return deliver(&keep, &newest[0], true, r);
    }

    if (o->mode == SHOT_TITLE)
    {
        capture_t before[NEWEST];
        memset(before, 0, sizeof(before));
        walk(o->folder, 0, before);
        const int fd = open(o->request, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0)
        {
            tr_error(r, "cannot create the request %s: %s", o->request, strerror(errno));
            return -1;
        }
        close(fd);
        chmod(o->request, 0666);
        capture_t found;
        if (!wait_new(o->folder, &before[0], o->timeout_ms, 15, &found))
        {
            /* A request nobody took must not linger until the next launch. */
            const bool taken = unlink(o->request) != 0;
            tr_error(r,
                     "no picture in %s within %d ms: the title %s the request. Is it running, "
                     "and does it have the grab hook? mode=system works meanwhile.",
                     o->folder, o->timeout_ms, taken ? "took" : "never took");
            return -1;
        }
        return deliver(o, &found, false, r);
    }

    if (o->mode == SHOT_NATIVE)
    {
        /* backend_capture_native drives the libSceComposite command queue to
         * make AgcCompositor commit the current composited frame to its shared
         * screenshot memory, which we then read out as linear BGRA. See
         * backend_ps5.c for the full chain. */
        cJSON *report = cJSON_CreateObject();
        uint8_t *bgra = NULL;
        int w = 0, h = 0;
        const int rc = backend_capture_native(&bgra, &w, &h, report);
        if (rc != 0)
        {
            char *rep = cJSON_PrintUnformatted(report);
            cJSON_Delete(report);
            tr_error(r, "native capture failed: %s", rep ? rep : "{}");
            free(rep);
            return -1;
        }
        cJSON_Delete(report);

        /* Convert BGRA → RGB in place-ish for img_process. */
        uint8_t *rgb = malloc((size_t)w * (size_t)h * 3u);
        if (rgb == NULL)
        {
            free(bgra);
            tr_error(r, "oom converting the native frame");
            return -1;
        }
        for (int i = 0; i < w * h; i++)
        {
            rgb[i * 3 + 0] = bgra[i * 4 + 2];
            rgb[i * 3 + 1] = bgra[i * 4 + 1];
            rgb[i * 3 + 2] = bgra[i * 4 + 0];
        }
        free(bgra);

        /* Stage as a PNG and feed through the usual crop/scale/encode path. */
        const char *staged = "/data/ps5-mcp/native.png";
        if (img_write_png(staged, rgb, w, h) != 0)
        {
            free(rgb);
            tr_error(r, "encoding the native frame failed");
            return -1;
        }
        free(rgb);

        capture_t found;
        memset(&found, 0, sizeof(found));
        snprintf(found.path, sizeof(found.path), "%s", staged);
        struct stat st;
        if (stat(found.path, &st) == 0)
        {
            found.mtime = st.st_mtime;
            found.size = st.st_size;
        }
        shot_opts_t keep = *o;
        keep.keep = true; /* not a console gallery file — do not delete */
        const int d = deliver(&keep, &found, false, r);
        unlink(found.path);
        if (d == 0)
            tr_text(r, "(captured natively via AgcCompositor, %dx%d BGRA, "
                       "no pad)", w, h);
        return d;
    }

    /* SHOT_SYSTEM */
    if (!pad_is_open())
    {
        tr_error(r, "mode=system holds the Create button of the virtual pad: call pad_open "
                    "first (mode=latest returns the newest existing capture)");
        return -1;
    }
    capture_t before[NEWEST];
    captures_newest(before);
    /* Create is added to whatever is held, so a held trigger or stick stays
     * held in the picture. */
    const pad_state_t held = pad_get();
    pad_state_t shot = held;
    shot.buttons |= BTN_CREATE;
    pad_set(&shot);
    sleep_ms((unsigned)o->hold_ms);
    pad_set(&held);
    capture_t found;
    const bool ok = wait_new(NULL, &before[0], o->timeout_ms, 250, &found);
    if (!ok)
    {
        tr_error(r,
                 "no new capture within %d ms (newest is still %s). Screenshots may be blocked "
                 "by the running title, or the Create menu opened: press circle and retry.",
                 o->timeout_ms, before[0].path[0] ? before[0].path : "none");
        return -1;
    }
    return deliver(o, &found, true, r);
}

/* ---- the screenshot tool ---------------------------------------------- */

static void tool_screenshot(const cJSON *args, tool_result_t *r)
{
    shot_opts_t o;
    if (!shot_opts_parse(args, &o, r))
        return;
    /* system needs the pad+gallery and must serialize with any other capture;
     * native drives the compositor directly and takes its own slot there. We
     * serialize native too so two captures do not fight for the 16 compositor
     * client slots at once. */
    const bool lock = o.mode == SHOT_SYSTEM || o.mode == SHOT_NATIVE;
    if (lock)
        console_lock();
    shot_take(&o, r);
    if (lock)
        console_unlock();
}

static const tool_def_t k_tools[] = {
    {
        "screenshot",
        "Screenshot",
        "Take a picture of what the PS5 shows and return it as an image. mode=system (default) "
        "captures the whole composited screen by holding Create on the virtual pad, then "
        "deletes the file from the gallery (~3 s; needs pad_open; JPEG XR with HDR on and "
        "unreadable on the console). mode=title asks the running title via the grab hook. "
        "mode=latest returns the newest existing capture. mode=native triggers a fresh 4K "
        "compose inside SceShellUI via pt_call (no pad, ~200 ms SceShellUI freeze) and reads "
        "the composed BGRA frame from the compositor shm.",
        "{'type':'object','properties':{" SHOT_SCHEMA_PROPS "}}",
        0,
        tool_screenshot,
    },
};

void tools_screen_register(void)
{
    mcp_register(k_tools, ARRAY_LEN(k_tools));
}
