/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "util.h"

#define HTTP_MAX_HEAD 16384
#define HTTP_IO_TIMEOUT_S 15

static atomic_int g_active;
static atomic_bool g_stop;
static http_handler_t g_handler;

typedef struct conn
{
    int fd;
    char peer[64];
} conn_t;

const char *http_header(const http_request_t *req, const char *name)
{
    for (int i = 0; i < req->header_count; i++)
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    return NULL;
}

void http_set_text(http_response_t *resp, int status, const char *content_type, const char *text)
{
    free(resp->body);
    resp->status = status;
    resp->content_type = content_type;
    resp->body = strdup(text ? text : "");
    resp->body_len = resp->body ? strlen(resp->body) : 0;
}

static const char *status_text(int status)
{
    switch (status)
    {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Status";
    }
}

static bool send_all(int fd, const void *data, size_t len)
{
    const char *p = data;
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

static void send_response(int fd, const http_response_t *resp)
{
    char head[1024];
    const int n = snprintf(head, sizeof(head),
                           "HTTP/1.1 %d %s\r\n"
                           "Server: " PS5MCP_NAME "/" PS5MCP_VERSION "\r\n"
                           "Content-Length: %zu\r\n"
                           "%s%s%s"
                           "%s"
                           "Cache-Control: no-store\r\n"
                           "Connection: close\r\n\r\n",
                           resp->status, status_text(resp->status), resp->body_len,
                           resp->content_type ? "Content-Type: " : "",
                           resp->content_type ? resp->content_type : "",
                           resp->content_type ? "\r\n" : "", resp->extra_headers);
    if (n > 0 && send_all(fd, head, (size_t)n) && resp->body_len > 0)
        send_all(fd, resp->body, resp->body_len);
}

static void send_simple(int fd, int status, const char *message)
{
    http_response_t resp = {.status = status, .content_type = "text/plain"};
    resp.body = (char *)message;
    resp.body_len = strlen(message);
    send_response(fd, &resp);
}

/* Read more bytes into the buffer; false on EOF, error or timeout. */
static bool recv_more(int fd, buf_t *in)
{
    if (!buf_reserve(in, 8192))
        return false;
    for (;;)
    {
        const ssize_t n = recv(fd, in->data + in->len, in->cap - in->len - 1, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        in->len += (size_t)n;
        in->data[in->len] = '\0';
        return true;
    }
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        *--end = '\0';
    return s;
}

/* Decode a chunked body that starts at in->data + start, reading more as needed.
 * Returns the decoded body length or -1 / -2 (too large). */
static long read_chunked(int fd, buf_t *in, size_t start, buf_t *body)
{
    size_t pos = start;
    for (;;)
    {
        char *eol;
        while ((eol = strstr(in->data + pos, "\r\n")) == NULL)
            if (!recv_more(fd, in))
                return -1;
        const unsigned long size = strtoul(in->data + pos, NULL, 16);
        pos = (size_t)(eol - in->data) + 2;
        if (size == 0)
            return (long)body->len; /* trailers are ignored */
        if (body->len + size > HTTP_MAX_BODY)
            return -2;
        while (in->len < pos + size + 2)
            if (!recv_more(fd, in))
                return -1;
        buf_append(body, in->data + pos, size);
        pos += size + 2;
    }
}

static void handle_connection(conn_t *c)
{
    const int fd = c->fd;
    struct timeval tv = {.tv_sec = HTTP_IO_TIMEOUT_S};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    buf_t in;
    buf_init(&in);
    buf_t chunked;
    buf_init(&chunked);
    char *head = NULL;
    http_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.peer, sizeof(req.peer), "%s", c->peer);

    char *head_end;
    for (;;)
    {
        if (in.data != NULL && (head_end = strstr(in.data, "\r\n\r\n")) != NULL)
            break;
        if (in.len > HTTP_MAX_HEAD)
        {
            send_simple(fd, 431, "request head too large\n");
            goto out;
        }
        if (!recv_more(fd, &in))
            goto out;
    }
    const size_t body_start = (size_t)(head_end - in.data) + 4;
    /* Parse a copy of the head: reading the body may move the input buffer. */
    head = strndup(in.data, (size_t)(head_end - in.data));
    if (head == NULL)
        goto out;

    /* Request line. */
    char *save = NULL;
    char *line = strtok_r(head, "\r\n", &save);
    char *sp1 = line ? strchr(line, ' ') : NULL;
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (sp1 == NULL || sp2 == NULL)
    {
        send_simple(fd, 400, "bad request line\n");
        goto out;
    }
    *sp1 = *sp2 = '\0';
    req.method = line;
    char *target = sp1 + 1;
    char *q = strchr(target, '?');
    if (q != NULL)
    {
        *q = '\0';
        req.query = q + 1;
    }
    req.path = target;

    while ((line = strtok_r(NULL, "\r\n", &save)) != NULL && req.header_count < HTTP_MAX_HEADERS)
    {
        char *colon = strchr(line, ':');
        if (colon == NULL)
            continue;
        *colon = '\0';
        req.headers[req.header_count].name = trim(line);
        req.headers[req.header_count].value = trim(colon + 1);
        req.header_count++;
    }

    /* Body. */
    const char *te = http_header(&req, "Transfer-Encoding");
    const char *cl = http_header(&req, "Content-Length");
    if (te != NULL && strcasestr(te, "chunked") != NULL)
    {
        const long n = read_chunked(fd, &in, body_start, &chunked);
        if (n == -2)
        {
            send_simple(fd, 413, "body too large\n");
            goto out;
        }
        if (n < 0)
            goto out;
        req.body = chunked.data ? chunked.data : (char *)"";
        req.body_len = chunked.len;
    }
    else if (cl != NULL)
    {
        const unsigned long len = strtoul(cl, NULL, 10);
        if (len > HTTP_MAX_BODY)
        {
            send_simple(fd, 413, "body too large\n");
            goto out;
        }
        while (in.len < body_start + len)
            if (!recv_more(fd, &in))
                goto out;
        req.body = in.data + body_start;
        req.body_len = len;
        req.body[len] = '\0';
    }
    else
    {
        req.body = (char *)"";
        req.body_len = 0;
    }

    http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.status = 500;
    g_handler(&req, &resp);
    send_response(fd, &resp);
    free(resp.body);

out:
    shutdown(fd, SHUT_WR);
    /* Let the client read the response before the socket goes away. */
    {
        char drain[256];
        struct pollfd p = {.fd = fd, .events = POLLIN};
        for (int i = 0; i < 4 && poll(&p, 1, 50) > 0; i++)
            if (recv(fd, drain, sizeof(drain), 0) <= 0)
                break;
    }
    close(fd);
    free(head);
    buf_free(&in);
    buf_free(&chunked);
}

static void *connection_thread(void *arg)
{
    conn_t *c = arg;
    handle_connection(c);
    free(c);
    atomic_fetch_sub(&g_active, 1);
    return NULL;
}

void http_stop(void)
{
    atomic_store(&g_stop, true);
}

int http_serve(uint16_t port, http_handler_t handler)
{
    g_handler = handler;
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0)
    {
        log_msg("socket: %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(server, 16) != 0)
    {
        log_msg("cannot listen on port %u: %s (already running?)", port, strerror(errno));
        close(server);
        return -1;
    }
    log_msg("listening on port %u", port);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512 * 1024);

    while (!atomic_load(&g_stop))
    {
        struct pollfd p = {.fd = server, .events = POLLIN};
        if (poll(&p, 1, 500) <= 0)
            continue;
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        const int fd = accept(server, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED)
                continue;
            log_msg("accept: %s", strerror(errno));
            sleep_ms(100);
            continue;
        }
        if (atomic_fetch_add(&g_active, 1) >= HTTP_MAX_CONNECTIONS)
        {
            atomic_fetch_sub(&g_active, 1);
            send_simple(fd, 503, "too many connections\n");
            close(fd);
            continue;
        }
        conn_t *c = calloc(1, sizeof(*c));
        if (c == NULL)
        {
            atomic_fetch_sub(&g_active, 1);
            close(fd);
            continue;
        }
        c->fd = fd;
        inet_ntop(AF_INET, &peer.sin_addr, c->peer, sizeof(c->peer));
        pthread_t thread;
        if (pthread_create(&thread, &attr, connection_thread, c) != 0)
        {
            atomic_fetch_sub(&g_active, 1);
            send_simple(fd, 503, "no thread\n");
            close(fd);
            free(c);
        }
    }
    pthread_attr_destroy(&attr);
    close(server);
    /* Give in-flight requests (e.g. the shutdown reply) a moment. */
    for (int i = 0; i < 50 && atomic_load(&g_active) > 0; i++)
        sleep_ms(100);
    return 0;
}
