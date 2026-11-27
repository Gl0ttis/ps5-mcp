/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console backend, built with the ps5-payload-dev SDK.
 *
 * The virtual pad is the system's own virtual device (libScePad's
 * scePadVirtualDevice*, what Remote Play goes through), so the home screen
 * and every title see a controller:
 *   - libSceMbus has no SDK stub and libScePad imports from it: dlopen it first;
 *   - scePadInit, then scePadSetProcessPrivilege(1), in that order;
 *   - AddDevice for an unassigned user (1) answers 0x803b0006 ("assignment
 *     pending") even when the device exists; the device id is only in the
 *     kernel log (SceShellCore's DEVICE_ADDED line, see pad_klog.h);
 *   - sceMbusBindDeviceWithUserId gives it to a user, as choosing a profile on
 *     "Who's using this controller?" does;
 *   - the InsertData handle differs by firmware: every candidate is tried and
 *     what each returned is reported.
 * The device id and the controller table are read from this server's own klog
 * ring, so no second klog client can race the open.
 *
 * App launch/kill and notifications follow ps5-payload-dev/websrv; process
 * listing follows the SDK's ps sample. Every call reports its raw result code
 * so that another firmware can be brought up from the tool output. */
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <machine/reg.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "backend.h"
#include "klog.h"
#include "pad_klog.h"
#include "util.h"

#define DATA_DIR "/data/ps5-mcp"
#define KLOG_SERVER_PORT 3232

#define PAD_DEVICE_TYPE 3
#define PAD_UNASSIGNED_USER 1
#define PAD_ASSIGNMENT_PENDING 0x803b0006u
#define PAYLOAD_AUTHID 0x4800000000010003ull
#define CONTENT_DELETE_BUFFER 0x4000

const char *const backend_capture_roots[] = {"/user/av_contents/photo", "/user/av_contents"};
const size_t backend_capture_root_count = 2;
const char *const backend_app_dirs[] = {"/user/app", "/mnt/ext0/user/app", "/mnt/usb0/user/app"};
const size_t backend_app_dir_count = 3;
const char *backend_appmeta_dir = "/system_data/priv/appmeta";

/* ---- system library declarations ---------------------------------------- */

typedef struct pad_touch
{
    uint16_t x, y;
    uint8_t id;
    uint8_t reserved[3];
} pad_touch_t;

typedef struct pad_data
{
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    uint8_t l2, r2;
    uint8_t reserved0[2];
    float orientation[4];
    float acceleration[3];
    float angular_velocity[3];
    uint8_t touch_count;
    uint8_t reserved1[7];
    pad_touch_t touch[2];
    uint8_t connected;
    uint8_t reserved2[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count;
    uint8_t reserved3[15];
} pad_data_t;
_Static_assert(sizeof(pad_data_t) == 120, "the pad sample is 120 bytes");

typedef struct add_device_param
{
    int32_t size;
    int32_t user_id;
    int32_t unknown[6];
} add_device_param_t;

extern int32_t scePadInit(void);
extern int32_t scePadSetProcessPrivilege(int32_t privilege);
extern int32_t scePadVirtualDeviceAddDevice(void *param, int32_t device_type);
extern int32_t scePadVirtualDeviceInsertData(int32_t handle, const void *data);
extern int32_t scePadVirtualDeviceDeleteDevice(int32_t handle);

extern int sceUserServiceInitialize(void *param);
extern int sceUserServiceGetForegroundUser(int *user_id);
extern int sceUserServiceGetUserName(int user_id, char *name, size_t size);

/* sceKernelGetAppInfo's output. The title id starts at offset 16: on 13.20
 * reading it at 20 (after an app_type word, as the SDK's ps sample lays it
 * out) returned "40087" for NPXS40087. */
typedef struct app_info
{
    uint32_t app_id;
    uint32_t reserved0;
    uint64_t unknown1;
    char title_id[10];
    char unknown2[0x3e];
} app_info_t;
_Static_assert(__builtin_offsetof(app_info_t, title_id) == 16, "title id at offset 16");
extern int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

extern int sceKernelGetHwModelName(char *name);
extern long sceKernelGetCpuFrequency(void);
extern int sceKernelGetCpuTemperature(int *temperature);
extern int sceKernelGetSocSensorTemperature(int sensor, int *temperature);

typedef struct notify_request
{
    char useless1[45];
    char message[3075];
} notify_request_t;
extern int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

typedef struct app_launch_ctx
{
    uint32_t structsize;
    uint32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
} app_launch_ctx_t;
extern int sceSystemServiceGetAppIdOfRunningBigApp(void);
extern int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
extern int sceSystemServiceLaunchApp(const char *title_id, char **argv, app_launch_ctx_t *ctx);

/* Resolved at run time (no SDK stubs). */
static int32_t (*mbus_bind)(uint64_t device_id, int32_t user_id);
static int32_t (*mbus_disconnect)(uint64_t device_id);

typedef struct content_delete_param
{
    uint64_t unknown0;
    uint64_t buffer_size;
    uint64_t unknown[6];
} content_delete_param_t;
static int32_t (*content_delete_by_path)(const char *path);

static pthread_mutex_t g_lib_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_pad_libraries;
static bool g_delete_library;

static void report_hex(cJSON *report, const char *key, uint64_t value)
{
    char text[24];
    snprintf(text, sizeof(text), value > 0xffffffffull ? "0x%llx" : "0x%08llx",
             (unsigned long long)value);
    cJSON_AddStringToObject(report, key, text);
}

/* ---- lifecycle ---------------------------------------------------------- */

/* The authid the pad service and libSceContentDelete accept from a payload
 * (the ELF loader already gives root and every capability). Raised only when
 * a pad or capture call first needs it; launch, kill and
 * notify run with the loader's credentials until then. If those fail after
 * pad_open on some firmware, this is the first suspect. */
static void elevate(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    static const uint8_t all_caps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    kernel_set_ucred_authid(getpid(), PAYLOAD_AUTHID);
    kernel_set_ucred_caps(getpid(), all_caps);
}

int backend_init(void)
{
    mkdir(DATA_DIR, 0777);
    return sceUserServiceInitialize(NULL);
}

const char *backend_data_dir(void)
{
    return DATA_DIR;
}

/* ---- pad ---------------------------------------------------------------- */

static int pad_libraries(cJSON *report)
{
    pthread_mutex_lock(&g_lib_lock);
    if (g_pad_libraries)
    {
        pthread_mutex_unlock(&g_lib_lock);
        return 0;
    }
    elevate();
    /* libScePad's imports from libSceMbus are unresolved in a payload: a pad
     * call before this load can end the process. */
    void *mbus = dlopen("/system/common/lib/libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (mbus == NULL)
        mbus = dlopen("libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (mbus == NULL)
    {
        cJSON_AddStringToObject(report, "libSceMbus", "not loaded");
        pthread_mutex_unlock(&g_lib_lock);
        return -1;
    }
    mbus_bind = (int32_t (*)(uint64_t, int32_t))dlsym(mbus, "sceMbusBindDeviceWithUserId");
    mbus_disconnect = (int32_t (*)(uint64_t))dlsym(mbus, "sceMbusDisconnectDevice");
    cJSON_AddStringToObject(report, "mbus_bind", mbus_bind ? "found" : "missing");
    const int32_t init = scePadInit();
    const int32_t privilege = scePadSetProcessPrivilege(1);
    report_hex(report, "padInit", (uint32_t)init);
    report_hex(report, "privilege", (uint32_t)privilege);
    if (init == 0)
        g_pad_libraries = true;
    pthread_mutex_unlock(&g_lib_lock);
    return init == 0 ? 0 : -1;
}

static void to_pad_data(const pad_state_t *s, pad_data_t *d)
{
    memset(d, 0, sizeof(*d));
    d->buttons = s->buttons;
    d->lx = s->lx;
    d->ly = s->ly;
    d->rx = s->rx;
    d->ry = s->ry;
    d->l2 = s->l2;
    d->r2 = s->r2;
    /* The service discards a sample whose orientation is not a rotation. */
    d->orientation[3] = 1.0f;
    d->acceleration[1] = 1.0f;
    if (s->touch)
    {
        d->touch_count = 1;
        d->touch[0].x = s->touch_x;
        d->touch[0].y = s->touch_y;
        d->touch[0].id = 0;
    }
    d->connected = 1;
    d->connected_count = 1;
    d->timestamp = now_us();
}

int backend_pad_insert(int handle, const pad_state_t *state)
{
    pad_data_t d;
    to_pad_data(state, &d);
    return scePadVirtualDeviceInsertData(handle, &d);
}

int backend_pad_create(const pad_open_opts_t *opts, cJSON *report, uint64_t *device_id,
                       int *user_id)
{
    if (pad_libraries(report) != 0)
        return -1;
    int user = opts->user_id;
    if (user <= PAD_UNASSIGNED_USER && sceUserServiceGetForegroundUser(&user) != 0)
        user = -1;
    report_hex(report, "user", (uint32_t)user);

    cJSON *status = cJSON_CreateObject();
    klog_status(status);
    const bool klog_ok = cJSON_IsTrue(cJSON_GetObjectItem(status, "connected"));
    cJSON_Delete(status);
    if (!klog_ok)
        cJSON_AddStringToObject(report, "klog", "not connected: the device id cannot be read");
    const uint64_t since = klog_next_seq();

    /* Created for user 1 the device arrives with no owner, and is then bound.
     * Created for the real user, AddDevice answers 0x803b0001 and makes
     * nothing (firmware 13.60). */
    add_device_param_t param;
    memset(&param, 0, sizeof(param));
    param.size = (int32_t)sizeof(param);
    param.user_id = PAD_UNASSIGNED_USER;
    const int32_t added = scePadVirtualDeviceAddDevice(&param, PAD_DEVICE_TYPE);
    report_hex(report, "add", (uint32_t)added);

    bool exact = false;
    const uint64_t device = klog_ok ? padlog_wait_device(since, 3000, &exact) : 0;
    report_hex(report, "device", device);
    cJSON_AddStringToObject(report, "device_from",
                            device == 0 ? "no usable DEVICE_ADDED line in klog"
                            : exact     ? "DEVICE_ADDED line with sub type 22"
                                        : "the only DEVICE_ADDED line after AddDevice");
    if (added != 0 && (uint32_t)added != PAD_ASSIGNMENT_PENDING && device == 0)
        return -1;

    if (device != 0 && user > PAD_UNASSIGNED_USER && mbus_bind != NULL)
        report_hex(report, "bind", (uint32_t)mbus_bind(device, user));
    else
        cJSON_AddStringToObject(report, "bind", "skipped");

    /* Find the handle InsertData accepts. */
    int32_t candidates[10];
    int count = 0;
    if (added > 0)
        candidates[count++] = added;
    for (int i = 0; i < 6; i++)
        if (param.unknown[i] > 0)
            candidates[count++] = param.unknown[i];
    if (device != 0)
    {
        candidates[count++] = (int32_t)(device & 0xffffffffu);
        if ((device >> 32) != 0)
            candidates[count++] = (int32_t)(device >> 32);
    }
    pad_state_t neutral;
    memset(&neutral, 0, sizeof(neutral));
    neutral.lx = neutral.ly = neutral.rx = neutral.ry = 128;
    cJSON *tries = cJSON_AddArrayToObject(report, "insert_tries");
    for (int i = 0; i < count; i++)
    {
        const int32_t r = backend_pad_insert(candidates[i], &neutral);
        char text[48];
        snprintf(text, sizeof(text), "insert(0x%x)=0x%08x", (unsigned)candidates[i], (unsigned)r);
        cJSON_AddItemToArray(tries, cJSON_CreateString(text));
        if (r == 0)
        {
            *device_id = device;
            *user_id = user;
            return candidates[i];
        }
    }
    return -1;
}

int backend_pad_disconnect(uint64_t device)
{
    return mbus_disconnect ? mbus_disconnect(device) : -1;
}

int backend_pad_destroy(int handle)
{
    return scePadVirtualDeviceDeleteDevice(handle);
}

/* ---- captures ----------------------------------------------------------- */

static void delete_library(void)
{
    pthread_mutex_lock(&g_lib_lock);
    if (!g_delete_library)
    {
        g_delete_library = true;
        elevate();
        void *lib = dlopen("/system/common/lib/libSceContentDelete.sprx", RTLD_NOW);
        if (lib != NULL)
        {
            int32_t (*init)(const content_delete_param_t *) =
                (int32_t (*)(const content_delete_param_t *))dlsym(lib,
                                                                   "sceContentDeleteInitialize");
            content_delete_param_t param;
            memset(&param, 0, sizeof(param));
            param.buffer_size = CONTENT_DELETE_BUFFER;
            const int32_t r = init ? init(&param) : -1;
            log_msg("sceContentDeleteInitialize -> 0x%08x", (unsigned)r);
            if (r == 0)
                content_delete_by_path =
                    (int32_t (*)(const char *))dlsym(lib, "sceContentDeleteByPath");
        }
        else
            log_msg("libSceContentDelete: %s", dlerror());
    }
    pthread_mutex_unlock(&g_lib_lock);
}

int backend_capture_delete(const char *path)
{
    delete_library();
    if (content_delete_by_path == NULL)
        return -1;
    /* The service knows captures by their path under /user, with or without
     * that prefix: try both. Never unlink by hand: the gallery's database and
     * its files would go out of step. */
    int32_t r = content_delete_by_path(path);
    if (r != 0 && strncmp(path, "/user/", 6) == 0)
        r = content_delete_by_path(path + 5);
    log_msg("delete %s -> 0x%08x", path, (unsigned)r);
    return r;
}

/* Experimental in-payload capture via direct memory reads of AgcCompositor
 * (pid 57, /system/sys/AgcCompositor.elf). The compositor keeps framebuffers,
 * UI textures and render surfaces in its process memory; our payload uses
 * kernel_proc_copyout to read them. Different consoles render at different
 * resolutions, so the first call scans the compositor's VM map and reports
 * candidate BGRA regions; a second call (passing base/width/height) reads
 * one of them as a raw frame.
 *
 * Direct POSIX shm_open doesn't work: the compositor's shm objects at
 * /SceWebCore/CompositorShm... live in a jail-isolated namespace that
 * payload-context callers can't see. The kernel-R/W primitive (which our
 * elevated payload has) bypasses that restriction cleanly. */

pid_t backend_find_pid_by_name(const char *name)
{
    proc_entry_t *list = NULL;
    const int n = backend_proc_list(&list);
    if (n <= 0)
    {
        free(list);
        return 0;
    }
    pid_t found = 0;
    for (int i = 0; i < n; i++)
    {
        if (strcmp(list[i].name, name) == 0)
        {
            found = list[i].pid;
            break;
        }
    }
    free(list);
    return found;
}

/* ---- native screenshot: CoreDump ptrace + compositor shm readback -------
 *
 * How this works end-to-end:
 * 1. Scan SceShellUI's text for sceScreenshotCaptureForCoreDump and the
 *    DAT_013a84a0 compositor shm base pointer (both runtime-discovered —
 *    no hardcoded FW offsets; see sui_trigger_compose).
 * 2. Patch CoreDump's mode byte 0x03 → 0x05 so it composes at 4K.
 * 3. pt_attach SceShellUI, direct pt_call(fn_coredump, ...) with fast-trap
 *    (PT_CONTINUE + 0xCC return trap — NOT single-step; see sui_pt_call).
 *    SceShellUI is frozen for the ~150 ms CoreDump duration.
 * 4. CoreDump's save step fails with 0x80be0006 (expected — fp16 intermediate
 *    format exceeds save's format check), but the compose + GPU dispatch
 *    have already written a 4K BGRA frame to compositor shm.
 * 5. Revert the mode patch, pt_detach.
 * 6. Register OUR process as a compositor client (sceCompositorInit) so the
 *    shm is mapped at 0x4000000000 in our address space, then read the frame
 *    from (shm_base + 0x8000000) — formula from the vtable[0x10] method body
 *    (mode-5 bitmap 0x10, tz=4, 4 << 25 = 0x8000000).
 *
 * The scanned offsets and 4 KB target-side scratch are cached per SceShellUI
 * PID. The only firmware-specific state we touch on the client-side is the
 * compositor client-slot table (16 entries) — we init+quit per capture so
 * the slot usage stays at 0 between calls.
 *
 * See memory ps5-screenshot-working-state.md for the derivation. */

typedef int (*comp_init_t)(uint32_t, uint32_t, uint32_t, uint8_t);
typedef void (*comp_quit_t)(void);
typedef int (*setinfo_t)(pid_t, const void *);

/* ------------------------------------------------------------------------ */
/* ShellUI inject: drives sceScreenshotCaptureForCoreDump from within        */
/* SceShellUI to force a fresh composited frame into compositor shm.         */
/*                                                                           */
/* Why: the compositor does not update its screenshot shm on its own; a      */
/* real screenshot event (Create hold, Trophy capture, etc.) is normally     */
/* what triggers it. We trigger it ourselves by pt_call'ing CoreDump         */
/* directly from the hijacked main thread (not a new pthread — see memo      */
/* ps5-shellui-oom-pfauthclient for why pthread injection races with the     */
/* libthr heap mutex on fresh-boot SceShellUI).                              */
/*                                                                           */
/* Mode-5 patch: a single-byte eboot patch (mov r?, 0x300000093 → 0x500000093)*/
/* makes CoreDump compose at 3840x2160 instead of 1280x720; the save step    */
/* fails afterwards with 0x80be0006, which is fine — the compose + GPU       */
/* dispatch has already written to the compositor shm. We revert the patch   */
/* before pt_detach so Sony's own CoreDump calls stay at 720p as designed.   */
/*                                                                           */
/* Firmware-agnostic: no hardcoded SceShellUI offsets. CoreDump's entry      */
/* point, the mode byte inside it, and DAT_013a84a0 (compositor shm base    */
/* pointer) are all signature-scanned at runtime — see sui_scan_coredump_fn  */
/* and sui_scan_vt10_dat. The scan results are cached across captures on     */
/* the same SceShellUI PID. */

static int sui_sys_ptrace(int req, pid_t pid, caddr_t addr, int data)
{
    const pid_t me = getpid();
    uint64_t old = kernel_get_ucred_authid(me);
    if (!old) return -1;
    if (kernel_set_ucred_authid(me, 0x4800000000010003LL)) return -1;
    int ret = (int)syscall(SYS_ptrace, req, pid, addr, data);
    int se = errno;
    kernel_set_ucred_authid(me, old);
    errno = se;
    return ret;
}
static int sui_pt_attach(pid_t p)
{
    if (sui_sys_ptrace(PT_ATTACH, p, 0, 0) == -1) return -1;
    if (waitpid(p, 0, 0) == -1) return -1;
    return 0;
}
static int sui_pt_detach(pid_t p) { return sui_sys_ptrace(PT_DETACH, p, 0, 0); }
static int sui_pt_getregs(pid_t p, struct reg *r) { return sui_sys_ptrace(PT_GETREGS, p, (caddr_t)r, 0); }
static int sui_pt_setregs(pid_t p, const struct reg *r) { return sui_sys_ptrace(PT_SETREGS, p, (caddr_t)r, 0); }
static int sui_pt_step(pid_t p) {
    if (sui_sys_ptrace(PT_STEP, p, (caddr_t)1, 0)) return -1;
    if (waitpid(p, 0, 0) < 0) return -1;
    return 0;
}
static int sui_pt_copyin(pid_t p, const void *buf, intptr_t dst, size_t len)
{
    struct ptrace_io_desc iod = {.piod_op=PIOD_WRITE_D,.piod_offs=(void*)dst,.piod_addr=(void*)buf,.piod_len=len};
    return sui_sys_ptrace(PT_IO, p, (caddr_t)&iod, 0);
}
static long sui_pt_syscall(pid_t pid, int sysno, uint64_t a0, uint64_t a1,
                            uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    intptr_t addr = kernel_dynlib_resolve(pid, 0x1, "HoLVWNanBBc");
    if (!addr) addr = kernel_dynlib_resolve(pid, 0x2001, "HoLVWNanBBc");
    if (!addr) return -1;
    addr += 0xa;
    struct reg bak, jmp;
    if (sui_pt_getregs(pid, &bak)) return -1;
    jmp = bak;
    jmp.r_rip = addr; jmp.r_rax = sysno;
    jmp.r_rdi = a0; jmp.r_rsi = a1; jmp.r_rdx = a2;
    jmp.r_r10 = a3; jmp.r_r8 = a4; jmp.r_r9 = a5;
    if (sui_pt_setregs(pid, &jmp)) return -1;
    int steps = 0;
    while (jmp.r_rsp <= bak.r_rsp) {
        if (sui_pt_step(pid)) { sui_pt_setregs(pid, &bak); return -1; }
        if (sui_pt_getregs(pid, &jmp)) { sui_pt_setregs(pid, &bak); return -1; }
        if (++steps > 200000) { sui_pt_setregs(pid, &bak); return -1; }
    }
    sui_pt_setregs(pid, &bak);
    return (long)jmp.r_rax;
}
static intptr_t sui_pt_mmap(pid_t p, size_t len, int prot)
{
    return sui_pt_syscall(p, 477, 0, len, prot, 0x1002, (uint64_t)-1, 0);
}
/* Trap-return VA: a 0xCC byte in SceShellUI's address space that sui_pt_call
 * uses as the fake return address so the called function returns into a
 * SIGTRAP we can catch. Discovered once by sui_find_cc_pad(), reused for all
 * calls on the same SceShellUI PID. */
static intptr_t sui_trap_va = 0;
static pid_t    sui_trap_pid = -1;

/* Scan a small chunk of SceShellUI's text for 8 consecutive 0xCC bytes
 * (interstitial function padding — safe to land into with a SIGTRAP). */
static intptr_t sui_find_cc_pad(pid_t pid, uint64_t eboot)
{
    uint8_t page[4096];
    for (intptr_t off = 0x80000; off < 0x100000; off += 4096) {
        if (kernel_proc_copyout(pid, (intptr_t)(eboot + off), page, 4096) != 0) continue;
        for (int i = 0; i < 4096 - 8; i++) {
            int ok = 1;
            for (int j = 0; j < 8; j++) if (page[i+j] != 0xcc) { ok = 0; break; }
            if (ok) return (intptr_t)(eboot + off + i);
        }
    }
    return 0;
}

/* Fast pt_call: set up the register file as if calling `addr(a0..a5)` with a
 * trap VA pushed as the fake return address, then PT_CONTINUE (full-speed
 * execution). When the called function RETs it lands on the 0xCC trap byte,
 * raising SIGTRAP; we catch it and read rax. Required for long-running
 * target functions like fn_coredump (~150 ms) — step-based pt_call would be
 * millions of single-step syscalls. XMM state is also saved/restored because
 * the called function may clobber FP registers the caller held live. */
static long sui_pt_call(pid_t pid, intptr_t addr, uint64_t a0, uint64_t a1,
                         uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    if (!sui_trap_va) return -1;
    struct reg bak, jmp;
    struct fpreg fpbak;
    if (sui_pt_getregs(pid, &bak)) return -2;
    int have_fp = (sui_sys_ptrace(PT_GETFPREGS, pid, (caddr_t)&fpbak, 0) == 0);
    jmp = bak;
    uint64_t retaddr = (uint64_t)sui_trap_va;
    jmp.r_rsp -= 8;
    struct ptrace_io_desc iod = {.piod_op=PIOD_WRITE_D,
                                 .piod_offs=(void*)jmp.r_rsp,
                                 .piod_addr=&retaddr, .piod_len=8};
    if (sui_sys_ptrace(PT_IO, pid, (caddr_t)&iod, 0)) { sui_pt_setregs(pid, &bak); return -3; }
    jmp.r_rip = addr;
    jmp.r_rdi = a0; jmp.r_rsi = a1; jmp.r_rdx = a2;
    jmp.r_rcx = a3; jmp.r_r8 = a4; jmp.r_r9 = a5;
    if (sui_pt_setregs(pid, &jmp)) { sui_pt_setregs(pid, &bak); return -4; }
    /* Loop until SIGTRAP at trap_va. Non-trap signals (SIGCHLD, SIGSEGV from
     * normal page faults, etc.) are re-continued so the function keeps running. */
    struct reg ret;
    int status = 0;
    for (int tries = 0; tries < 1000; tries++) {
        if (sui_sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, 0) == -1) { sui_pt_setregs(pid, &bak); return -5; }
        if (waitpid(pid, &status, 0) < 0) { sui_pt_setregs(pid, &bak); return -6; }
        if (!WIFSTOPPED(status)) { sui_pt_setregs(pid, &bak); return -9; }
        if (sui_pt_getregs(pid, &ret)) { sui_pt_setregs(pid, &bak); return -7; }
        int sig = WSTOPSIG(status);
        /* SIGTRAP after int3 lands RIP at trap_va+1; some kernels report trap_va. */
        int at_trap = ((uint64_t)ret.r_rip == (uint64_t)sui_trap_va ||
                       (uint64_t)ret.r_rip == (uint64_t)sui_trap_va + 1);
        if (sig == SIGTRAP && at_trap) break;
    }
    const long rax = (long)ret.r_rax;
    if (sui_pt_setregs(pid, &bak)) return -8;
    if (have_fp) sui_sys_ptrace(PT_SETFPREGS, pid, (caddr_t)&fpbak, 0);
    return rax;
}

static pid_t sui_find_pid(void)
{
    int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC};
    size_t sz = 0; sysctl(mib, 3, NULL, &sz, NULL, 0);
    uint8_t *buf = malloc(sz + 0x10000); sz += 0x10000;
    if (!buf) return -1;
    sysctl(mib, 3, buf, &sz, NULL, 0);
    pid_t found = -1;
    for (size_t off = 0; off + sizeof(struct kinfo_proc) <= sz;) {
        const struct kinfo_proc *kp = (const struct kinfo_proc *)(buf + off);
        if (kp->ki_structsize <= 0) break;
        if (!strcmp(kp->ki_comm, "SceShellUI")) { found = kp->ki_pid; break; }
        off += kp->ki_structsize;
    }
    free(buf); return found;
}

static uint64_t sui_find_eboot(pid_t pid)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, pid};
    size_t sz = 0; sysctl(mib, 4, NULL, &sz, NULL, 0);
    uint8_t *buf = malloc(sz + 0x10000); sz += 0x10000;
    if (!buf) return 0;
    sysctl(mib, 4, buf, &sz, NULL, 0);
    uint64_t base = 0, best = 0;
    for (size_t off = 0; off + sizeof(struct kinfo_vmentry) <= sz;) {
        const struct kinfo_vmentry *kv = (const struct kinfo_vmentry *)(buf + off);
        if (kv->kve_structsize <= 0) break;
        if ((kv->kve_protection & KVME_PROT_EXEC) &&
            (kv->kve_end - kv->kve_start) > best) {
            best = kv->kve_end - kv->kve_start;
            base = kv->kve_start;
        }
        off += kv->kve_structsize;
    }
    free(buf); return base;
}

/* Every executable mapping of at least 1 MB, in address order. sui_find_eboot
 * assumes the main executable is the largest of them, which holds on 13.20 but
 * not on 12.02: there SceShellUI's eboot (0x400000, ~13 MB) is smaller than one
 * of its libraries, so the signature scans must be able to try the others. */
static int sui_exec_regions(pid_t pid, uint64_t *st, uint64_t *en, int max)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, pid};
    size_t sz = 0; sysctl(mib, 4, NULL, &sz, NULL, 0);
    uint8_t *buf = malloc(sz + 0x10000); sz += 0x10000;
    if (!buf) return 0;
    sysctl(mib, 4, buf, &sz, NULL, 0);
    int n = 0;
    for (size_t off = 0; off + sizeof(struct kinfo_vmentry) <= sz && n < max;) {
        const struct kinfo_vmentry *kv = (const struct kinfo_vmentry *)(buf + off);
        if (kv->kve_structsize <= 0) break;
        if ((kv->kve_protection & KVME_PROT_EXEC) &&
            (kv->kve_end - kv->kve_start) >= 0x100000) {
            st[n] = kv->kve_start; en[n] = kv->kve_end; n++;
        }
        off += kv->kve_structsize;
    }
    free(buf);
    return n;
}

