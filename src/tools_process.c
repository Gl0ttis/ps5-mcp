/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "backend.h"
#include "mcp.h"
#include "util.h"

/* qsort has no context argument; one per thread keeps concurrent calls apart. */
static _Thread_local const char *g_sort_key;

static int compare(const void *a, const void *b)
{
    const proc_entry_t *x = a, *y = b;
    if (strcmp(g_sort_key, "memory") == 0)
        return x->rss_bytes < y->rss_bytes ? 1 : x->rss_bytes > y->rss_bytes ? -1 : 0;
    if (strcmp(g_sort_key, "cpu") == 0)
        return x->cpu_percent < y->cpu_percent ? 1 : x->cpu_percent > y->cpu_percent ? -1 : 0;
    if (strcmp(g_sort_key, "name") == 0)
        return strcasecmp(x->name, y->name);
    return x->pid - y->pid;
}

static bool matches(const proc_entry_t *p, const char *filter)
{
    if (filter == NULL || filter[0] == '\0')
        return true;
    char pid[16];
    snprintf(pid, sizeof(pid), "%d", (int)p->pid);
    return strcasestr(p->name, filter) != NULL || strcasestr(p->title_id, filter) != NULL ||
           strcmp(pid, filter) == 0;
}

static void tool_list_processes(const cJSON *args, tool_result_t *r)
{
    proc_entry_t *list = NULL;
    const int n = backend_proc_list(&list);
    if (n < 0)
    {
        tr_error(r, "cannot list processes: %s", strerror(-n));
        return;
    }
    const char *filter = arg_str(args, "filter", NULL);
    const bool apps_only = arg_bool(args, "apps_only", false);
    const int limit = CLAMP(arg_int(args, "limit", 400), 1, 5000);
    g_sort_key = arg_str(args, "sort", "pid");
    qsort(list, (size_t)n, sizeof(*list), compare);

    if (strcmp(arg_str(args, "format", "table"), "json") == 0)
    {
        cJSON *obj = cJSON_CreateObject();
        cJSON *arr = cJSON_AddArrayToObject(obj, "processes");
        int shown = 0;
        for (int i = 0; i < n && shown < limit; i++)
        {
            const proc_entry_t *p = &list[i];
            if (!matches(p, filter) || (apps_only && p->title_id[0] == '\0'))
                continue;
            cJSON *e = cJSON_CreateObject();
            char hex[24];
            cJSON_AddNumberToObject(e, "pid", p->pid);
            cJSON_AddNumberToObject(e, "ppid", p->ppid);
            cJSON_AddStringToObject(e, "name", p->name);
            cJSON_AddStringToObject(e, "state", p->state);
            cJSON_AddNumberToObject(e, "uid", p->uid);
            snprintf(hex, sizeof(hex), "0x%016llx", (unsigned long long)p->authid);
            cJSON_AddStringToObject(e, "authid", hex);
            if (p->title_id[0])
            {
                cJSON_AddStringToObject(e, "title_id", p->title_id);
                snprintf(hex, sizeof(hex), "0x%08x", p->app_id);
                cJSON_AddStringToObject(e, "app_id", hex);
            }
            cJSON_AddNumberToObject(e, "rss_mib", (double)p->rss_bytes / (1 << 20));
            cJSON_AddNumberToObject(e, "vsize_mib", (double)p->vsize_bytes / (1 << 20));
            cJSON_AddNumberToObject(e, "threads", p->threads);
            cJSON_AddNumberToObject(e, "cpu_percent", p->cpu_percent);
            cJSON_AddItemToArray(arr, e);
            shown++;
        }
        cJSON_AddNumberToObject(obj, "total", n);
        tr_json(r, obj);
        free(list);
        return;
    }

    buf_t b;
    buf_init(&b);
    buf_printf(&b, "%6s %6s %-5s %4s %6s %-18s %-10s %9s %9s  %s\n", "PID", "PPID", "STATE",
               "THR", "CPU%", "AUTHID", "TITLE", "RSS(MiB)", "VSZ(MiB)", "NAME");
    int shown = 0;
    for (int i = 0; i < n && shown < limit; i++)
    {
        const proc_entry_t *p = &list[i];
        if (!matches(p, filter) || (apps_only && p->title_id[0] == '\0'))
            continue;
        buf_printf(&b, "%6d %6d %-5s %4d %6.1f %016llx   %-10s %9.1f %9.1f  %s\n", (int)p->pid,
                   (int)p->ppid, p->state, p->threads, p->cpu_percent,
                   (unsigned long long)p->authid, p->title_id[0] ? p->title_id : "-",
                   (double)p->rss_bytes / (1 << 20), (double)p->vsize_bytes / (1 << 20),
                   p->name);
        shown++;
    }
    buf_printf(&b, "%d of %d processes\n", shown, n);
    tr_text(r, "%s", b.data);
    buf_free(&b);
    free(list);
}

