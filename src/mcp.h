/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Model Context Protocol over Streamable HTTP (stateless, JSON responses):
 * JSON-RPC dispatch and the tool registry every tools_*.c file adds to. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../third_party/cjson/cJSON.h"
#include "http.h"

/* Tool annotations (hints to the client, see the MCP spec). */
enum
{
    TOOL_READ_ONLY = 1 << 0,
    TOOL_DESTRUCTIVE = 1 << 1,
    TOOL_IDEMPOTENT = 1 << 2,
};

typedef struct tool_result
{
    cJSON *content; /* array of content blocks */
    cJSON *structured;
    bool is_error;
} tool_result_t;

typedef void (*tool_fn_t)(const cJSON *args, tool_result_t *result);

typedef struct tool_def
{
    const char *name;
    const char *title;
    const char *description;
    /* JSON Schema for the arguments, written with single quotes for
     * readability; they are turned into double quotes when registered, so a
     * schema must not contain an apostrophe. */
    const char *schema;
    unsigned flags;
    tool_fn_t fn;
} tool_def_t;

void mcp_register(const tool_def_t *defs, size_t count);
void mcp_handle_http(const http_request_t *req, http_response_t *resp);

/* Result builders. */
void tr_text(tool_result_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void tr_error(tool_result_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Adds an image block; takes ownership of base64. */
void tr_image(tool_result_t *r, char *base64, const char *mime);
/* Adds the object as pretty JSON text; takes ownership of obj. */
void tr_json(tool_result_t *r, cJSON *obj);

/* Argument accessors: the default when the key is absent or the wrong type. */
const char *arg_str(const cJSON *args, const char *key, const char *def);
double arg_num(const cJSON *args, const char *key, double def);
int arg_int(const cJSON *args, const char *key, int def);
bool arg_bool(const cJSON *args, const char *key, bool def);
const cJSON *arg_get(const cJSON *args, const char *key);

/* Registration hooks, one per tools file. */
void tools_pad_register(void);
void tools_screen_register(void);
void tools_system_register(void);
void tools_process_register(void);
void tools_klog_register(void);
void tools_fs_register(void);
void tools_app_register(void);
void tools_elf_register(void);
