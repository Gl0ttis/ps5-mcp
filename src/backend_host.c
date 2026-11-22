/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host mock backend: builds and runs the whole server on a Linux development
 * machine, so the MCP layer, the pad engine, the capture pipeline and the
 * tools can be exercised without a console.
 *
 *   - the virtual pad logs state changes; holding Create >= 1 s "takes a
 *     screenshot": a test card PNG appears in the mock capture folder;
 *   - processes come from /proc;
 *   - the kernel log is a heartbeat line every 2 s plus lines the mock injects
 *     (DEVICE_ADDED when the pad opens, a controller table);
 *   - everything lives under $PS5MCP_MOCK_ROOT (default ./build/mock). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "backend.h"
#include "image.h"
#include "klog.h"
#include "pad_klog.h"
#include "util.h"

static char g_root[512];
static char g_capture_root[600];
static char g_app_root[600];
static char g_appmeta_root[600];
static char g_data_root[600];

const char *const backend_capture_roots[] = {g_capture_root};
const size_t backend_capture_root_count = 1;
const char *const backend_app_dirs[] = {g_app_root};
const size_t backend_app_dir_count = 1;
const char *backend_appmeta_dir = g_appmeta_root;

static app_running_t g_running;
static pthread_mutex_t g_mock_lock = PTHREAD_MUTEX_INITIALIZER;

static void mkdirs(const char *path)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/')
        {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    mkdir(tmp, 0777);
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f)
    {
        fputs(text, f);
        fclose(f);
    }
}

int backend_init(void)
{
    const char *root = getenv("PS5MCP_MOCK_ROOT");
    snprintf(g_root, sizeof(g_root), "%s", root ? root : "build/mock");
    snprintf(g_capture_root, sizeof(g_capture_root), "%s/user/av_contents/photo", g_root);
    snprintf(g_app_root, sizeof(g_app_root), "%s/user/app", g_root);
    snprintf(g_appmeta_root, sizeof(g_appmeta_root), "%s/system_data/priv/appmeta", g_root);
    snprintf(g_data_root, sizeof(g_data_root), "%s/data/ps5-mcp", g_root);
    mkdirs(g_capture_root);
    mkdirs(g_data_root);
    static const struct
    {
        const char *tid, *name;
    } apps[] = {{"PPSA01234", "Mock Racer"}, {"CUSA00001", "Mock Legacy"},
                {"FAKE00001", "My Homebrew"}};
    for (size_t i = 0; i < ARRAY_LEN(apps); i++)
    {
        char path[800];
        snprintf(path, sizeof(path), "%s/%s", g_app_root, apps[i].tid);
        mkdirs(path);
        snprintf(path, sizeof(path), "%s/%s", g_appmeta_root, apps[i].tid);
        mkdirs(path);
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"titleId\":\"%s\",\"localizedParameters\":{\"defaultLanguage\":\"en-US\","
                 "\"en-US\":{\"titleName\":\"%s\"}}}",
                 apps[i].tid, apps[i].name);
        snprintf(path, sizeof(path), "%s/%s/param.json", g_appmeta_root, apps[i].tid);
        write_text(path, json);
    }
    return 0;
}

const char *backend_data_dir(void)
{
    return g_data_root;
}

/* ---- pad --------------------------------------------------------------- */

static uint32_t g_last_buttons;
static uint64_t g_create_down_ms;
static unsigned g_shot_counter;

/* A test card: colour bars, a grid every 240 px (to check crop maths) and a
 * counter band, at 3840x2160 like a 4K console. */
static void mock_capture(void)
{
    const int w = 3840, h = 2160;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    if (rgb == NULL)
        return;
    static const uint8_t bars[8][3] = {{235, 235, 235}, {235, 235, 16}, {16, 235, 235},
                                       {16, 235, 16},   {235, 16, 235}, {235, 16, 16},
                                       {16, 16, 235},   {16, 16, 16}};
    const unsigned n = ++g_shot_counter;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            const uint8_t *c = bars[x * 8 / w];
            p[0] = c[0];
            p[1] = c[1];
            p[2] = c[2];
            if (x % 240 == 0 || y % 240 == 0)
                p[0] = p[1] = p[2] = 0;
            if (y > 1800 && y < 2000 && x < (int)(n % 16 + 1) * 240)
                p[0] = 255, p[1] = 128, p[2] = 0;
        }
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char dir[700], path[800];
    snprintf(dir, sizeof(dir), "%s/MOCK00000", g_capture_root);
    mkdirs(dir);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &tm);
    snprintf(path, sizeof(path), "%s/mock_%s_%04u.png", dir, stamp, n);
    /* Write to a temporary name, then rename, as the console finishes a file. */
    char tmp[820];
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    if (img_write_png(tmp, rgb, w, h) == 0)
        rename(tmp, path);
    free(rgb);
    fprintf(stderr, "[mock] screenshot -> %s\n", path);
}

