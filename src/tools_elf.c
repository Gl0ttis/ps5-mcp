/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Start ELF payloads through the console's own ELF loader (ps5-payload-dev
 * elfldr or compatible, port 9021). The loader reads exactly the ELF's size
 * from the socket, then hands that socket to the new payload as its stdin,
 * stdout and stderr, so whatever the payload prints comes back here. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "backend.h"
#include "mcp.h"
#include "util.h"

#define ELF_MAX (16u * 1024u * 1024u)
#define OUTPUT_MAX (64 * 1024)

static int b64_value(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

static uint8_t *base64_decode(const char *in, size_t *out_len)
{
    const size_t n = strlen(in);
    uint8_t *out = malloc(n / 4 * 3 + 3);
    if (out == NULL)
        return NULL;
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++)
    {
        const int v = b64_value((unsigned char)in[i]);
        if (v < 0)
            continue; /* padding, whitespace */
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    *out_len = o;
    return out;
}

static bool send_all(int fd, const uint8_t *p, size_t len)
{
    while (len > 0)
    {
        const ssize_t n = send(fd, p, len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static void tool_elf_run(const cJSON *args, tool_result_t *r)
{
    const char *path = arg_str(args, "path", NULL);
    const char *b64 = arg_str(args, "data_base64", NULL);
    if ((path == NULL) == (b64 == NULL))
    {
        tr_error(r, "give exactly one of path (an ELF on the console) or data_base64 (the ELF "
                    "itself)");
        return;
    }
    uint8_t *elf = NULL;
    size_t len = 0;
    if (path)
    {
        const int rc = read_file(path, &elf, &len, ELF_MAX);
        if (rc != 0)
        {
            tr_error(r, "%s: %s", path, strerror(-rc));
            return;
        }
    }
    else if ((elf = base64_decode(b64, &len)) == NULL)
    {
        tr_error(r, "out of memory decoding data_base64");
        return;
    }
    if (len < 64 || memcmp(elf, "\x7f" "ELF", 4) != 0)
    {
        free(elf);
        tr_error(r, "that is not an ELF file (%zu bytes)", len);
        return;
    }

    const int port = CLAMP(arg_int(args, "loader_port", 9021), 1, 65535);
    const int capture_ms = CLAMP(arg_int(args, "capture_ms", 3000), 0, 60000);
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0)
    {
        tr_error(r, "no ELF loader on 127.0.0.1:%d (%s). Start one (e.g. elfldr) or pass "
                    "loader_port.", port, strerror(errno));
        if (fd >= 0)
            close(fd);
        free(elf);
        return;
    }
    struct timeval tv = {.tv_sec = 15};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Processes before, to name what the payload started. */
    proc_entry_t *before = NULL;
    const int before_n = backend_proc_list(&before);

    const bool sent = send_all(fd, elf, len);
    free(elf);
    if (!sent)
    {
        close(fd);
        free(before);
        tr_error(r, "the loader closed the connection while the ELF was sent (%s)",
                 strerror(errno));
        return;
    }
    log_msg("elf_run: sent %zu bytes to the loader on port %d", len, port);

    /* The payload's output, until it closes stdout or capture_ms passes. */
    buf_t out;
    buf_init(&out);
    bool closed = false;
    const uint64_t deadline = now_ms() + (uint64_t)capture_ms;
    while (out.len < OUTPUT_MAX)
    {
        const uint64_t now = now_ms();
        if (now >= deadline)
            break;
        struct pollfd p = {.fd = fd, .events = POLLIN};
        const int ready = poll(&p, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            break;
        char chunk[4096];
        const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            closed = true;
            break;
        }
        buf_append(&out, chunk, (size_t)n);
    }
    close(fd);

    cJSON *report = cJSON_CreateObject();
    cJSON_AddNumberToObject(report, "bytes_sent", (double)len);
    cJSON_AddNumberToObject(report, "loader_port", port);
    cJSON_AddBoolToObject(report, "output_closed", closed);
    /* New processes since the send. */
    proc_entry_t *after = NULL;
    const int after_n = backend_proc_list(&after);
    cJSON *started = cJSON_AddArrayToObject(report, "new_processes");
    for (int i = 0; i < after_n; i++)
    {
        bool known = false;
        for (int j = 0; j < before_n && !known; j++)
            known = before[j].pid == after[i].pid;
        if (known)
            continue;
        char text[80];
        snprintf(text, sizeof(text), "%d %s", (int)after[i].pid, after[i].name);
        cJSON_AddItemToArray(started, cJSON_CreateString(text));
    }
    free(before);
    free(after);

    tr_text(r, "sent %zu bytes to the ELF loader. Output (%zu bytes, %s):\n%s", len, out.len,
            closed ? "the payload closed its output" : "still running when capture ended",
            out.len ? out.data : "(none)");
    tr_json(r, report);
    buf_free(&out);
}

static const tool_def_t k_tools[] = {
    {
        "elf_run",
        "Run an ELF payload",
        "Start an ELF payload through the console ELF loader on 127.0.0.1 (port 9021 by "
        "default), from a file on the console or uploaded as base64 (up to 16 MiB), and return "
        "what it prints during capture_ms. The payload keeps running after the call; its "
        "stdout is the loader socket, which closes when the call returns, so long-running "
        "payloads should log to klog (see klog_read).",
        "{'type':'object','properties':{"
        "'path':{'type':'string','description':'ELF on the console, e.g. /data/payloads/x.elf.'},"
        "'data_base64':{'type':'string','description':'The ELF itself, base64.'},"
        "'loader_port':{'type':'integer','default':9021},"
        "'capture_ms':{'type':'integer','default':3000,'minimum':0,'maximum':60000,"
        "'description':'How long to collect the payload output.'}}}",
        0,
        tool_elf_run,
    },
};

void tools_elf_register(void)
{
    mcp_register(k_tools, ARRAY_LEN(k_tools));
}
