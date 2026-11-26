/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A small HTTP/1.1 server: one thread per connection, one request per
 * connection (Connection: close), bodies by Content-Length or chunked. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define HTTP_MAX_HEADERS 32
#define HTTP_MAX_BODY (24u * 1024u * 1024u) /* elf_run uploads up to 16 MiB as base64 */
#define HTTP_MAX_CONNECTIONS 16

typedef struct http_header
{
    const char *name;
    const char *value;
} http_header_t;

typedef struct http_request
{
    const char *method;
    const char *path; /* without the query string */
    const char *query;
    http_header_t headers[HTTP_MAX_HEADERS];
    int header_count;
    char *body;
    size_t body_len;
    char peer[64];
} http_request_t;

typedef struct http_response
{
    int status;
    const char *content_type;
    char *body; /* malloc'd, freed by the server */
    size_t body_len;
    char extra_headers[512]; /* "Name: value\r\n" lines */
} http_response_t;

typedef void (*http_handler_t)(const http_request_t *req, http_response_t *resp);

/* Case-insensitive header lookup; NULL when absent. */
const char *http_header(const http_request_t *req, const char *name);

void http_set_text(http_response_t *resp, int status, const char *content_type, const char *text);

/* Serve forever on the port (blocks). Returns non-zero when it cannot listen. */
int http_serve(uint16_t port, http_handler_t handler);

/* Ask http_serve to return after the current requests. */
void http_stop(void);

/* Break the listening socket, as a network stack reset does; http_serve then
 * rebuilds it. Async-signal-safe (main wires it to SIGUSR1). */
void http_kick_listener(void);
