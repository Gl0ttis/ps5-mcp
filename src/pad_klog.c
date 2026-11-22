/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "pad_klog.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "klog.h"
#include "util.h"

/* The value after "key" in "key=0x1f", "key:1f", "\"key\":\"0x1f\"" or "key=22".
 * hex_default: a value without 0x is hex (device ids) rather than decimal. */
static bool value_after(const char *line, const char *key, bool hex_default, uint64_t *out)
{
    const char *p = strstr(line, key);
    if (p == NULL)
        return false;
    p += strlen(key);
    while (*p == '"' || *p == ' ')
        p++;
    if (*p != '=' && *p != ':')
        return false;
    p++;
    while (*p == '"' || *p == ' ')
        p++;
    if (!isxdigit((unsigned char)*p))
        return false;
    char *end;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        *out = strtoull(p + 2, &end, 16);
    else
        *out = strtoull(p, &end, hex_default ? 16 : 10);
    return end != p;
}

bool padlog_parse_added(const char *line, padlog_added_t *out)
{
    if (strstr(line, "DEVICE_ADDED") == NULL)
        return false;
    uint64_t id = 0;
    if (!value_after(line, "deviceId", true, &id) && !value_after(line, "DeviceId", true, &id))
        return false;
    out->device = id;
    uint64_t sub;
    out->subtype = value_after(line, "subType", false, &sub) ? (int)sub : -1;
    return id != 0;
}

typedef struct wait_ctx
{
    uint64_t exact;    /* a line with sub type 22 */
    uint64_t fallback; /* a line without a sub type */
    int fallback_ids;  /* distinct devices seen without a sub type */
} wait_ctx_t;

static bool wait_visit(uint64_t seq, const char *line, void *arg)
{
    (void)seq;
    wait_ctx_t *c = arg;
    padlog_added_t a;
    if (!padlog_parse_added(line, &a))
        return true;
    if (a.subtype == PAD_SUBTYPE_VIRTUAL)
    {
        c->exact = a.device;
        return false;
    }
    if (a.subtype < 0 && a.device != c->fallback)
    {
        c->fallback = a.device;
        c->fallback_ids++;
    }
    return true;
}

uint64_t padlog_wait_device(uint64_t since, unsigned timeout_ms, bool *exact)
{
    const uint64_t deadline = now_ms() + timeout_ms;
    wait_ctx_t c = {0};
    uint64_t cursor = since;
    for (;;)
    {
        cursor = klog_scan(cursor, wait_visit, &c, NULL);
        if (c.exact)
        {
            *exact = true;
            return c.exact;
        }
        const uint64_t now = now_ms();
        if (now >= deadline)
            break;
        klog_wait(cursor, (unsigned)(deadline - now));
    }
    *exact = false;
    /* Only trust a line without a sub type when it is the only device that
     * appeared: a real controller connecting at that moment must not win. */
    return c.fallback_ids == 1 ? c.fallback : 0;
}

bool padlog_parse_row(const char *line, padlog_controller_t *out)
{
    const char *p = strstr(line, "[CIM]");
    if (p == NULL)
        return false;
    p += 5;
    while (*p == ' ')
        p++;
    if (*p != '[' || !isdigit((unsigned char)p[1]))
        return false;
    p = strchr(p, ']');
    if (p == NULL)
        return false;
    char *end;
    out->device = strtoull(p + 1, &end, 16); /* strtoull accepts the 0x */
    if (end == p + 1)
        return false;
    out->user = (uint32_t)strtoul(end, &end, 16);
    out->main_type = (int)strtol(end, &end, 10);
    out->sub_type = (int)strtol(end, &end, 10);
    return true;
}

typedef struct table
{
    padlog_controller_t *pads;
    int max;
    int count;
    bool seen;
} table_t;

static bool table_visit(uint64_t seq, const char *line, void *arg)
{
    (void)seq;
    table_t *t = arg;
    const char *cim = strstr(line, "[CIM]");
    if (cim == NULL)
        return true;
    if (strncmp(cim, "[CIM]  Pad", 10) == 0)
    {
        t->count = 0; /* a new table: the last one is the current one */
        t->seen = true;
        return true;
    }
    padlog_controller_t row;
    if (t->seen && t->count < t->max && padlog_parse_row(line, &row))
        t->pads[t->count++] = row;
    return true;
}

int padlog_controllers(uint64_t since, unsigned timeout_ms, padlog_controller_t *pads, int max)
{
    table_t t;
    const uint64_t deadline = now_ms() + timeout_ms;
    for (;;)
    {
        t = (table_t){.pads = pads, .max = max};
        klog_scan(since, table_visit, &t, NULL);
        if (t.seen || now_ms() >= deadline)
            break;
        sleep_ms(100);
    }
    if (!t.seen)
        return -1;
    /* Let the rest of the table land, then read it again. */
    sleep_ms(200);
    t = (table_t){.pads = pads, .max = max};
    klog_scan(since, table_visit, &t, NULL);
    return t.count;
}