/* Signature-scan SceShellUI's eboot text for sceScreenshotCaptureForCoreDump.
 * The function sets the compose config at ctx+0x854 = 0x93 (flags) and
 * ctx+0x858 = 0x03 (mode-3, 720p default) via a single `mov r?, 0x300000093`
 * instruction. We find that instruction and patch the mode byte to 0x05 for
 * a 4K compose. The instruction is Sony-semantic (not a compiler artifact),
 * so the pattern is stable across firmware revisions.
 *
 * Returns the function entry VA, or 0 if not found. *out_mode_byte_va gets
 * the VA of the mode byte we need to patch. */
static uint64_t sui_scan_coredump_fn(pid_t pid, uint64_t eboot, uint64_t ebsize,
                                      uint64_t *out_mode_byte_va)
{
    if (out_mode_byte_va) *out_mode_byte_va = 0;
    static const uint8_t prologue[] = {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
        0x41, 0x55, 0x41, 0x54, 0x53
    };
    const size_t CHUNK = 0x10000;
    uint8_t *buf = malloc(CHUNK + 16);
    if (!buf) return 0;
    uint64_t scan_end = ebsize ? ebsize : 0x400000;
    uint64_t match_va = 0;
    for (uint64_t off = 0; off + 10 < scan_end; off += CHUNK - 16) {
        size_t n = CHUNK;
        if (off + n > scan_end) n = (size_t)(scan_end - off);
        if (kernel_proc_copyout(pid, (intptr_t)(eboot + off), buf, n) != 0) continue;
        for (size_t i = 0; i + 10 <= n; i++) {
            /* Pattern: 48 B? 93 00 00 00 (03|05) 00 00 00
             * = mov r?, 0x?00000093 — accept either pristine (03) or
             * previously-patched (05) mode byte. */
            if (buf[i] != 0x48) continue;
            if ((buf[i+1] & 0xf8) != 0xb8) continue;
            if (buf[i+2] != 0x93) continue;
            if (buf[i+3] || buf[i+4] || buf[i+5]) continue;
            if (buf[i+6] != 0x03 && buf[i+6] != 0x05) continue;
            if (buf[i+7] || buf[i+8] || buf[i+9]) continue;
            match_va = eboot + off + i;
            if (out_mode_byte_va) *out_mode_byte_va = match_va + 6;
            goto found;
        }
    }
found:
    free(buf);
    if (!match_va) return 0;
    /* Walk back up to 0x400 bytes for the function prologue. */
    uint8_t back[0x400];
    uint64_t back_start = match_va > 0x400 ? match_va - 0x400 : 0;
    size_t back_len = (size_t)(match_va - back_start);
    if (kernel_proc_copyout(pid, (intptr_t)back_start, back, back_len) != 0) return 0;
    for (ssize_t i = (ssize_t)back_len - (ssize_t)sizeof prologue; i >= 0; i--) {
        if (memcmp(back + i, prologue, sizeof prologue) == 0)
            return back_start + (uint64_t)i;
    }
    return 0;
}

