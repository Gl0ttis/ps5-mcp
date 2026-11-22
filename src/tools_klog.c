/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "klog.h"
#include "mcp.h"
#include "util.h"

typedef struct scan_ctx
{
    regex_t include, exclude, until;
    bool has_include, has_exclude, has_until;
    bool include_self;
    int max_lines;
    int matched;
    bool hit_until;
    bool overflow;
    buf_t out;
} scan_ctx_t;

static bool visit(uint64_t seq, const char *line, void *arg)
{
    scan_ctx_t *c = arg;
    (void)seq;
    if (!c->include_self && strstr(line, LOG_TAG) != NULL)
        return true;
    if (c->has_exclude && regexec(&c->exclude, line, 0, NULL, 0) == 0)
        return true;
    const bool is_until = c->has_until && regexec(&c->until, line, 0, NULL, 0) == 0;
    if (!is_until && c->has_include && regexec(&c->include, line, 0, NULL, 0) != 0)
        return true;
    if (c->matched >= c->max_lines)
    {
        c->overflow = true;
        return false;
    }
    buf_puts(&c->out, line);
    buf_puts(&c->out, "\n");
    c->matched++;
    if (is_until)
    {
        c->hit_until = true;
        return false;
    }
    return true;
}

static bool compile(regex_t *re, const char *pattern, const char *what, tool_result_t *r)
{
    const int rc = regcomp(re, pattern, REG_EXTENDED | REG_ICASE | REG_NOSUB);
    if (rc != 0)
    {
        char msg[128];
        regerror(rc, re, msg, sizeof(msg));
        tr_error(r, "bad %s pattern: %s", what, msg);
        return false;
    }
    return true;
}

static void tool_klog_read(const cJSON *args, tool_result_t *r)
{
    scan_ctx_t c;
    memset(&c, 0, sizeof(c));
    buf_init(&c.out);
    c.max_lines = CLAMP(arg_int(args, "max_lines", 200), 1, 5000);
    c.include_self = arg_bool(args, "include_self", false);
    const char *include = arg_str(args, "filter", NULL);
    const char *exclude = arg_str(args, "exclude", NULL);
    const char *until = arg_str(args, "until", NULL);
    if (include && include[0])
    {
        if (!compile(&c.include, include, "filter", r))
            goto out;
        c.has_include = true;
    }
    if (exclude && exclude[0])
    {
        if (!compile(&c.exclude, exclude, "exclude", r))
            goto out;
        c.has_exclude = true;
    }
    if (until && until[0])
    {
        if (!compile(&c.until, until, "until", r))
            goto out;
        c.has_until = true;
    }

    const cJSON *cursor_arg = arg_get(args, "cursor");
    uint64_t cursor;
    if (cJSON_IsNumber(cursor_arg) && cursor_arg->valuedouble >= 0)
        cursor = (uint64_t)cursor_arg->valuedouble;
    else
    {
        /* No cursor: the last `tail` lines (0 = only what arrives from now). */
        const uint64_t tail = (uint64_t)CLAMP(arg_int(args, "tail", 100), 0, KLOG_RING);
        const uint64_t next = klog_next_seq();
        cursor = next > tail ? next - tail : 0;
    }
    const int wait_ms = CLAMP(arg_int(args, "wait_ms", 0), 0, 60000);
    const uint64_t deadline = now_ms() + (uint64_t)wait_ms;
    uint64_t oldest = 0;
    bool waited = false;
    for (;;)
    {
        cursor = klog_scan(cursor, visit, &c, &oldest);
        if (c.overflow || c.hit_until)
            break;
        /* Keep waiting while asked to: for `until`, or for any first match. */
        const bool want_more = c.has_until ? true : c.matched == 0;
        const uint64_t now = now_ms();
        if (!want_more || now >= deadline)
            break;
        waited = true;
        klog_wait(cursor, (unsigned)(deadline - now));
    }
    if (c.overflow)
    {
        /* The line that did not fit is the next one to read. */
        cursor--;
    }

    cJSON *status = cJSON_CreateObject();
    klog_status(status);
    const bool connected = cJSON_IsTrue(cJSON_GetObjectItem(status, "connected"));
    cJSON_Delete(status);

    buf_t head;
    buf_init(&head);
    buf_printf(&head, "cursor=%llu lines=%d", (unsigned long long)cursor, c.matched);
    if (c.has_until)
        buf_printf(&head, " until=%s", c.hit_until ? "matched" : "not matched (timed out)");
    if (c.overflow)
        buf_printf(&head, " (max_lines reached: call again with this cursor for more)");
    if (oldest > 0 && cJSON_IsNumber(cursor_arg) && (uint64_t)cursor_arg->valuedouble < oldest)
        buf_printf(&head, " (lines before %llu were dropped from the ring)",
                   (unsigned long long)oldest);
    if (!connected)
        buf_printf(&head, " [klog reader not connected: is the klog server running?]");
    if (waited && c.matched == 0)
        buf_printf(&head, " (waited %d ms)", wait_ms);
    tr_text(r, "%s\n%s", head.data, c.out.data ? c.out.data : "");
    buf_free(&head);

out:
    if (c.has_include)
        regfree(&c.include);
    if (c.has_exclude)
        regfree(&c.exclude);
    if (c.has_until)
        regfree(&c.until);
    buf_free(&c.out);
}

static const tool_def_t k_tools[] = {
    {
        "klog_read",
        "Read the kernel log",
        "Read the console kernel log (what payloads and titles print, crash reports, system "
        "messages), kept in a ring of the last 8192 lines since the server started. Without a "
        "cursor it returns the last `tail` lines; pass the returned cursor next time to get "
        "only new lines. filter/exclude/until are case-insensitive extended regexes. With "
        "wait_ms it waits for new matching lines; with until it waits until a line matches "
        "(e.g. launch a title, then wait for its startup message).",
        "{'type':'object','properties':{"
        "'cursor':{'type':'integer','description':'Return lines from this sequence number on "
        "(from a previous call).'},"
        "'tail':{'type':'integer','default':100,'description':'Without cursor: how many recent "
        "lines.'},"
        "'filter':{'type':'string','description':'Only lines matching this regex.'},"
        "'exclude':{'type':'string','description':'Drop lines matching this regex.'},"
        "'until':{'type':'string','description':'Stop at (and include) the first line matching "
        "this regex, waiting up to wait_ms for it.'},"
        "'wait_ms':{'type':'integer','default':0,'maximum':60000},"
        "'max_lines':{'type':'integer','default':200,'maximum':5000},"
        "'include_self':{'type':'boolean','default':false,'description':'Include this server "
        "own log lines.'}}}",
        TOOL_READ_ONLY,
        tool_klog_read,
    },
};

void tools_klog_register(void)
{
    mcp_register(k_tools, ARRAY_LEN(k_tools));
}