static void tool_process_info(const cJSON *args, tool_result_t *r)
{
    const int pid = arg_int(args, "pid", -1);
    const char *name = arg_str(args, "name", NULL);
    proc_entry_t *list = NULL;
    const int n = backend_proc_list(&list);
    if (n < 0)
    {
        tr_error(r, "cannot list processes: %s", strerror(-n));
        return;
    }
    const proc_entry_t *p = NULL;
    for (int i = 0; i < n && p == NULL; i++)
        if ((pid >= 0 && list[i].pid == pid) ||
            (pid < 0 && name && strcasecmp(list[i].name, name) == 0))
            p = &list[i];
    if (p == NULL)
    {
        tr_error(r, "no such process (give pid, or name exactly as list_processes shows it)");
        free(list);
        return;
    }
    cJSON *obj = cJSON_CreateObject();
    char hex[24];
    cJSON_AddNumberToObject(obj, "pid", p->pid);
    cJSON_AddNumberToObject(obj, "ppid", p->ppid);
    cJSON_AddStringToObject(obj, "name", p->name);
    cJSON_AddStringToObject(obj, "state", p->state);
    cJSON_AddNumberToObject(obj, "uid", p->uid);
    snprintf(hex, sizeof(hex), "0x%016llx", (unsigned long long)p->authid);
    cJSON_AddStringToObject(obj, "authid", hex);
    if (p->title_id[0])
        cJSON_AddStringToObject(obj, "title_id", p->title_id);
    cJSON_AddNumberToObject(obj, "rss_mib", (double)p->rss_bytes / (1 << 20));
    cJSON_AddNumberToObject(obj, "vsize_mib", (double)p->vsize_bytes / (1 << 20));
    cJSON_AddNumberToObject(obj, "cpu_percent", p->cpu_percent);
    const pid_t target = p->pid;
    free(list);
    const int rc = backend_proc_detail(target, obj, arg_bool(args, "threads", true),
                                       arg_bool(args, "memory_map", false),
                                       arg_bool(args, "files", false));
    if (rc != 0)
        cJSON_AddStringToObject(obj, "detail_error", strerror(-rc));
    tr_json(r, obj);
}