/* Scan for the vtable[0x10] lookup method's body signature:
 *   TZCNT EDX, ESI  (F3 0F BC ??)
 *   SHL   EDX, 0x19 (C1 E? 19)   within 10 bytes
 * Verify each match by walking back for the `MOV RCX, [RIP+disp32]` DAT load
 * (48 8B 0D within 0x40 bytes) — the disp32 gives us the compositor shm-base
 * pointer DAT_013a84a0. We don't need the method VA itself since the vtable
 * hook is skipped in favor of the deterministic formula; we only need DAT.
 *
 * Returns 0 on success and sets *out_dat_va to DAT's VA. */
static int sui_scan_vt10_dat(pid_t pid, uint64_t eboot, uint64_t ebsize,
                              uint64_t *out_dat_va)
{
    if (out_dat_va) *out_dat_va = 0;
    const size_t CHUNK = 0x10000;
    uint8_t *buf = malloc(CHUNK + 16);
    if (!buf) return -1;
    uint64_t scan_end = ebsize ? ebsize : 0x400000;
    for (uint64_t off = 0; off + 10 < scan_end; off += CHUNK - 16) {
        size_t n = CHUNK;
        if (off + n > scan_end) n = (size_t)(scan_end - off);
        if (kernel_proc_copyout(pid, (intptr_t)(eboot + off), buf, n) != 0) continue;
        for (size_t i = 0; i + 10 <= n; i++) {
            if (buf[i] != 0xF3 || buf[i+1] != 0x0F || buf[i+2] != 0xBC) continue;
            /* Within 0..6 bytes after: C1 E? 19 (SHL r32, 0x19) */
            for (size_t j = i + 4; j <= i + 10 && j + 3 <= n; j++) {
                if (buf[j] != 0xC1 || (buf[j+1] & 0xF8) != 0xE0 || buf[j+2] != 0x19)
                    continue;
                uint64_t tzcnt_va = eboot + off + i;
                /* Walk back 0x40 bytes for the DAT load */
                uint8_t back[0x100];
                uint64_t back_start = tzcnt_va > 0x100 ? tzcnt_va - 0x100 : eboot;
                size_t back_len = (size_t)(tzcnt_va - back_start);
                if (kernel_proc_copyout(pid, (intptr_t)back_start, back, back_len) != 0)
                    break;
                ssize_t dat_min = back_len > 0x40 ? (ssize_t)back_len - 0x40 : 0;
                ssize_t prol_min = back_len > 0x80 ? (ssize_t)back_len - 0x80 : 0;
                uint64_t dat_va = 0;
                for (ssize_t k = (ssize_t)back_len - 7; k >= dat_min; k--) {
                    if (back[k] == 0x48 && back[k+1] == 0x8B && back[k+2] == 0x0D) {
                        int32_t disp32;
                        memcpy(&disp32, back + k + 3, 4);
                        uint64_t instr_va = back_start + (uint64_t)k;
                        dat_va = instr_va + 7 + (int64_t)disp32;
                        break;
                    }
                }
                if (!dat_va) break;
                /* Verify prologue is present within 0x80 bytes */
                static const uint8_t prologue[] = {0x55,0x48,0x89,0xE5,0x41,0x56,0x53};
                int have_prologue = 0;
                for (ssize_t k = (ssize_t)back_len - (ssize_t)sizeof prologue;
                     k >= prol_min; k--) {
                    if (memcmp(back + k, prologue, sizeof prologue) == 0) {
                        have_prologue = 1;
                        break;
                    }
                }
                if (!have_prologue) break;
                if (out_dat_va) *out_dat_va = dat_va;
                free(buf);
                return 0;
            }
        }
    }
    free(buf);
    return -1;
}

