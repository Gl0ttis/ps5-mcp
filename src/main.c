/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A resident payload that is itself a Model Context Protocol server
 * (Streamable HTTP, POST /mcp, no auth), so an agent can drive and inspect
 * the console with no bridge on the host: a virtual DualSense, screenshots,
 * processes, the kernel log, files and titles. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backend.h"
#include "http.h"
#include "klog.h"
#include "mcp.h"
#include "pad.h"
#include "util.h"

uint64_t g_started_ms;

static void on_sigusr1(int sig)
{
    (void)sig;
    http_kick_listener();
}

int main(int argc, char *argv[])
{
    unsigned port = PS5MCP_DEFAULT_PORT;
    const char *env = getenv("PS5MCP_PORT");
    if (argc > 1)
        env = argv[1];
    if (env != NULL)
    {
        const unsigned long p = strtoul(env, NULL, 10);
        if (p > 0 && p < 65536)
            port = (unsigned)p;
    }
    signal(SIGPIPE, SIG_IGN);
    /* kill -USR1 rebuilds the listener, as after a network stack reset. */
    signal(SIGUSR1, on_sigusr1);
    g_started_ms = now_ms();

    const int init = backend_init();
    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/ps5-mcp.log", backend_data_dir());
    log_set_file(log_path);
    log_msg("%s %s starting, pid %d, backend init %d", PS5MCP_NAME, PS5MCP_VERSION, (int)getpid(),
            init);

    tools_pad_register();
    tools_screen_register();
    tools_system_register();
    tools_process_register();
    tools_klog_register();
    tools_fs_register();
    tools_app_register();
    tools_elf_register();

    klog_start();

    const int rc = http_serve((uint16_t)port, mcp_handle_http);

    cJSON *report = cJSON_CreateObject();
    pad_close(report);
    cJSON_Delete(report);
    log_msg("leaving (%d)", rc);
    return rc == 0 ? 0 : 1;
}
