/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "mcp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

#define MAX_TOOLS 64

typedef struct registered_tool
{
    const tool_def_t *def;
    cJSON *schema;
} registered_tool_t;

static registered_tool_t g_tools[MAX_TOOLS];
static size_t g_tool_count;

static const char *const k_versions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

static const char k_instructions[] =
    "You are driving a real PlayStation 5 through a virtual DualSense controller.\n"
    "Loop: look (screenshot), act (pad_press / pad_sequence), look again. Never assume a press "
    "landed; menus do not always start on their first entry, and titles need a moment after "
    "launch before they read the pad.\n"
    "Setup: call pad_open once before sending any input. It is NOT required for screenshots "
    "(mode=native is the default and uses no pad). By default pad_open disconnects the real "
    "controller (titles read only their user's first controller); the PS button on the real "
    "controller reconnects it. Call pad_close when done.\n"
    "Screenshots: screenshot mode=native (default) triggers a fresh 4K compose inside "
    "SceShellUI and returns the live composited frame — no pad, no gallery file, about "
    "200 ms (plus a one-time ~1.5 s scan on the first call per SceShellUI PID). mode=system "
    "captures through the console's own Create-hold path (needs pad_open, about 3 s; JPEG XR "
    "with HDR on, which cannot be decoded on the console). mode=title asks a title with the "
    "grab hook for its own frame (about 1 s). mode=latest returns the newest existing capture "
    "without taking a new one. Use max_width to keep images small and crop to zoom into small "
    "text.\n"
    "pad_sequence with a trailing 'shot' step does a whole navigation and returns the picture "
    "in one call.\n"
    "Diagnostics: system_info, list_processes, process_info, klog_read (kernel log with a "
    "cursor, filters and wait-for-pattern), fs_list / fs_read, app_list / app_launch / "
    "app_kill, elf_run (start a payload through the ELF loader and see its output).\n"
    "Etiquette: one console job at a time, never over a title someone is playing; leave the "
    "console as you found it.";

/* ---- result builders ------------------------------------------------- */

static void add_block(tool_result_t *r, cJSON *block)
{
    if (r->content == NULL)
        r->content = cJSON_CreateArray();
    cJSON_AddItemToArray(r->content, block);
}

static void add_text(tool_result_t *r, const char *fmt, va_list ap)
{
    buf_t b;
    buf_init(&b);
    buf_vprintf(&b, fmt, ap);
    cJSON *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "type", "text");
    cJSON_AddStringToObject(block, "text", b.data ? b.data : "");
    buf_free(&b);
    add_block(r, block);
}

void tr_text(tool_result_t *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    add_text(r, fmt, ap);
    va_end(ap);
}

void tr_error(tool_result_t *r, const char *fmt, ...)
{
    r->is_error = true;
    va_list ap;
    va_start(ap, fmt);
    add_text(r, fmt, ap);
    va_end(ap);
}

void tr_image(tool_result_t *r, char *base64, const char *mime)
{
    cJSON *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "type", "image");
    cJSON_AddStringToObject(block, "data", base64);
    cJSON_AddStringToObject(block, "mimeType", mime);
    free(base64);
    add_block(r, block);
}

void tr_json(tool_result_t *r, cJSON *obj)
{
    char *text = cJSON_Print(obj);
    cJSON *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "type", "text");
    cJSON_AddStringToObject(block, "text", text ? text : "null");
    free(text);
    add_block(r, block);
    if (r->structured == NULL && cJSON_IsObject(obj))
        r->structured = obj;
    else
        cJSON_Delete(obj);
}

/* ---- argument accessors ---------------------------------------------- */

const cJSON *arg_get(const cJSON *args, const char *key)
{
    return args ? cJSON_GetObjectItemCaseSensitive(args, key) : NULL;
}

const char *arg_str(const cJSON *args, const char *key, const char *def)
{
    const cJSON *v = arg_get(args, key);
    return cJSON_IsString(v) ? v->valuestring : def;
}

double arg_num(const cJSON *args, const char *key, double def)
{
    const cJSON *v = arg_get(args, key);
    if (cJSON_IsNumber(v))
        return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0] != '\0')
    {
        char *end;
        const double d = strtod(v->valuestring, &end);
        if (*end == '\0')
            return d;
    }
    return def;
}