/* Call sceScreenshotCaptureForCoreDump inside SceShellUI via direct pt_call,
 * patched to compose at 4K. On return, the compositor shm has a fresh 4K BGRA
 * frame at (shm_base + 0x8000000), where shm_base is read from the discovered
 * DAT_013a84a0 pointer and 0x8000000 is Sony's mode-5 offset constant.
 *
 * Firmware-agnostic: all SceShellUI offsets (CoreDump fn, mode byte,
 * DAT_013a84a0) are signature-scanned at runtime. ShellUI is frozen for the
 * full ~150 ms duration of fn_coredump; no pthread_create, no shellcode
 * thread, no vtable hook (pixel VA comes from the deterministic formula).
 *
 * *out_shm_base receives the compositor shm base VA (0x4000000000 on FW 13.20)
 * so the caller can read the composed frame from (shm_base + 0x8000000). */
static int sui_trigger_compose(cJSON *report, uint64_t *out_shm_base)
{
    const pid_t target = sui_find_pid();
    if (target <= 0) {
        cJSON_AddStringToObject(report, "err", "SceShellUI process not found");
        return -1;
    }
    cJSON_AddNumberToObject(report, "sui_pid", target);
    if (!sui_find_eboot(target)) {
        cJSON_AddStringToObject(report, "err", "could not find SceShellUI eboot base");
        return -1;
    }

    /* Per-SceShellUI-PID cache: scanned offsets AND the 4 KB scratch we
     * mmap into SceShellUI for the path string. Reusing scratch avoids
     * leaking 4 KB of VMA in SceShellUI on every capture. On a fresh
     * SceShellUI PID (respawn) the stale scratch is forgotten and a new
     * one is mapped — the old one went with the old process. */
    static pid_t    c_pid         = -1;
    static uint64_t c_eboot       = 0;
    static uint64_t c_fn_coredump = 0;
    static uint64_t c_mode_va     = 0;
    static uint64_t c_dat_va      = 0;
    static intptr_t c_scratch     = 0;
    const int cache_hit = (c_pid == target && c_eboot != 0 &&
                           c_fn_coredump != 0 && c_mode_va != 0 && c_dat_va != 0);
    cJSON_AddBoolToObject(report, "cache_hit", cache_hit);

    uint64_t eboot       = c_eboot;
    uint64_t fn_coredump = c_fn_coredump;
    uint64_t mode_va     = c_mode_va;
    uint64_t dat_va      = c_dat_va;

    if (!cache_hit) {
        /* Scan executable text only (no rodata/data — scanning further
         * destabilizes the compositor, see notes in ps5-screenshot-working-state).
         * The largest executable mapping goes first (the eboot on 13.20), then
         * the other large ones in address order (the eboot on 12.02). */
        uint64_t rst[64], ren[64];
        const int nreg = sui_exec_regions(target, rst, ren, 64);
        if (nreg == 0) {
            cJSON_AddStringToObject(report, "err", "no large executable mapping in SceShellUI");
            return -1;
        }
        int big = 0;
        for (int i = 1; i < nreg; i++)
            if (ren[i] - rst[i] > ren[big] - rst[big]) big = i;
        int found = 0, found_fn = 0;
        for (int k = 0; k < nreg && !found; k++) {
            const int i = k == 0 ? big : (k - 1 < big ? k - 1 : k);
            const uint64_t size = ren[i] - rst[i];
            uint64_t m = 0, d = 0;
            const uint64_t f = sui_scan_coredump_fn(target, rst[i], size, &m);
            if (!f || !m) continue;
            found_fn = 1;
            if (sui_scan_vt10_dat(target, rst[i], size, &d) != 0) continue;
            eboot = rst[i]; fn_coredump = f; mode_va = m; dat_va = d; found = 1;
        }
        if (!found) {
            cJSON_AddStringToObject(report, "err", found_fn ? "DAT_013a84a0 not found"
                                    : "sceScreenshotCaptureForCoreDump not found");
            return -1;
        }
        c_pid = target; c_eboot = eboot;
        c_fn_coredump = fn_coredump; c_mode_va = mode_va; c_dat_va = dat_va;
        c_scratch = 0; /* new PID → previous scratch VMA is gone */
    }
    cJSON_AddNumberToObject(report, "eboot", (double)eboot);
    cJSON_AddNumberToObject(report, "fn_coredump", (double)fn_coredump);
    cJSON_AddNumberToObject(report, "dat_013a84a0_va", (double)dat_va);

    /* Fast-trap pt_call needs a 0xCC byte in ShellUI text to use as the
     * fake return address. Scan once, cache per PID. */
    if (sui_trap_pid != target || !sui_trap_va) {
        sui_trap_va = sui_find_cc_pad(target, eboot);
        if (!sui_trap_va) {
            cJSON_AddStringToObject(report, "err", "no 0xCC trap pad found in ShellUI text");
            return -1;
        }
        sui_trap_pid = target;
    }
    cJSON_AddNumberToObject(report, "trap_va", (double)sui_trap_va);

    /* Read the compositor shm base from the discovered DAT pointer and expose
     * it to the caller. The pixel formula is shm_base + (tz(mode_bitmap) << 25),
     * where mode-5 (bitmap 0x10) gives tz=4 → offset 0x8000000. Both the
     * formula and 0x8000000 come from Sony's own vtable[0x10] method body. */
    uint64_t shm_base = 0;
    kernel_proc_copyout(target, (intptr_t)dat_va, &shm_base, 8);
    cJSON_AddNumberToObject(report, "shm_base", (double)shm_base);
    if (out_shm_base) *out_shm_base = shm_base;
    if (!shm_base) {
        cJSON_AddStringToObject(report, "err", "DAT_013a84a0 is null (compositor not up?)");
        return -1;
    }

    /* Patch mode byte 0x03 -> 0x05 for a 4K compose. kernel_proc_copyin
     * writes directly, no mprotect needed. */
    uint8_t pre_mode = 0, new_mode = 0x05;
    kernel_proc_copyout(target, mode_va, &pre_mode, 1);
    if (pre_mode != 0x03 && pre_mode != 0x05) {
        cJSON_AddNumberToObject(report, "unexpected_mode", (double)pre_mode);
        return -1;
    }
    if (pre_mode != 0x05)
        kernel_proc_copyin(target, &new_mode, mode_va, 1);

    /* Attach, map scratch for the path string, drive fn_coredump directly.
     * No pthread injection — synchronous call under ptrace freeze (~150 ms). */
    if (sui_pt_attach(target)) {
        cJSON_AddNumberToObject(report, "attach_errno", errno);
        cJSON_AddStringToObject(report, "err", "pt_attach SceShellUI failed");
        /* Revert the mode patch if we bailed */
        if (pre_mode != 0x05)
            kernel_proc_copyin(target, &pre_mode, mode_va, 1);
        return -1;
    }

    /* Reuse cached scratch VMA if we have one for this SceShellUI PID. */
    intptr_t scratch = c_scratch;
    if (!scratch) {
        scratch = sui_pt_mmap(target, 0x1000, PROT_READ | PROT_WRITE);
        if (scratch < 0) {
            sui_pt_detach(target);
            if (pre_mode != 0x05)
                kernel_proc_copyin(target, &pre_mode, mode_va, 1);
            return -1;
        }
        c_scratch = scratch;
    }
    /* Path string must be in a sce_coredumps.0 folder to pass CoreDump's
     * internal validatePathPrefix check. The save step fails afterwards
     * (expected: 0x80be0006), but the compose + GPU dispatch has already run. */
    static const char *const path_s =
        "/user/devlog/system/sce_coredumps.0/ps5-mcp.jpg";
    sui_pt_copyin(target, path_s, scratch, strlen(path_s) + 1);

    /* fn_coredump(path, w=1280, h=720, quality=0x80). The dimensions are
     * passed unchanged to pass CoreDump's guard check; the mode-byte patch
     * forces the inner compose to render 3840x2160 regardless. */
    long rc = sui_pt_call(target, fn_coredump, (uint64_t)scratch,
                          0x500, 0x2d0, 0x80, 0, 0);
    cJSON_AddNumberToObject(report, "coredump_rc", (double)rc);

    /* Revert the mode patch so Sony's own CoreDump calls (e.g. real crash
     * dumps) run at 720p as designed. */
    if (pre_mode != 0x05)
        kernel_proc_copyin(target, &pre_mode, mode_va, 1);

    sui_pt_detach(target);

    /* CoreDump's save step writes a partial file to the path before failing
     * at the format check (0x80be0006). Unlink it so we don't accumulate
     * stale JPEGs under /user/devlog/system/sce_coredumps.0/ on every
     * capture. The actual pixel data for our MCP comes from compositor shm,
     * not this file. */
    unlink(path_s);
    return 0;
}