static void *mock_capture_thread(void *arg)
{
    (void)arg;
    sleep_ms(400); /* the console takes a moment to write the file */
    mock_capture();
    return NULL;
}

/* Controllers the mock has "disconnected": later tables leave them out. */
static uint64_t g_gone[8];
static int g_gone_n;

static bool mock_gone(uint64_t device)
{
    for (int i = 0; i < g_gone_n; i++)
        if (g_gone[i] == device)
            return true;
    return false;
}

/* The mock's virtual pad, and a "real" controller the user already has. */
#define MOCK_DEVICE 0x2a00000000001234ull
#define MOCK_REAL_PAD 0x2a0000000000beefull

/* klog lines in SceShellCore's own formats (see pad_klog.h). */
static void *mock_device_added(void *arg)
{
    (void)arg;
    sleep_ms(150); /* the console logs it a moment after AddDevice */
    char line[256];
    snprintf(line, sizeof(line),
             "<118>[SceShellCore] UmmMbusEventThread() event=DEVICE_ADDED deviceId=0x%llx "
             "deviceType=3 userId=0x1(1)",
             (unsigned long long)MOCK_DEVICE);
    klog_inject("<118>[SceShellCore] UmmMbusEventThread() event=DEVICE_REMOVED "
                "deviceId=0x2a0000000000aaaa deviceType=1 userId=0x10000001(268435457)");
    klog_inject(line);
    return NULL;
}

int backend_pad_create(const pad_open_opts_t *opts, cJSON *report, uint64_t *device_id,
                       int *user_id)
{
    /* Same flow as the console: AddDevice, then the id from the klog ring
     * through the portable parser. */
    const uint64_t since = klog_next_seq();
    pthread_t t;
    if (pthread_create(&t, NULL, mock_device_added, NULL) == 0)
        pthread_detach(t);
    bool exact = false;
    const uint64_t device = padlog_wait_device(since, 1000, &exact);
    char hex[24];
    snprintf(hex, sizeof(hex), "0x%llx", (unsigned long long)device);
    cJSON_AddStringToObject(report, "backend", "host mock");
    cJSON_AddStringToObject(report, "add", "0x803b0006");
    cJSON_AddStringToObject(report, "device", hex);
    cJSON_AddStringToObject(report, "device_from", exact ? "sub type 22" : "only DEVICE_ADDED");
    if (device == 0)
        return -1;
    cJSON_AddStringToObject(report, "bind", "0x00000000");
    *device_id = device;
    *user_id = opts->user_id > 0 ? opts->user_id : 0x10000001;
    /* The controller table the console logs after the owner changes: a real
     * DualSense, our pad, another payload's virtual pad, an empty slot. */
    char row[200];
    klog_inject("<118>[SceShellCore] [CIM]  Pad    deviceId        userId    main  sub    "
                "padHandle  vpadDeviceId isCoPilot");
    if (!mock_gone(MOCK_REAL_PAD))
    {
        snprintf(row, sizeof(row), "<118>[SceShellCore] [CIM] [0] 0x%016llx 0x%08x  %2d   %2d  %8d    0x%08x    %d",
                 (unsigned long long)MOCK_REAL_PAD, 0x10000001, 1, 1, 0x100, 0, 0);
        klog_inject(row);
    }
    snprintf(row, sizeof(row), "<118>[SceShellCore] [CIM] [1] 0x%016llx 0x%08x  %2d   %2d  %8d    0x%08x    %d",
             (unsigned long long)device, 0x10000001, 1, 22, 0x101, 0x1234, 0);
    klog_inject(row);
    if (!mock_gone(0x2a00000000005555ull))
    {
        snprintf(row, sizeof(row), "<118>[SceShellCore] [CIM] [2] 0x%016llx 0x%08x  %2d   %2d  %8d    0x%08x    %d",
                 0x2a00000000005555ull, 0x10000001, 1, 22, 0x102, 0x5555, 0);
        klog_inject(row);
    }
    klog_inject("<118>[SceShellCore] [CIM] [3] 0xffffffffffffffff 0xffffffff   0    0        -1    0x00000000    0");
    return 0x1234;
}