int arg_int(const cJSON *args, const char *key, int def)
{
    const double d = arg_num(args, key, (double)def);
    if (d > 2147483647.0)
        return 2147483647;
    if (d < -2147483648.0)
        return (-2147483647 - 1);
    return (int)d;
}

bool arg_bool(const cJSON *args, const char *key, bool def)
{
    const cJSON *v = arg_get(args, key);
    if (cJSON_IsBool(v))
        return cJSON_IsTrue(v);
    if (cJSON_IsString(v))
        return strcasecmp(v->valuestring, "true") == 0 || strcmp(v->valuestring, "1") == 0;
    if (cJSON_IsNumber(v))
        return v->valuedouble != 0;
    return def;
}

/* ---- registry -------------------------------------------------------- */

void mcp_register(const tool_def_t *defs, size_t count)
{
    for (size_t i = 0; i < count && g_tool_count < MAX_TOOLS; i++)
    {
        char *text = strdup(defs[i].schema);
        for (char *p = text; p && *p; p++)
            if (*p == '\'')
                *p = '"';
        cJSON *schema = text ? cJSON_Parse(text) : NULL;
        free(text);
        if (schema == NULL)
        {
            log_msg("BUG: schema of tool %s does not parse", defs[i].name);
            schema = cJSON_Parse("{\"type\":\"object\"}");
        }
        g_tools[g_tool_count].def = &defs[i];
        g_tools[g_tool_count].schema = schema;
        g_tool_count++;
    }
}

static const registered_tool_t *find_tool(const char *name)
{
    for (size_t i = 0; i < g_tool_count; i++)
        if (strcmp(g_tools[i].def->name, name) == 0)
            return &g_tools[i];
    return NULL;
}

/* ---- JSON-RPC -------------------------------------------------------- */

static cJSON *rpc_error(const cJSON *id, int code, const char *message)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON *err = cJSON_AddObjectToObject(resp, "error");
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return resp;
}

static cJSON *rpc_result(const cJSON *id, cJSON *result)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

static cJSON *do_initialize(const cJSON *params)
{
    const char *wanted = arg_str(params, "protocolVersion", NULL);
    const char *version = k_versions[0];
    for (size_t i = 0; wanted && i < ARRAY_LEN(k_versions); i++)
        if (strcmp(wanted, k_versions[i]) == 0)
            version = k_versions[i];

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", version);
    cJSON *caps = cJSON_AddObjectToObject(result, "capabilities");
    cJSON *tools = cJSON_AddObjectToObject(caps, "tools");
    cJSON_AddFalseToObject(tools, "listChanged");
    cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
    cJSON_AddStringToObject(info, "name", PS5MCP_NAME);
    cJSON_AddStringToObject(info, "title", "PS5 MCP");
    cJSON_AddStringToObject(info, "version", PS5MCP_VERSION);
    cJSON_AddStringToObject(result, "instructions", k_instructions);
    return result;
}

static cJSON *do_tools_list(void)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(result, "tools");
    for (size_t i = 0; i < g_tool_count; i++)
    {
        const tool_def_t *d = g_tools[i].def;
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", d->name);
        cJSON_AddStringToObject(t, "title", d->title);
        cJSON_AddStringToObject(t, "description", d->description);
        cJSON_AddItemToObject(t, "inputSchema", cJSON_Duplicate(g_tools[i].schema, 1));
        cJSON *ann = cJSON_AddObjectToObject(t, "annotations");
        cJSON_AddStringToObject(ann, "title", d->title);
        cJSON_AddBoolToObject(ann, "readOnlyHint", (d->flags & TOOL_READ_ONLY) != 0);
        cJSON_AddBoolToObject(ann, "destructiveHint", (d->flags & TOOL_DESTRUCTIVE) != 0);
        cJSON_AddBoolToObject(ann, "idempotentHint", (d->flags & TOOL_IDEMPOTENT) != 0);
        cJSON_AddFalseToObject(ann, "openWorldHint");
        cJSON_AddItemToArray(list, t);
    }
    return result;
}

