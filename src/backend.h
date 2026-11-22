/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The platform layer. backend_ps5.c implements it on the console with the
 * payload SDK; backend_host.c is a mock for building and testing the server
 * on a development machine. Everything else is portable. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "../third_party/cjson/cJSON.h"

/* DualSense button bits, as libScePad reports them. */
enum
{
    BTN_CREATE = 0x00000001u, /* "share" on older pads */
    BTN_L3 = 0x00000002u,
    BTN_R3 = 0x00000004u,
    BTN_OPTIONS = 0x00000008u,
    BTN_UP = 0x00000010u,
    BTN_RIGHT = 0x00000020u,
    BTN_DOWN = 0x00000040u,
    BTN_LEFT = 0x00000080u,
    BTN_L2 = 0x00000100u,
    BTN_R2 = 0x00000200u,
    BTN_L1 = 0x00000400u,
    BTN_R1 = 0x00000800u,
    BTN_TRIANGLE = 0x00001000u,
    BTN_CIRCLE = 0x00002000u,
    BTN_CROSS = 0x00004000u,
    BTN_SQUARE = 0x00008000u,
    BTN_PS = 0x00010000u,
    BTN_TOUCHPAD = 0x00100000u,
};

typedef struct pad_state
{
    uint32_t buttons;
    uint8_t lx, ly, rx, ry; /* 0..255, 128 is centre, y grows downwards */
    uint8_t l2, r2;         /* analog triggers 0..255 */
    bool touch;             /* one finger on the touchpad */
    uint16_t touch_x, touch_y; /* 0..1919, 0..1079 */
} pad_state_t;

/* ---- virtual pad ------------------------------------------------------ */

typedef struct pad_open_opts
{
    int user_id;         /* <= 0: the foreground user */
    bool exclusive;      /* disconnect other controllers of that user */
    bool dismiss_prompt; /* close "Who's using this controller?" with Circle */
} pad_open_opts_t;

/* Create and bind the device. Returns the InsertData handle (>= 0) or -1.
 * The report (a JSON object) collects what every underlying call returned. */
int backend_pad_create(const pad_open_opts_t *opts, cJSON *report, uint64_t *device_id,
                       int *user_id);
/* Disconnect a controller (sceMbusDisconnectDevice); the PS button on it
 * reconnects it. Returns the result code, or -1 when unavailable. */
int backend_pad_disconnect(uint64_t device);
int backend_pad_insert(int handle, const pad_state_t *state);
int backend_pad_destroy(int handle);

/* ---- captures --------------------------------------------------------- */

/* Folders the console writes its screenshots into, searched in order. */
extern const char *const backend_capture_roots[];
extern const size_t backend_capture_root_count;
/* Remove a capture as the gallery does (file, thumbnail, database row). */
int backend_capture_delete(const char *path);

/* Native screenshot: drives the libSceComposite command queue to make
 * AgcCompositor write the current composited frame into its shared screenshot
 * memory, then kernel_proc_copyout()s a linear BGRA frame out of it. No
 * ptrace, no pad input, no Create hold. Portable: the display resolution comes
 * from sceVideoOutGetResolutionStatus. Returns 0 and sets *out_bgra (malloc'd,
 * caller free()s), *out_w, *out_h on success, or -1 with details in `report`. */
int backend_capture_native(uint8_t **out_bgra, int *out_w, int *out_h, cJSON *report);
/* Find a process by exact name (e.g. "AgcCompositor.elf"). Returns its pid
 * or 0 when not found. */
pid_t backend_find_pid_by_name(const char *name);

/* ---- processes -------------------------------------------------------- */

typedef struct proc_entry
{
    pid_t pid, ppid;
    uid_t uid;
    char state[8];
    char name[40];
    char title_id[16];
    uint32_t app_id;
    uint64_t authid;
    uint64_t rss_bytes, vsize_bytes;
    int threads;
    double cpu_percent;
    uint64_t start_sec; /* seconds since boot (0 when unknown) */
} proc_entry_t;

/* A malloc'd array of every process. Returns the count or -errno. */
int backend_proc_list(proc_entry_t **out);
/* Extra per-process detail (threads, memory map, files) into obj. */
int backend_proc_detail(pid_t pid, cJSON *obj, bool threads, bool vmmap, bool files);

/* ---- system ----------------------------------------------------------- */

void backend_system_info(cJSON *obj);
int backend_notify(const char *message);

/* ---- applications ----------------------------------------------------- */

typedef struct app_running
{
    int app_id;
    char title_id[16];
} app_running_t;

int backend_app_running(app_running_t *out); /* 0 when a big app runs */
int backend_app_launch(const char *title_id, const char *args, cJSON *report);
int backend_app_kill(int app_id, cJSON *report);
/* Where installed titles live, and their metadata. */
extern const char *const backend_app_dirs[];
extern const size_t backend_app_dir_count;
extern const char *backend_appmeta_dir;

/* ---- kernel log ------------------------------------------------------- */

/* A readable fd delivering the kernel log, or -1. *what describes it. */
int backend_klog_open(const char **what);

/* ---- lifecycle -------------------------------------------------------- */

int backend_init(void);
const char *backend_data_dir(void);