static void tool_kill_process(const cJSON *args, tool_result_t *r)
{
    const int pid = arg_int(args, "pid", -1);
    const char *sig_name = arg_str(args, "signal", "TERM");
    static const struct
    {
        const char *name;
        int sig;
    } sigs[] = {{"TERM", SIGTERM}, {"KILL", SIGKILL}, {"INT", SIGINT}, {"HUP", SIGHUP},
                {"STOP", SIGSTOP}, {"CONT", SIGCONT}, {"ABRT", SIGABRT}, {"USR1", SIGUSR1},
                {"USR2", SIGUSR2}};
    int sig = -1;
    for (size_t i = 0; i < ARRAY_LEN(sigs); i++)
        if (strcasecmp(sig_name, sigs[i].name) == 0 ||
            (strncasecmp(sig_name, "SIG", 3) == 0 && strcasecmp(sig_name + 3, sigs[i].name) == 0))
            sig = sigs[i].sig;
    if (sig < 0)
    {
        tr_error(r, "signal is one of TERM, KILL, INT, HUP, STOP, CONT, ABRT, USR1, USR2");
        return;
    }
    if (pid <= 1)
    {
        tr_error(r, "pid must be a real process id (> 1)");
        return;
    }
    if (pid == (int)getpid())
    {
        tr_error(r, "that is this server; use server_shutdown");
        return;
    }
    /* Refuse system daemons unless forced: killing one can reboot the console. */
    proc_entry_t *list = NULL;
    const int n = backend_proc_list(&list);
    const proc_entry_t *p = NULL;
    for (int i = 0; i < n; i++)
        if (list[i].pid == pid)
            p = &list[i];
    if (p == NULL)
    {
        free(list);
        tr_error(r, "no process %d", pid);
        return;
    }
    char name[40];
    snprintf(name, sizeof(name), "%s", p->name);
    const bool system = strncmp(name, "Sce", 3) == 0 || strncmp(name, "sce", 3) == 0 ||
                        strcmp(name, "init") == 0 || strcmp(name, "kernel") == 0;
    free(list);
    if (system && !arg_bool(args, "force", false))
    {
        tr_error(r, "%s (%d) looks like a system process; killing it may crash or reboot the "
                    "console. Pass force=true if you mean it.", name, pid);
        return;
    }
    if (kill(pid, sig) != 0)
    {
        tr_error(r, "kill(%d, %s) failed: %s", pid, sig_name, strerror(errno));
        return;
    }
    tr_text(r, "sent SIG%s to %s (%d)", sig_name, name, pid);
}

static const tool_def_t k_tools[] = {
    {
        "list_processes",
        "List processes",
        "Every process on the console: pid, parent, state, threads, CPU, authid, title id (for "
        "applications), resident and virtual memory, name. Filter by name, title id or pid; "
        "sort by pid, name, memory or cpu.",
        "{'type':'object','properties':{"
        "'filter':{'type':'string','description':'Substring of the name or title id, or a pid.'},"
        "'apps_only':{'type':'boolean','default':false,'description':'Only processes with a "
        "title id.'},"
        "'sort':{'type':'string','enum':['pid','name','memory','cpu'],'default':'pid'},"
        "'limit':{'type':'integer','default':400},"
        "'format':{'type':'string','enum':['table','json'],'default':'table'}}}",
        TOOL_READ_ONLY | TOOL_IDEMPOTENT,
        tool_list_processes,
    },
    {
        "process_info",
        "Process details",
        "Details of one process: its threads (name, state, CPU time), and optionally its "
        "memory map and open files.",
        "{'type':'object','properties':{"
        "'pid':{'type':'integer'},'name':{'type':'string','description':'Exact process name, "
        "when pid is not given.'},"
        "'threads':{'type':'boolean','default':true},"
        "'memory_map':{'type':'boolean','default':false},"
        "'files':{'type':'boolean','default':false}}}",
        TOOL_READ_ONLY | TOOL_IDEMPOTENT,
        tool_process_info,
    },
    {
        "kill_process",
        "Signal a process",
        "Send a signal to a process (default TERM), e.g. to end a hung homebrew. System "
        "processes (Sce*) are refused unless force=true. To end a title prefer app_kill.",
        "{'type':'object','properties':{'pid':{'type':'integer'},"
        "'signal':{'type':'string','default':'TERM','enum':['TERM','KILL','INT','HUP','STOP',"
        "'CONT','ABRT','USR1','USR2']},"
        "'force':{'type':'boolean','default':false}},'required':['pid']}",
        TOOL_DESTRUCTIVE,
        tool_kill_process,
    },
};

void tools_process_register(void)
{
    mcp_register(k_tools, ARRAY_LEN(k_tools));
}