static cJSON *do_tools_call(const cJSON *id, const cJSON *params, const char *peer)
{
    const char *name = arg_str(params, "name", NULL);
    if (name == NULL)
        return rpc_error(id, -32602, "tools/call needs a tool name");
    const registered_tool_t *tool = find_tool(name);
    if (tool == NULL)
    {
        char msg[160];
        snprintf(msg, sizeof(msg), "Unknown tool: %s", name);
        return rpc_error(id, -32602, msg);
    }
    const cJSON *args = arg_get(params, "arguments");
    if (args != NULL && !cJSON_IsObject(args))
        return rpc_error(id, -32602, "arguments must be an object");

    char *printed = args ? cJSON_PrintUnformatted(args) : NULL;
    log_msg("%s call %s %.300s", peer, name, printed ? printed : "{}");
    free(printed);

    tool_result_t r = {0};
    const uint64_t start = now_ms();
    tool->def->fn(args, &r);
    log_msg("%s done %s in %llu ms%s", peer, name, (unsigned long long)(now_ms() - start),
            r.is_error ? " (error)" : "");

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "content", r.content ? r.content : cJSON_CreateArray());
    if (r.structured)
        cJSON_AddItemToObject(result, "structuredContent", r.structured);
    cJSON_AddBoolToObject(result, "isError", r.is_error);
    return rpc_result(id, result);
}

/* Handle one JSON-RPC message; NULL when it needs no reply. */
static cJSON *handle_message(const cJSON *msg, const char *peer)
{
    if (!cJSON_IsObject(msg))
        return rpc_error(NULL, -32600, "Invalid Request");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const char *method = arg_str(msg, "method", NULL);
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");

    if (method == NULL)
        return NULL; /* a response to something we never asked: ignore */
    if (id == NULL || cJSON_IsNull(id))
        return NULL; /* a notification (notifications/initialized, cancelled, ...) */

    if (strcmp(method, "initialize") == 0)
        return rpc_result(id, do_initialize(params));
    if (strcmp(method, "ping") == 0)
        return rpc_result(id, cJSON_CreateObject());
    if (strcmp(method, "tools/list") == 0)
        return rpc_result(id, do_tools_list());
    if (strcmp(method, "tools/call") == 0)
        return do_tools_call(id, params, peer);
    if (strcmp(method, "resources/list") == 0)
    {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddArrayToObject(r, "resources");
        return rpc_result(id, r);
    }
    if (strcmp(method, "prompts/list") == 0)
    {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddArrayToObject(r, "prompts");
        return rpc_result(id, r);
    }
    if (strcmp(method, "logging/setLevel") == 0)
        return rpc_result(id, cJSON_CreateObject());
    return rpc_error(id, -32601, "Method not found");
}

/* The host part of "scheme://host:port/..." or "host:port". */
static void host_part(const char *in, char *out, size_t len)
{
    const char *p = strstr(in, "://");
    p = p ? p + 3 : in;
    size_t n = 0;
    if (*p == '[') /* IPv6 literal */
    {
        while (p[n] && p[n] != ']' && n + 2 < len)
            n++;
        if (p[n] == ']')
            n++;
    }
    else
        while (p[n] && p[n] != ':' && p[n] != '/' && n + 1 < len)
            n++;
    memcpy(out, p, n);
    out[n] = '\0';
}

static bool is_ipv4_literal(const char *h)
{
    int dots = 0;
    if (*h == '\0')
        return false;
    for (; *h; h++)
    {
        if (*h == '.')
            dots++;
        else if (*h < '0' || *h > '9')
            return false;
    }
    return dots == 3;
}

/* DNS-rebinding guard. Clients that are not browsers send no Origin and pass.
 * A browser page passes only when it is served from this machine's localhost,
 * or from this console's own IP address. In a rebinding attack the Origin and
 * the Host both carry the attacker's domain name, so names never pass. */
