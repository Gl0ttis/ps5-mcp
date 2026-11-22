/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "klog.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "backend.h"
#include "util.h"

static struct
{
    pthread_mutex_t lock;
    pthread_cond_t grew;
    char *lines[KLOG_RING];
    uint64_t next; /* seq of the next line */
    bool connected;
    const char *source;
    uint64_t connects;
    uint64_t bytes;
} g_klog = {.lock = PTHREAD_MUTEX_INITIALIZER, .grew = PTHREAD_COND_INITIALIZER};

static void push_line(const char *line, size_t len)
{
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
        len--;
    char *copy = malloc(len + 1);
    if (copy == NULL)
        return;
    memcpy(copy, line, len);
    copy[len] = '\0';
    pthread_mutex_lock(&g_klog.lock);
    char **slot = &g_klog.lines[g_klog.next % KLOG_RING];
    free(*slot);
    *slot = copy;
    g_klog.next++;
    pthread_cond_broadcast(&g_klog.grew);
    pthread_mutex_unlock(&g_klog.lock);
}

void klog_inject(const char *line)
{
    push_line(line, strlen(line));
}

uint64_t klog_next_seq(void)
{
    pthread_mutex_lock(&g_klog.lock);
    const uint64_t next = g_klog.next;
    pthread_mutex_unlock(&g_klog.lock);
    return next;
}

uint64_t klog_scan(uint64_t since, klog_visit_t visit, void *ctx, uint64_t *oldest)
{
    pthread_mutex_lock(&g_klog.lock);
    const uint64_t first = g_klog.next > KLOG_RING ? g_klog.next - KLOG_RING : 0;
    if (oldest)
        *oldest = first;
    uint64_t seq = since < first ? first : since;
    for (; seq < g_klog.next; seq++)
    {
        const char *line = g_klog.lines[seq % KLOG_RING];
        if (!visit(seq, line ? line : "", ctx))
        {
            seq++;
            break;
        }
    }
    pthread_mutex_unlock(&g_klog.lock);
    return seq;
}

bool klog_wait(uint64_t since, unsigned timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_klog.lock);
    while (g_klog.next <= since)
        if (pthread_cond_timedwait(&g_klog.grew, &g_klog.lock, &deadline) == ETIMEDOUT)
            break;
    const bool have = g_klog.next > since;
    pthread_mutex_unlock(&g_klog.lock);
    return have;
}

void klog_status(cJSON *obj)
{
    pthread_mutex_lock(&g_klog.lock);
    cJSON_AddBoolToObject(obj, "connected", g_klog.connected);
    cJSON_AddStringToObject(obj, "source", g_klog.source ? g_klog.source : "none");
    cJSON_AddNumberToObject(obj, "next_seq", (double)g_klog.next);
    cJSON_AddNumberToObject(obj, "held_lines",
                            (double)(g_klog.next < KLOG_RING ? g_klog.next : KLOG_RING));
    cJSON_AddNumberToObject(obj, "bytes_read", (double)g_klog.bytes);
    cJSON_AddNumberToObject(obj, "connects", (double)g_klog.connects);
    pthread_mutex_unlock(&g_klog.lock);
}

static void *klog_thread(void *arg)
{
    (void)arg;
    char partial[KLOG_LINE_MAX];
    size_t used = 0;
    for (;;)
    {
        const char *what = NULL;
        const int fd = backend_klog_open(&what);
        if (fd < 0)
        {
            sleep_ms(5000);
            continue;
        }
        pthread_mutex_lock(&g_klog.lock);
        g_klog.connected = true;
        g_klog.source = what;
        g_klog.connects++;
        pthread_mutex_unlock(&g_klog.lock);
        log_msg("klog: reading %s", what);

        char chunk[4096];
        for (;;)
        {
            struct pollfd p = {.fd = fd, .events = POLLIN};
            const int ready = poll(&p, 1, 1000);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready == 0)
                continue;
            const ssize_t n = read(fd, chunk, sizeof(chunk));
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            if (n <= 0)
                break;
            pthread_mutex_lock(&g_klog.lock);
            g_klog.bytes += (uint64_t)n;
            pthread_mutex_unlock(&g_klog.lock);
            for (ssize_t i = 0; i < n; i++)
            {
                if (chunk[i] == '\n' || used + 1 >= sizeof(partial))
                {
                    push_line(partial, used);
                    used = 0;
                    if (chunk[i] == '\n')
                        continue;
                }
                partial[used++] = chunk[i];
            }
        }
        close(fd);
        pthread_mutex_lock(&g_klog.lock);
        g_klog.connected = false;
        pthread_mutex_unlock(&g_klog.lock);
        log_msg("klog: %s closed, reconnecting", what);
        sleep_ms(2000);
    }
    return NULL;
}

void klog_start(void)
{
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, klog_thread, NULL) != 0)
        log_msg("klog: no reader thread");
    pthread_attr_destroy(&attr);
}
