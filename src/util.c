/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "util.h"

#ifdef PS5MCP_PS5
#include <ps5/klog.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void buf_init(buf_t *b)
{
    memset(b, 0, sizeof(*b));
}

void buf_free(buf_t *b)
{
    free(b->data);
    buf_init(b);
}

bool buf_reserve(buf_t *b, size_t extra)
{
    if (b->oom)
        return false;
    if (b->len + extra + 1 <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1)
        cap *= 2;
    char *data = realloc(b->data, cap);
    if (data == NULL)
    {
        b->oom = true;
        return false;
    }
    b->data = data;
    b->cap = cap;
    return true;
}

void buf_append(buf_t *b, const void *data, size_t len)
{
    if (!buf_reserve(b, len))
        return;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
}

void buf_puts(buf_t *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

void buf_vprintf(buf_t *b, const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    const int need = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (need < 0 || !buf_reserve(b, (size_t)need))
        return;
    vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap);
    b->len += (size_t)need;
}

void buf_printf(buf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    buf_vprintf(b, fmt, ap);
    va_end(ap);
}

char *buf_take(buf_t *b)
{
    char *data = b->data;
    if (data == NULL)
        data = calloc(1, 1);
    buf_init(b);
    return data;
}

char *base64_encode(const void *data, size_t len)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *in = data;
    char *out = malloc(((len + 2) / 3) * 4 + 1);
    if (out == NULL)
        return NULL;
    size_t o = 0;
    size_t i = 0;
    for (; i + 2 < len; i += 3)
    {
        const uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[o++] = alphabet[v >> 18 & 63];
        out[o++] = alphabet[v >> 12 & 63];
        out[o++] = alphabet[v >> 6 & 63];
        out[o++] = alphabet[v & 63];
    }
    if (i < len)
    {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len)
            v |= (uint32_t)in[i + 1] << 8;
        out[o++] = alphabet[v >> 18 & 63];
        out[o++] = alphabet[v >> 12 & 63];
        out[o++] = i + 1 < len ? alphabet[v >> 6 & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}

uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

uint64_t now_ms(void)
{
    return now_us() / 1000u;
}

void sleep_ms(unsigned ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        continue;
}

int read_file(const char *path, uint8_t **out, size_t *out_len, size_t max_len)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    struct stat st;
    if (fstat(fd, &st) != 0)
    {
        const int e = errno;
        close(fd);
        return -e;
    }
    if (!S_ISREG(st.st_mode))
    {
        close(fd);
        return -EISDIR;
    }
    if ((size_t)st.st_size > max_len)
    {
        close(fd);
        return -EFBIG;
    }
    uint8_t *data = malloc((size_t)st.st_size + 1);
    if (data == NULL)
    {
        close(fd);
        return -ENOMEM;
    }
    size_t have = 0;
    while (have < (size_t)st.st_size)
    {
        const ssize_t n = read(fd, data + have, (size_t)st.st_size - have);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        have += (size_t)n;
    }
    close(fd);
    data[have] = 0;
    *out = data;
    *out_len = have;
    return 0;
}

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_log_path[256];

void log_set_file(const char *path)
{
    snprintf(g_log_path, sizeof(g_log_path), "%s", path ? path : "");
}

void log_msg(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_log_lock);
#ifdef PS5MCP_PS5
    /* A payload's stdout is the ELF loader's socket, gone once the loader
     * client hangs up: the kernel log is where people look. */
    klog_printf(LOG_TAG " %s\n", line);
#else
    printf(LOG_TAG " %s\n", line);
    fflush(stdout);
#endif
    if (g_log_path[0] != '\0')
    {
        FILE *f = fopen(g_log_path, "a");
        if (f != NULL)
        {
            time_t t = time(NULL);
            struct tm tm;
            char stamp[32];
            localtime_r(&t, &tm);
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
            fprintf(f, "%s %s\n", stamp, line);
            fclose(f);
        }
    }
    pthread_mutex_unlock(&g_log_lock);
}