/* In the compositor shm, find the byte offset of the live composited frame.
 * The compositor writes exactly one frame into the 162 MiB area between
 * captures; other bytes are zeros or stale data with much lower opacity, so
 * finding "the opaque run of the right byte count" is enough. Returns the
 * frame-start offset, or -1 if nothing plausible is there. */
static int64_t find_frame_in_shm(pid_t me, size_t frame_bytes, cJSON *report)
{
    const uint64_t SHM_BASE = 0x4000000000ULL;
    const uint64_t SHM_SIZE = 162ULL * 1024 * 1024; /* CompositorShmScreenshotMem */

    /* Walk the shm at page granularity, counting opaque samples. Keep the
     * start-offset of the LONGEST contiguous run of opaque pages — that is
     * the current frame. (A page not entirely opaque — some rows are black
     * on the home screen — is still counted as "in the run" if even a few
     * samples match.) */
    const size_t PAGE = 0x1000; /* 4 KiB */
    const int MIN_HITS = 16;    /* 16 of 1024 pixels sampled per page */
    int64_t best_start = -1, best_len = 0;
    int64_t run_start = -1, run_len = 0;
    uint8_t sample[PAGE];
    for (uint64_t off = 0; off + PAGE <= SHM_SIZE; off += PAGE)
    {
        int hit = 0;
        if (kernel_proc_copyout(me, SHM_BASE + off, sample, PAGE) == 0)
        {
            int opaque = 0;
            for (size_t i = 3; i < PAGE; i += 4)
                if (sample[i] == 0xff)
                    opaque++;
            if (opaque >= MIN_HITS)
                hit = 1;
        }
        if (hit)
        {
            if (run_start < 0)
                run_start = (int64_t)off;
            run_len += (int64_t)PAGE;
        }
        else
        {
            if (run_len > best_len)
            {
                best_len = run_len;
                best_start = run_start;
            }
            run_start = -1;
            run_len = 0;
        }
    }
    if (run_len > best_len)
    {
        best_len = run_len;
        best_start = run_start;
    }
    cJSON_AddNumberToObject(report, "shm_run_start", (double)best_start);
    cJSON_AddNumberToObject(report, "shm_run_bytes", (double)best_len);
    cJSON_AddNumberToObject(report, "shm_frame_bytes", (double)frame_bytes);
    if (best_start < 0 || (uint64_t)best_len < frame_bytes / 2 ||
        (uint64_t)best_start + frame_bytes > SHM_SIZE)
        return -1;
    return best_start;
}

