/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Read-only filesystem tools: enough to look at logs, configs, crash dumps
 * and a title's files. Writing is left to the FTP server. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "image.h"
#include "mcp.h"
#include "util.h"

#define FS_MAX_READ (1024 * 1024)
#define FS_MAX_ENTRIES 2000

typedef struct entry
{
    char name[256];
    struct stat st;
    bool ok;
} entry_t;

static int by_name(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    const bool dx = x->ok && S_ISDIR(x->st.st_mode), dy = y->ok && S_ISDIR(y->st.st_mode);
    if (dx != dy)
        return dx ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void mode_string(mode_t m, char out[11])
{
    out[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b'
             : S_ISFIFO(m) ? 'p' : S_ISSOCK(m) ? 's' : '-';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        out[i + 1] = (m & (1 << (8 - i))) ? rwx[i] : '-';
    out[10] = '\0';
}

static void human(double bytes, char *out, size_t len)
{
    const char *units[] = {"B", "K", "M", "G", "T"};
    int u = 0;
    while (bytes >= 1024 && u < 4)
    {
        bytes /= 1024;
        u++;
    }
    snprintf(out, len, u ? "%.1f%s" : "%.0f%s", bytes, units[u]);
}

static void list_dir(const char *path, int depth, int max_depth, bool hidden, buf_t *out,
                     int *count)
{
    DIR *dir = opendir(path);
    if (dir == NULL)
    {
        buf_printf(out, "%*s(cannot open %s: %s)\n", depth * 2, "", path, strerror(errno));
        return;
    }
    entry_t *entries = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL)
    {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (!hidden && e->d_name[0] == '.')
            continue;
        if (n == cap)
        {
            cap = cap ? cap * 2 : 64;
            entry_t *grown = realloc(entries, cap * sizeof(entry_t));
            if (grown == NULL)
                break;
            entries = grown;
        }
        snprintf(entries[n].name, sizeof(entries[n].name), "%s", e->d_name);
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", strcmp(path, "/") == 0 ? "" : path, e->d_name);
        entries[n].ok = lstat(full, &entries[n].st) == 0;
        n++;
    }
    closedir(dir);
    qsort(entries, n, sizeof(entry_t), by_name);
    for (size_t i = 0; i < n; i++)
    {
        if (*count >= FS_MAX_ENTRIES)
        {
            buf_printf(out, "... (stopped at %d entries)\n", FS_MAX_ENTRIES);
            break;
        }
        (*count)++;
        const entry_t *x = &entries[i];
        if (!x->ok)
        {
            buf_printf(out, "%*s?????????? %8s %16s  %s\n", depth * 2, "", "?", "?", x->name);
            continue;
        }
        char mode[11], size[16], when[20];
        mode_string(x->st.st_mode, mode);
        human((double)x->st.st_size, size, sizeof(size));
        struct tm tm;
        localtime_r(&x->st.st_mtime, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
        buf_printf(out, "%*s%s %8s %16s  %s%s\n", depth * 2, "", mode, size, when, x->name,
                   S_ISDIR(x->st.st_mode) ? "/" : "");
        if (S_ISDIR(x->st.st_mode) && depth + 1 < max_depth)
        {
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", strcmp(path, "/") == 0 ? "" : path, x->name);
            list_dir(full, depth + 1, max_depth, hidden, out, count);
        }
    }
    free(entries);
}

static void tool_fs_list(const cJSON *args, tool_result_t *r)
{
    const char *path = arg_str(args, "path", "/");
    struct stat st;
    if (stat(path, &st) != 0)
    {
        tr_error(r, "%s: %s", path, strerror(errno));
        return;
    }
    if (!S_ISDIR(st.st_mode))
    {
        char mode[11], when[32];
        mode_string(st.st_mode, mode);
        struct tm tm;
        localtime_r(&st.st_mtime, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
        tr_text(r, "%s %lld bytes, modified %s, uid %d gid %d: %s", mode, (long long)st.st_size,
                when, (int)st.st_uid, (int)st.st_gid, path);
        return;
    }
    const int depth = CLAMP(arg_int(args, "depth", 1), 1, 6);
    buf_t out;
    buf_init(&out);
    int count = 0;
    buf_printf(&out, "%s\n", path);
    list_dir(path, 0, depth, arg_bool(args, "hidden", true), &out, &count);
    tr_text(r, "%s", out.data);
    buf_free(&out);
}

static bool looks_text(const uint8_t *d, size_t n)
{
    size_t bad = 0;
    for (size_t i = 0; i < n; i++)
    {
        const uint8_t c = d[i];
        if (c == 0)
            return false;
        if (c < 9 || (c > 13 && c < 32))
            bad++;
    }
    return bad * 20 <= n;
}

static void hexdump(const uint8_t *d, size_t n, long long base, buf_t *out)
{
    for (size_t i = 0; i < n; i += 16)
    {
        buf_printf(out, "%08llx  ", (unsigned long long)(base + (long long)i));
        for (size_t j = 0; j < 16; j++)
            if (i + j < n)
                buf_printf(out, "%02x%s", d[i + j], j == 7 ? "  " : " ");
            else
                buf_puts(out, j == 7 ? "    " : "   ");
        buf_puts(out, " |");
        for (size_t j = 0; j < 16 && i + j < n; j++)
            buf_printf(out, "%c", d[i + j] >= 32 && d[i + j] < 127 ? d[i + j] : '.');
        buf_puts(out, "|\n");
    }
}

static void tool_fs_read(const cJSON *args, tool_result_t *r)
{
    const char *path = arg_str(args, "path", NULL);
    if (path == NULL)
    {
        tr_error(r, "path is required");
        return;
    }
    const char *encoding = arg_str(args, "encoding", "auto");
    /* Only regular files: a FIFO or a device node could block this thread
     * forever, or have side effects on read. */
    struct stat st;
    if (stat(path, &st) != 0)
    {
        tr_error(r, "%s: %s", path, strerror(errno));
        return;
    }
    if (!S_ISREG(st.st_mode))
    {
        tr_error(r, "%s is not a regular file%s", path,
                 S_ISDIR(st.st_mode) ? ": use fs_list" : " (devices and FIFOs are not read)");
        return;
    }
    const int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
    {
        tr_error(r, "%s: %s", path, strerror(errno));
        return;
    }

    /* Pictures: show them, scaled like a screenshot. */
    if (strcmp(encoding, "auto") == 0 || strcmp(encoding, "image") == 0)
    {
        uint8_t head[16];
        const ssize_t got = pread(fd, head, sizeof(head), 0);
        const img_kind_t kind = got > 0 ? img_sniff(head, (size_t)got) : IMG_UNKNOWN;
        if (kind == IMG_JPEG || kind == IMG_PNG || kind == IMG_BMP)
        {
            close(fd);
            uint8_t *data;
            size_t len;
            const int rf = read_file(path, &data, &len, 256u * 1024u * 1024u);
            if (rf != 0)
            {
                tr_error(r, "%s: %s", path, strerror(-rf));
                return;
            }
            img_opts_t o = {.max_width = arg_int(args, "max_width", 1280), .quality = 85};
            img_out_t img;
            char err[200];
            const int rc = img_process(data, len, &o, &img, err, sizeof(err));
            free(data);
            if (rc != 0)
            {
                tr_error(r, "%s", err);
                return;
            }
            char *b64 = base64_encode(img.data, img.len);
            free(img.data);
            if (b64)
                tr_image(r, b64, img.mime);
            tr_text(r, "%s: %dx%d picture (shown at %dx%d)", path, img.src_w, img.src_h,
                    img.out_w, img.out_h);
            return;
        }
    }

    long long offset = (long long)arg_num(args, "offset", 0);
    if (offset < 0)
        offset = st.st_size + offset < 0 ? 0 : st.st_size + offset;
    const int length = CLAMP(arg_int(args, "length", 65536), 1, FS_MAX_READ);
    uint8_t *data = malloc((size_t)length);
    if (data == NULL)
    {
        close(fd);
        tr_error(r, "out of memory");
        return;
    }
    ssize_t n = pread(fd, data, (size_t)length, (off_t)offset);
    close(fd);
    if (n < 0)
    {
        free(data);
        tr_error(r, "%s: %s", path, strerror(errno));
        return;
    }
    const bool more = offset + n < st.st_size;
    char where[200];
    snprintf(where, sizeof(where), "%s: bytes %lld-%lld of %lld%s", path, offset, offset + n,
             (long long)st.st_size,
             more ? " (more: call again with a larger offset)" : "");

    if (strcmp(encoding, "base64") == 0)
    {
        char *b64 = base64_encode(data, (size_t)n);
        tr_text(r, "%s, base64:\n%s", where, b64 ? b64 : "");
        free(b64);
    }
    else if (strcmp(encoding, "hex") == 0 ||
             (strcmp(encoding, "text") != 0 && !looks_text(data, (size_t)n)))
    {
        buf_t out;
        buf_init(&out);
        hexdump(data, (size_t)(n > 16384 ? 16384 : n), offset, &out);
        tr_text(r, "%s, hex%s:\n%s", where, n > 16384 ? " (first 16 KiB of the read)" : "",
                out.data ? out.data : "");
        buf_free(&out);
    }
    else
    {
        buf_t out;
        buf_init(&out);
        buf_append(&out, data, (size_t)n);
        tr_text(r, "%s\n%s", where, out.data ? out.data : "");
        buf_free(&out);
    }
    free(data);
}

static const tool_def_t k_tools[] = {
    {
        "fs_list",
        "List files",
        "List a directory on the console (or describe a file), with sizes, dates and modes. "
        "Useful places: /data (payload data, logs), /data/homebrew, /user/app (installed "
        "titles), /user/av_contents (captures), /system_data/priv/appmeta, /mnt/usb0.",
        "{'type':'object','properties':{'path':{'type':'string','default':'/'},"
        "'depth':{'type':'integer','default':1,'minimum':1,'maximum':6},"
        "'hidden':{'type':'boolean','default':true}}}",
        TOOL_READ_ONLY | TOOL_IDEMPOTENT,
        tool_fs_list,
    },
    {
        "fs_read",
        "Read a file",
        "Read part of a file on the console. Text is returned as text, binary as a hex dump "
        "(or base64), and JPEG/PNG/BMP pictures as an image. A negative offset counts from the "
        "end, e.g. offset=-4096 for the tail of a log.",
        "{'type':'object','properties':{'path':{'type':'string'},"
        "'offset':{'type':'integer','default':0},"
        "'length':{'type':'integer','default':65536,'maximum':1048576},"
        "'encoding':{'type':'string','enum':['auto','text','hex','base64','image'],"
        "'default':'auto'},"
        "'max_width':{'type':'integer','default':1280,'description':'For pictures.'}},"
        "'required':['path']}",
        TOOL_READ_ONLY | TOOL_IDEMPOTENT,
        tool_fs_read,
    },
};

void tools_fs_register(void)
{
    mcp_register(k_tools, ARRAY_LEN(k_tools));
}
