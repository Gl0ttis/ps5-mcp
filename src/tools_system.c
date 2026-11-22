/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/statvfs.h>

#include "backend.h"
#include "http.h"
#include "klog.h"
#include "mcp.h"
#include "pad.h"
#include "util.h"

extern uint64_t g_started_ms;

static void add_storage(cJSON *obj)
{
    static const char *const paths[] = {"/data", "/user", "/system_data", "/mnt/usb0",
                                        "/mnt/usb1", "/mnt/ext0", "/"};
    cJSON *arr = cJSON_AddArrayToObject(obj, "storage");
    for (size_t i = 0; i < ARRAY_LEN(paths); i++)
    {
        struct statvfs st;
        if (statvfs(paths[i], &st) != 0)
            continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "path", paths[i]);
        const double unit = (double)st.f_frsize;
        cJSON_AddNumberToObject(e, "total_gib", (double)st.f_blocks * unit / (1 << 30));
        cJSON_AddNumberToObject(e, "free_gib", (double)st.f_bavail * unit / (1 << 30));
        cJSON_AddItemToArray(arr, e);
    }
}

static void add_network(cJSON *obj)
{
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0)
        return;
    cJSON *arr = cJSON_AddArrayToObject(obj, "ipv4");
    for (struct ifaddrs *i = list; i; i = i->ifa_next)
    {
        if (i->ifa_addr == NULL || i->ifa_addr->sa_family != AF_INET)
            continue;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof(ip));
        if (strncmp(ip, "127.", 4) == 0)
            continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "interface", i->ifa_name);
        cJSON_AddStringToObject(e, "address", ip);
        cJSON_AddItemToArray(arr, e);
    }
    freeifaddrs(list);
}

static void tool_system_info(const cJSON *args, tool_result_t *r)
{
    (void)args;
    cJSON *obj = cJSON_CreateObject();
    cJSON *server = cJSON_AddObjectToObject(obj, "server");
    cJSON_AddStringToObject(server, "name", PS5MCP_NAME);
    cJSON_AddStringToObject(server, "version", PS5MCP_VERSION);
    cJSON_AddNumberToObject(server, "uptime_s", (double)(now_ms() - g_started_ms) / 1000.0);
    cJSON_AddStringToObject(server, "data_dir", backend_data_dir());
    backend_system_info(obj);
    add_network(obj);
    add_storage(obj);
    pad_status(cJSON_AddObjectToObject(obj, "pad"));
    klog_status(cJSON_AddObjectToObject(obj, "klog"));
    tr_json(r, obj);
}

static void tool_notify(const cJSON *args, tool_result_t *r)
{
    const char *message = arg_str(args, "message", NULL);
    if (message == NULL || message[0] == '\0')
    {
        tr_error(r, "message is required");
        return;
    }
    const int rc = backend_notify(message);
    if (rc != 0)
        tr_error(r, "notification failed: 0x%08x", (unsigned)rc);
    else
        tr_text(r, "notification shown");
}

static volatile bool g_shutdown_requested;

bool shutdown_requested(void)
{
    return g_shutdown_requested;
}

static void tool_server_shutdown(const cJSON *args, tool_result_t *r)
{
    if (!arg_bool(args, "confirm", false))
    {
        tr_error(r, "pass confirm=true: the server leaves the console and must be sent again "
                    "through the ELF loader to come back");
        return;
    }
    g_shutdown_requested = true;
    http_stop();
    tr_text(r, "shutting down: the pad is closed and the payload exits after this reply");
}

static const tool_def_t k_tools[] = {
    {
        "system_info",
        "System info",
        "Firmware, console model, temperatures, CPU clock, memory, the foreground user, the "
        "running title, IP addresses, free storage, and the state of this server (pad, klog "
        "reader, uptime).",
        "{'type':'object','properties':{}}",
        TOOL_READ_ONLY | TOOL_IDEMPOTENT,
        tool_system_info,
    },
    {
        "notify",
        "Show a notification",
        "Show a toast notification on the console screen, e.g. to tell the person at the TV "
        "what the agent is doing.",
        "{'type':'object','properties':{'message':{'type':'string','maxLength':1000}},"
        "'required':['message']}",
        0,
        tool_notify,
    },
    {
        "server_shutdown",
        "Stop this server",
        "Close the virtual pad and end the ps5-mcp payload (e.g. before sending a new build). "
        "It does not come back until sent through the ELF loader again.",
        "{'type':'object','properties':{'confirm':{'type':'boolean'}},'required':['confirm']}",
        TOOL_DESTRUCTIVE,
        tool_server_shutdown,
    },
};

void tools_system_register(void)
{
    mcp_register(k_tools, ARRAY_LEN(k_tools));
}