int backend_capture_native(uint8_t **out_bgra, int *out_w, int *out_h, cJSON *report)
{
    *out_bgra = NULL;
    *out_w = 0;
    *out_h = 0;
    elevate();
    const pid_t me = getpid();

#define TICK(label) do { \
    struct timespec _ts; clock_gettime(CLOCK_MONOTONIC, &_ts); \
    cJSON_AddNumberToObject(report, "t_" label, \
        (double)((_ts.tv_sec - t0.tv_sec) * 1000 + \
                 (_ts.tv_nsec - t0.tv_nsec) / 1000000)); \
} while (0)

    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    TICK("start");

    /* 1. Trigger a fresh compose inside SceShellUI. On return (after a brief
     *    ptrace freeze on SceShellUI), the compositor shm has a current
     *    3840×2160 BGRA frame at (shm_base + 0x8000000). The compose base
     *    comes from SceShellUI's own DAT_013a84a0, scanned at runtime. */
    uint64_t trigger_shm_base = 0;
    if (sui_trigger_compose(report, &trigger_shm_base) != 0)
        return -1;
    TICK("trigger_done");

    /* CoreDump composes at 3840×2160 with the mode-5 eboot patch (mode byte
     * 0x03 → 0x05) applied by sui_trigger_compose. The display's own output
     * resolution is irrelevant for the capture. */
    const uint32_t width = 3840, height = 2160;

    /* 2. Elevate to a compositor-eligible client so our own process can map
     *    the compositor shm. */
    kernel_set_ucred_authid(me, 0x480000000000000fULL);
    uint32_t libk = 0;
    kernel_dynlib_handle(me, "libkernel_sys.sprx", &libk);
    if (!libk)
        kernel_dynlib_handle(me, "libkernel.sprx", &libk);
    setinfo_t set_app_info = libk ? (setinfo_t)kernel_dynlib_dlsym(me, libk, "sceKernelSetAppInfo")
                                  : NULL;
    if (set_app_info)
    {
        uint8_t info[0x60] = {0};
        *(uint32_t *)(info + 0x50) = 0x01000000;
        set_app_info(me, info);
    }

    /* 3. Resolve the compositor client API. */
    void *ch = dlopen("libSceComposite.sprx", RTLD_LAZY | RTLD_GLOBAL);
    if (!ch)
        ch = dlopen("/system/priv/lib/libSceComposite.sprx", RTLD_LAZY | RTLD_GLOBAL);
    if (!ch)
    {
        cJSON_AddStringToObject(report, "err", "dlopen libSceComposite failed");
        return -1;
    }
    comp_init_t cinit = (comp_init_t)dlsym(ch, "IUlpGnuoR1c");
    comp_quit_t cquit = (comp_quit_t)dlsym(ch, "y5IJdPxdBrE");
    if (!cinit || !cquit)
    {
        cJSON_AddStringToObject(report, "err", "cannot resolve libSceComposite NIDs");
        return -1;
    }

    TICK("pre_cinit");
    /* 4. Register as a compositor client so OUR process has the shm mapped
     *    at 0x4000000000 and can read the frame CoreDump just wrote. */
    const int ir = cinit(0x200000, 0x200000, 0x200000, 1);
    TICK("post_cinit");
    if (ir != 0)
    {
        cJSON_AddNumberToObject(report, "cinit_err", ir);
        if (ir == (int)0x80d40009)
            cJSON_AddStringToObject(report, "hint",
                                    "all 16 compositor client slots may be used — reboot clears them");
        return -1;
    }

    /* 5. CoreDump's mode-5 4K compose lands at (shm_base + 0x8000000), with
     *    shm_base discovered by scanning the SceShellUI eboot and reading its
     *    own DAT_013a84a0 (compositor shm base pointer). The 0x8000000 offset
     *    is Sony's design constant for mode-5 (tz(0x10) << 25). On FW 13.20
     *    shm_base is 0x4000000000 — but any firmware-specific drift is now
     *    runtime-discovered, not hardcoded. */
    const uint64_t SHM_BASE = trigger_shm_base;
    const uint64_t SHM_FRAME_OFF = 0x8000000ULL;  /* Sony mode-5 constant */
    const size_t frame_bytes = (size_t)width * (size_t)height * 4u;
    uint8_t probe[32] = {0};
    if (kernel_proc_copyout(me, SHM_BASE + SHM_FRAME_OFF, probe, 32) != 0)
    {
        cJSON_AddStringToObject(report, "err", "shm not mapped (cinit silently failed?)");
        cquit();
        return -1;
    }
    int opaque_hits = 0;
    for (int i = 3; i < 32; i += 4)
        if (probe[i] == 0xff) opaque_hits++;
    cJSON_AddNumberToObject(report, "shm_frame_off", (double)SHM_FRAME_OFF);
    cJSON_AddNumberToObject(report, "shm_opaque_hits", opaque_hits);
    if (opaque_hits == 0)
    {
        /* Fall back to scanning if the canonical slot didn't get our frame */
        const int64_t off = find_frame_in_shm(me, frame_bytes, report);
        if (off < 0)
        {
            cJSON_AddStringToObject(report, "err",
                                    "compose ran but no frame found in shm");
            cquit();
            return -1;
        }
        cJSON_AddNumberToObject(report, "shm_frame_off_fallback", (double)off);
        /* Use the found offset. */
        uint8_t *frame2 = malloc(frame_bytes);
        if (!frame2) { cquit(); cJSON_AddStringToObject(report, "err", "oom"); return -1; }
        const size_t CHUNK2 = 0x100000;
        for (size_t o = 0; o < frame_bytes; o += CHUNK2) {
            const size_t c = frame_bytes - o > CHUNK2 ? CHUNK2 : frame_bytes - o;
            if (kernel_proc_copyout(me, SHM_BASE + (uint64_t)off + o, frame2 + o, c) != 0) {
                free(frame2); cquit();
                cJSON_AddStringToObject(report, "err", "copyout failed (fallback)");
                return -1;
            }
        }
        cquit();
        *out_bgra = frame2;
        *out_w = (int)width;
        *out_h = (int)height;
        return 0;
    }

    /* 6. Copy the 33 MB frame out of SceShellUI's address space in 1 MB chunks
     *    (kernel_proc_copyout has issues with larger single transfers). */
    uint8_t *frame = malloc(frame_bytes);
    if (frame == NULL)
    {
        cJSON_AddStringToObject(report, "err", "oom allocating the frame buffer");
        cquit();
        return -1;
    }
    const size_t CHUNK = 0x100000;
    int cp_err = 0;
    for (size_t o = 0; o < frame_bytes; o += CHUNK)
    {
        const size_t c = frame_bytes - o > CHUNK ? CHUNK : frame_bytes - o;
        if (kernel_proc_copyout(me, SHM_BASE + SHM_FRAME_OFF + o, frame + o, c) != 0)
        {
            cp_err = 1;
            break;
        }
    }
    TICK("post_copyout");
    cquit();
    TICK("post_cquit");
    /* Write the timing report to disk so we can inspect it even on success. */
    {
        char *dump = cJSON_PrintUnformatted(report);
        if (dump) {
            FILE *df = fopen("/data/ps5-mcp/native_timing.json", "w");
            if (df) { fputs(dump, df); fclose(df); }
            free(dump);
        }
    }
    if (cp_err)
    {
        free(frame);
        cJSON_AddStringToObject(report, "err", "copyout of the frame failed");
        return -1;
    }
    *out_bgra = frame;
    *out_w = (int)width;
    *out_h = (int)height;
    return 0;
}


