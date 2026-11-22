/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Small helpers shared by every module: a growable byte buffer, base64,
 * monotonic time and the server log. */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PS5MCP_NAME "ps5-mcp"
/* Set by the build (make VERSION=..., from git tags or semantic-release). */
#ifndef PS5MCP_VERSION
#define PS5MCP_VERSION "0.0.0-dev"
#endif

#ifndef PS5MCP_DEFAULT_PORT
#define PS5MCP_DEFAULT_PORT 7165
#endif

typedef struct buf
{
    char *data;
    size_t len;
    size_t cap;
    bool oom; /* an allocation failed: the contents are truncated */
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
bool buf_reserve(buf_t *b, size_t extra);
void buf_append(buf_t *b, const void *data, size_t len);
void buf_puts(buf_t *b, const char *s);
void buf_printf(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_vprintf(buf_t *b, const char *fmt, va_list ap);
/* Detach the NUL-terminated contents; the buffer is reset. */
char *buf_take(buf_t *b);

/* Base64 (RFC 4648, padded). Returns a malloc'd NUL-terminated string. */
char *base64_encode(const void *data, size_t len);

uint64_t now_us(void);
uint64_t now_ms(void);
void sleep_ms(unsigned ms);

/* Read a whole file. Returns 0 and a malloc'd buffer, or -errno. */
int read_file(const char *path, uint8_t **out, size_t *out_len, size_t max_len);

/* The server log: stdout (which klog carries on the console) and a file. */
void log_set_file(const char *path);
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* The marker every log line of ours starts with, so klog readers can skip it. */
#define LOG_TAG "[ps5-mcp]"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