int backend_pad_disconnect(uint64_t device)
{
    fprintf(stderr, "[mock] disconnect 0x%llx\n", (unsigned long long)device);
    if (g_gone_n < 8 && !mock_gone(device))
        g_gone[g_gone_n++] = device;
    return 0;
}

int backend_pad_insert(int handle, const pad_state_t *s)
{
    (void)handle;
    pthread_mutex_lock(&g_mock_lock);
    if (s->buttons != g_last_buttons)
    {
        if ((s->buttons & BTN_CREATE) && !(g_last_buttons & BTN_CREATE))
            g_create_down_ms = now_ms();
        if (!(s->buttons & BTN_CREATE) && (g_last_buttons & BTN_CREATE))
        {
            if (now_ms() - g_create_down_ms >= 1000)
            {
                pthread_t t;
                if (pthread_create(&t, NULL, mock_capture_thread, NULL) == 0)
                    pthread_detach(t);
            }
            else
                fprintf(stderr, "[mock] short Create press: the Create menu would open\n");
        }
        fprintf(stderr, "[mock] pad buttons 0x%05x lx=%u ly=%u rx=%u ry=%u l2=%u r2=%u\n",
                s->buttons, s->lx, s->ly, s->rx, s->ry, s->l2, s->r2);
        g_last_buttons = s->buttons;
    }
    pthread_mutex_unlock(&g_mock_lock);
    return 0;
}

int backend_pad_destroy(int handle)
{
    (void)handle;
    return 0;
}

int backend_capture_delete(const char *path)
{
    return unlink(path) == 0 ? 0 : -1;
}

int backend_capture_native(uint8_t **out_bgra, int *out_w, int *out_h, cJSON *report)
{
    *out_bgra = NULL;
    *out_w = 0;
    *out_h = 0;
    cJSON_AddStringToObject(report, "backend", "host mock");
    cJSON_AddStringToObject(report, "error",
                            "native capture uses PS5 system libraries; only runs on a console");
    return -1;
}

pid_t backend_find_pid_by_name(const char *name)
{
    (void)name;
    return 0;
}

/* ---- processes ---------------------------------------------------------- */

int backend_proc_list(proc_entry_t **out)
{
    DIR *dir = opendir("/proc");
    if (dir == NULL)
        return -errno;
    size_t cap = 256, n = 0;
    proc_entry_t *list = calloc(cap, sizeof(*list));
    struct dirent *e;
    const long page = sysconf(_SC_PAGESIZE);
    while (list && (e = readdir(dir)) != NULL)
    {
        const int pid = atoi(e->d_name);
        if (pid <= 0)
            continue;
        char path[64], buf[1024];
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (f == NULL)
            continue;
        const size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[got] = '\0';
        char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
        if (lp == NULL || rp == NULL)
            continue;
        if (n == cap)
        {
            cap *= 2;
            proc_entry_t *grown = realloc(list, cap * sizeof(*list));
            if (grown == NULL)
                break;
            list = grown;
        }
        proc_entry_t *p = &list[n++];
        memset(p, 0, sizeof(*p));
        p->pid = pid;
        snprintf(p->name, sizeof(p->name), "%.*s", (int)(rp - lp - 1), lp + 1);
        char state;
        int ppid;
        long threads;
        unsigned long vsize;
        long rss;
        sscanf(rp + 2,
               "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %ld %*d %*u %lu %ld",
               &state, &ppid, &threads, &vsize, &rss);
        p->ppid = ppid;
        p->state[0] = state;
        p->threads = (int)threads;
        p->vsize_bytes = vsize;
        p->rss_bytes = (uint64_t)rss * (uint64_t)page;
        p->uid = getuid();
    }
    closedir(dir);
    if (list == NULL)
        return -ENOMEM;
    *out = list;
    return (int)n;
}

