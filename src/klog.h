/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Keeps the kernel log in a ring buffer, read by one background thread from
 * the console's klog server (or /dev/klog). Every line gets a sequence number,
 * so a reader can ask for "everything after N" and wait for more. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../third_party/cjson/cJSON.h"

#define KLOG_RING 8192
#define KLOG_LINE_MAX 1024

void klog_start(void);
/* The sequence number the next line will get. */
uint64_t klog_next_seq(void);
/* Add a line as if the kernel had logged it (the host mock uses this). */
void klog_inject(const char *line);

/* Visit lines with seq >= since, oldest first, while visit returns true.
 * Returns the seq after the last line visited (a cursor to resume from).
 * *oldest receives the oldest seq still held. */
typedef bool (*klog_visit_t)(uint64_t seq, const char *line, void *ctx);
uint64_t klog_scan(uint64_t since, klog_visit_t visit, void *ctx, uint64_t *oldest);

/* Wait until a line with seq >= since exists, at most timeout_ms.
 * Returns true when one does. */
bool klog_wait(uint64_t since, unsigned timeout_ms);

void klog_status(cJSON *obj);