static bool origin_allowed(const http_request_t *req)
{
    const char *origin = http_header(req, "Origin");
    if (origin == NULL || origin[0] == '\0')
        return true;
    char oh[256], hh[256];
    host_part(origin, oh, sizeof(oh));
    if (strcmp(oh, "localhost") == 0 || strcmp(oh, "127.0.0.1") == 0 || strcmp(oh, "[::1]") == 0)
        return true;
    const char *host = http_header(req, "Host");
    if (host == NULL)
        return false;
    host_part(host, hh, sizeof(hh));
    return is_ipv4_literal(oh) && strcmp(oh, hh) == 0;
}

static void set_json(http_response_t *resp, int status, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    free(resp->body);
    resp->status = status;
    resp->content_type = "application/json";
    resp->body = text;
    resp->body_len = text ? strlen(text) : 0;
}

static void handle_info(http_response_t *resp)
{
    buf_t b;
    buf_init(&b);
    buf_printf(&b, "%s %s - Model Context Protocol server on a PS5\n\n", PS5MCP_NAME,
               PS5MCP_VERSION);
    buf_puts(&b, "Endpoint: POST /mcp (Streamable HTTP, JSON responses, no auth)\n\nTools:\n");
    for (size_t i = 0; i < g_tool_count; i++)
        buf_printf(&b, "  %-18s %s\n", g_tools[i].def->name, g_tools[i].def->title);
    resp->status = 200;
    resp->content_type = "text/plain; charset=utf-8";
    resp->body_len = b.len;
    resp->body = buf_take(&b);
}

void mcp_handle_http(const http_request_t *req, http_response_t *resp)
{
    const bool is_mcp = strcmp(req->path, "/mcp") == 0 || strcmp(req->path, "/mcp/") == 0;

    if (!origin_allowed(req))
    {
        http_set_text(resp, 403, "text/plain", "origin not allowed\n");
        return;
    }
    if (strcmp(req->method, "OPTIONS") == 0)
    {
        resp->status = 204;
        snprintf(resp->extra_headers, sizeof(resp->extra_headers),
                 "Allow: GET, POST, DELETE, OPTIONS\r\n");
        return;
    }
    if (strcmp(req->method, "GET") == 0 && (strcmp(req->path, "/") == 0 ||
                                            strcmp(req->path, "/health") == 0))
    {
        handle_info(resp);
        return;
    }
    if (!is_mcp)
    {
        http_set_text(resp, 404, "text/plain", "not found: the MCP endpoint is /mcp\n");
        return;
    }
    if (strcmp(req->method, "GET") == 0)
    {
        /* No server-initiated stream: the spec's answer is 405. */
        http_set_text(resp, 405, "text/plain", "no SSE stream; POST JSON-RPC to /mcp\n");
        snprintf(resp->extra_headers, sizeof(resp->extra_headers), "Allow: POST, DELETE\r\n");
        return;
    }
    if (strcmp(req->method, "DELETE") == 0)
    {
        /* Stateless: there is no session to end. */
        resp->status = 200;
        return;
    }
    if (strcmp(req->method, "POST") != 0)
    {
        http_set_text(resp, 405, "text/plain", "method not allowed\n");
        snprintf(resp->extra_headers, sizeof(resp->extra_headers), "Allow: POST, DELETE\r\n");
        return;
    }

    cJSON *msg = cJSON_ParseWithLength(req->body, req->body_len);
    if (msg == NULL)
    {
        set_json(resp, 400, rpc_error(NULL, -32700, "Parse error"));
        return;
    }

    cJSON *reply = NULL;
    if (cJSON_IsArray(msg))
    {
        /* JSON-RPC batch (allowed by protocol 2025-03-26). */
        cJSON *replies = cJSON_CreateArray();
        const cJSON *item;
        cJSON_ArrayForEach(item, msg)
        {
            cJSON *r = handle_message(item, req->peer);
            if (r)
                cJSON_AddItemToArray(replies, r);
        }
        if (cJSON_GetArraySize(replies) > 0)
            reply = replies;
        else
            cJSON_Delete(replies);
    }
    else
        reply = handle_message(msg, req->peer);
    cJSON_Delete(msg);

    if (reply == NULL)
    {
        resp->status = 202; /* notifications and responses only */
        return;
    }
    set_json(resp, 200, reply);
}