int backend_proc_detail(pid_t pid, cJSON *obj, bool threads, bool vmmap, bool files)
{
    char path[64];
    if (threads)
    {
        snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);
        DIR *dir = opendir(path);
        cJSON *arr = cJSON_AddArrayToObject(obj, "threads");
        struct dirent *e;
        while (dir && (e = readdir(dir)) != NULL)
        {
            if (e->d_name[0] == '.')
                continue;
            char comm[64] = "", p2[384];
            snprintf(p2, sizeof(p2), "/proc/%d/task/%s/comm", (int)pid, e->d_name);
            FILE *f = fopen(p2, "r");
            if (f)
            {
                if (fgets(comm, sizeof(comm), f))
                    comm[strcspn(comm, "\n")] = '\0';
                fclose(f);
            }
            cJSON *t = cJSON_CreateObject();
            cJSON_AddNumberToObject(t, "tid", atoi(e->d_name));
            cJSON_AddStringToObject(t, "name", comm);
            cJSON_AddItemToArray(arr, t);
        }
        if (dir)
            closedir(dir);
    }
    if (vmmap)
        cJSON_AddStringToObject(obj, "memory_map", "see /proc/PID/maps (host mock)");
    if (files)
        cJSON_AddStringToObject(obj, "files", "see /proc/PID/fd (host mock)");
    return 0;
}

/* ---- system ----------------------------------------------------------- */

void backend_system_info(cJSON *obj)
{
    struct utsname u;
    uname(&u);
    cJSON *c = cJSON_AddObjectToObject(obj, "console");
    cJSON_AddStringToObject(c, "backend", "host mock");
    cJSON_AddStringToObject(c, "model", u.machine);
    cJSON_AddStringToObject(c, "firmware", u.release);
    pthread_mutex_lock(&g_mock_lock);
    if (g_running.app_id)
    {
        cJSON *app = cJSON_AddObjectToObject(obj, "running_app");
        cJSON_AddStringToObject(app, "title_id", g_running.title_id);
        cJSON_AddNumberToObject(app, "app_id", g_running.app_id);
    }
    pthread_mutex_unlock(&g_mock_lock);
}

int backend_notify(const char *message)
{
    fprintf(stderr, "[mock] notification: %s\n", message);
    char line[1200];
    snprintf(line, sizeof(line), "<6>[SceShellUI] notification: %s", message);
    klog_inject(line);
    return 0;
}

/* ---- applications ------------------------------------------------------ */

int backend_app_running(app_running_t *out)
{
    pthread_mutex_lock(&g_mock_lock);
    *out = g_running;
    pthread_mutex_unlock(&g_mock_lock);
    return out->app_id ? 0 : -1;
}

int backend_app_launch(const char *title_id, const char *args, cJSON *report)
{
    pthread_mutex_lock(&g_mock_lock);
    g_running.app_id = 0x60000100;
    snprintf(g_running.title_id, sizeof(g_running.title_id), "%s", title_id);
    pthread_mutex_unlock(&g_mock_lock);
    char line[256];
    snprintf(line, sizeof(line), "<6>[SceSysCore] launched %s args=%s", title_id,
             args ? args : "");
    klog_inject(line);
    cJSON_AddStringToObject(report, "backend", "host mock");
    cJSON_AddNumberToObject(report, "launch", 0x60000100);
    return 0;
}

int backend_app_kill(int app_id, cJSON *report)
{
    pthread_mutex_lock(&g_mock_lock);
    if (g_running.app_id == app_id)
        memset(&g_running, 0, sizeof(g_running));
    pthread_mutex_unlock(&g_mock_lock);
    cJSON_AddStringToObject(report, "kill", "0x00000000");
    return 0;
}

/* ---- klog --------------------------------------------------------------- */

static int g_klog_pipe[2] = {-1, -1};

static void *heartbeat(void *arg)
{
    (void)arg;
    for (unsigned i = 0;; i++)
    {
        char line[128];
        const int n = snprintf(line, sizeof(line), "<6>[mock] heartbeat %u\n", i);
        if (write(g_klog_pipe[1], line, (size_t)n) < 0)
            return NULL;
        sleep_ms(2000);
    }
}

int backend_klog_open(const char **what)
{
    *what = "host mock heartbeat";
    if (g_klog_pipe[0] >= 0)
        return -1; /* only one reader; the mock never closes */
    if (pipe(g_klog_pipe) != 0)
        return -1;
    pthread_t t;
    pthread_create(&t, NULL, heartbeat, NULL);
    pthread_detach(t);
    return g_klog_pipe[0];
}