/* ---- processes ---------------------------------------------------------- */

static const char *state_name(int stat)
{
    static const char *const names[] = {"?", "IDL", "RUN", "SLP", "STOP", "ZOMB", "WAIT", "LOCK"};
    return stat >= 0 && stat < 8 ? names[stat] : "?";
}

static void *sysctl_alloc(int *mib, unsigned len, size_t *size)
{
    for (int attempt = 0; attempt < 4; attempt++)
    {
        if (sysctl(mib, len, NULL, size, NULL, 0) != 0)
            return NULL;
        *size += *size / 4 + 4096;
        void *buf = malloc(*size);
        if (buf == NULL)
            return NULL;
        if (sysctl(mib, len, buf, size, NULL, 0) == 0)
            return buf;
        free(buf);
        if (errno != ENOMEM)
            return NULL;
    }
    return NULL;
}

int backend_proc_list(proc_entry_t **out)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    uint8_t *buf = sysctl_alloc(mib, 4, &size);
    if (buf == NULL)
        return -errno;
    size_t cap = 128, n = 0;
    proc_entry_t *list = calloc(cap, sizeof(*list));
    for (size_t off = 0; list && off + sizeof(struct kinfo_proc) <= size;)
    {
        const struct kinfo_proc *ki = (const struct kinfo_proc *)(buf + off);
        if (ki->ki_structsize <= 0)
            break;
        off += (size_t)ki->ki_structsize;
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
        p->pid = ki->ki_pid;
        p->ppid = ki->ki_ppid;
        p->uid = ki->ki_uid;
        snprintf(p->state, sizeof(p->state), "%s", state_name(ki->ki_stat));
        snprintf(p->name, sizeof(p->name), "%s", ki->ki_comm);
        p->rss_bytes = (uint64_t)ki->ki_rssize * PAGE_SIZE;
        p->vsize_bytes = (uint64_t)ki->ki_size;
        p->threads = ki->ki_numthreads;
        p->cpu_percent = 100.0 * (double)ki->ki_pctcpu / FSCALE;
        p->authid = kernel_get_ucred_authid(ki->ki_pid);
        app_info_t info;
        memset(&info, 0, sizeof(info));
        if (sceKernelGetAppInfo(ki->ki_pid, &info) == 0 && info.title_id[0] != '\0')
        {
            snprintf(p->title_id, sizeof(p->title_id), "%.9s", info.title_id);
            p->app_id = info.app_id;
        }
    }
    free(buf);
    if (list == NULL)
        return -ENOMEM;
    *out = list;
    return (int)n;
}

int backend_proc_detail(pid_t pid, cJSON *obj, bool threads, bool vmmap, bool files)
{
    if (threads)
    {
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID | KERN_PROC_INC_THREAD, (int)pid};
        size_t size = 0;
        uint8_t *buf = sysctl_alloc(mib, 4, &size);
        cJSON *arr = cJSON_AddArrayToObject(obj, "threads");
        for (size_t off = 0; buf && off + sizeof(struct kinfo_proc) <= size;)
        {
            const struct kinfo_proc *ki = (const struct kinfo_proc *)(buf + off);
            if (ki->ki_structsize <= 0)
                break;
            off += (size_t)ki->ki_structsize;
            cJSON *t = cJSON_CreateObject();
            cJSON_AddNumberToObject(t, "tid", ki->ki_tid);
            cJSON_AddStringToObject(t, "name", ki->ki_tdname);
            cJSON_AddStringToObject(t, "state", state_name(ki->ki_stat));
            cJSON_AddNumberToObject(t, "cpu_time_s", (double)ki->ki_runtime / 1e6);
            cJSON_AddNumberToObject(t, "cpu_percent", 100.0 * (double)ki->ki_pctcpu / FSCALE);
            cJSON_AddItemToArray(arr, t);
        }
        if (buf == NULL)
            cJSON_AddStringToObject(obj, "threads_error", strerror(errno));
        free(buf);
    }
    if (vmmap)
    {
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, (int)pid};
        size_t size = 0;
        uint8_t *buf = sysctl_alloc(mib, 4, &size);
        cJSON *arr = cJSON_AddArrayToObject(obj, "memory_map");
        int shown = 0;
        for (size_t off = 0; buf && off < size && shown < 1000;)
        {
            const struct kinfo_vmentry *kv = (const struct kinfo_vmentry *)(buf + off);
            if (kv->kve_structsize <= 0)
                break;
            off += (size_t)kv->kve_structsize;
            char line[PATH_MAX + 96];
            snprintf(line, sizeof(line), "%012llx-%012llx %c%c%c %s",
                     (unsigned long long)kv->kve_start, (unsigned long long)kv->kve_end,
                     kv->kve_protection & KVME_PROT_READ ? 'r' : '-',
                     kv->kve_protection & KVME_PROT_WRITE ? 'w' : '-',
                     kv->kve_protection & KVME_PROT_EXEC ? 'x' : '-', kv->kve_path);
            cJSON_AddItemToArray(arr, cJSON_CreateString(line));
            shown++;
        }
        if (buf == NULL)
            cJSON_AddStringToObject(obj, "memory_map_error", strerror(errno));
        free(buf);
    }
    if (files)
    {
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_FILEDESC, (int)pid};
        size_t size = 0;
        uint8_t *buf = sysctl_alloc(mib, 4, &size);
        cJSON *arr = cJSON_AddArrayToObject(obj, "files");
        for (size_t off = 0; buf && off < size;)
        {
            const struct kinfo_file *kf = (const struct kinfo_file *)(buf + off);
            if (kf->kf_structsize <= 0)
                break;
            off += (size_t)kf->kf_structsize;
            char line[PATH_MAX + 32];
            snprintf(line, sizeof(line), "fd %d type %d %s", kf->kf_fd, kf->kf_type, kf->kf_path);
            cJSON_AddItemToArray(arr, cJSON_CreateString(line));
        }
        if (buf == NULL)
            cJSON_AddStringToObject(obj, "files_error", strerror(errno));
        free(buf);
    }
    return 0;
}

/* ---- system ------------------------------------------------------------- */

static bool find_title(int app_id, char *title_id, size_t len)
{
    proc_entry_t *list = NULL;
    const int n = backend_proc_list(&list);
    bool found = false;
    for (int i = 0; i < n && !found; i++)
        if ((int)list[i].app_id == app_id && list[i].title_id[0])
        {
            snprintf(title_id, len, "%s", list[i].title_id);
            found = true;
        }
    free(list);
    return found;
}

void backend_system_info(cJSON *obj)
{
    cJSON *c = cJSON_AddObjectToObject(obj, "console");
    const uint32_t fw = kernel_get_fw_version();
    char text[64];
    snprintf(text, sizeof(text), "%x.%02x", fw >> 24, (fw >> 16) & 0xff);
    cJSON_AddStringToObject(c, "firmware", text);
    report_hex(c, "firmware_raw", fw);
    char model[256] = "";
    if (sceKernelGetHwModelName(model) == 0)
        cJSON_AddStringToObject(c, "model", model);
    int t = 0;
    if (sceKernelGetCpuTemperature(&t) == 0)
        cJSON_AddNumberToObject(c, "cpu_temp_c", t);
    if (sceKernelGetSocSensorTemperature(0, &t) == 0)
        cJSON_AddNumberToObject(c, "soc_temp_c", t);
    cJSON_AddNumberToObject(c, "cpu_mhz", (double)sceKernelGetCpuFrequency() / 1e6);

    /* hw.physmem is refused by the console's sysctl policy ("not approved",
     * logged to klog), so it is not asked. */
    size_t len;
    struct timeval boot;
    len = sizeof(boot);
    if (sysctlbyname("kern.boottime", &boot, &len, NULL, 0) == 0)
        cJSON_AddNumberToObject(c, "uptime_s", (double)(time(NULL) - boot.tv_sec));

    int user = -1;
    if (sceUserServiceGetForegroundUser(&user) == 0)
    {
        cJSON *u = cJSON_AddObjectToObject(obj, "foreground_user");
        report_hex(u, "id", (uint32_t)user);
        char name[64] = "";
        if (sceUserServiceGetUserName(user, name, sizeof(name)) == 0)
            cJSON_AddStringToObject(u, "name", name);
    }
    app_running_t app;
    if (backend_app_running(&app) == 0)
    {
        cJSON *a = cJSON_AddObjectToObject(obj, "running_app");
        report_hex(a, "app_id", (uint32_t)app.app_id);
        cJSON_AddStringToObject(a, "title_id", app.title_id);
    }
    else
        cJSON_AddNullToObject(obj, "running_app");
}

int backend_notify(const char *message)
{
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", message);
    return sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* ---- applications ------------------------------------------------------- */

int backend_app_running(app_running_t *out)
{
    memset(out, 0, sizeof(*out));
    const int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
    if (app_id <= 0)
        return -1;
    out->app_id = app_id;
    if (!find_title(app_id, out->title_id, sizeof(out->title_id)))
        snprintf(out->title_id, sizeof(out->title_id), "?");
    return 0;
}

int backend_app_launch(const char *title_id, const char *args, cJSON *report)
{
    app_launch_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int user = -1;
    const int fg = sceUserServiceGetForegroundUser(&user);
    report_hex(report, "get_foreground_user", (uint32_t)fg);
    report_hex(report, "user", (uint32_t)user);
    if (fg != 0)
        return -1;
    ctx.user_id = (uint32_t)user;

    const int running = sceSystemServiceGetAppIdOfRunningBigApp();
    if (running > 0)
        report_hex(report, "kill_running",
                   (uint32_t)sceSystemServiceKillApp(running, -1, 0, 0));

    /* Split the arguments on spaces (no quoting). */
    char *copy = strdup(args ? args : "");
    char *argv[64];
    int argc = 0;
    char *save = NULL;
    for (char *tok = copy ? strtok_r(copy, " ", &save) : NULL; tok && argc < 63;
         tok = strtok_r(NULL, " ", &save))
        argv[argc++] = tok;
    argv[argc] = NULL;
    const int r = sceSystemServiceLaunchApp(title_id, argv, &ctx);
    free(copy);
    report_hex(report, "launch", (uint32_t)r);
    return r;
}

int backend_app_kill(int app_id, cJSON *report)
{
    const int r = sceSystemServiceKillApp(app_id, -1, 0, 0);
    report_hex(report, "kill", (uint32_t)r);
    return r;
}

/* ---- klog --------------------------------------------------------------- */

int backend_klog_open(const char **what)
{
    /* The klog server, when one runs: /dev/klog has a single reader, and the
     * server is it. */
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0)
    {
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(KLOG_SERVER_PORT);
        if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0)
        {
            *what = "klog server 127.0.0.1:3232";
            return s;
        }
        close(s);
    }
    const int fd = open("/dev/klog", O_RDONLY | O_NONBLOCK);
    if (fd >= 0)
    {
        *what = "/dev/klog";
        return fd;
    }
    *what = "none";
    return -1;
}
