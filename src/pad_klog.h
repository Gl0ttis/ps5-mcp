/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the kernel log says about controllers, read from the klog ring. The
 * formats are SceShellCore's own format strings (read from the firmware
 * binary, build 1777439):
 *
 *   "%s() event=%s deviceId=0x%lx deviceType=%d userId=0x%x(%d)"
 *       with event DEVICE_ADDED when a controller (or a virtual pad) appears;
 *   "[CIM] " " Pad    deviceId        userId    main  sub    padHandle  vpadDeviceId isCoPilot"
 *   "[CIM] " "[%d] 0x%016lx 0x%08x  %2d   %2d  %8d    0x%08x    %d"
 *       the controller table, logged when a controller changes owner. A
 *       DualSense reports sub type 22 whether real or virtual (13.20: the
 *       user's pad 0x70301 and ours 0xa030d were both main 1, sub 22).
 *
 * Lines in a "key:value" shape that name a subType (other system processes)
 * are accepted too. Portable, so the host build tests it. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PAD_SUBTYPE_VIRTUAL 22

typedef struct padlog_added
{
    uint64_t device;
    int subtype; /* -1 when the line names none */
} padlog_added_t;

/* Parse a DEVICE_ADDED line. Returns false for any other line. */
bool padlog_parse_added(const char *line, padlog_added_t *out);

/* Wait for the new virtual pad's DEVICE_ADDED line at or after seq `since`.
 * A line with sub type 22 wins at once; otherwise, at the timeout, the single
 * device added without a sub type is taken (*exact is then false). 0 when
 * nothing usable was logged. */
uint64_t padlog_wait_device(uint64_t since, unsigned timeout_ms, bool *exact);

typedef struct padlog_controller
{
    uint64_t device;
    uint32_t user;
    int main_type, sub_type;
} padlog_controller_t;

/* Parse one row of the controller table. */
bool padlog_parse_row(const char *line, padlog_controller_t *out);

/* The controllers in the last table logged at or after `since`, waiting up to
 * timeout_ms for a table to appear. Returns the count, or -1 when no table was
 * logged. */
int padlog_controllers(uint64_t since, unsigned timeout_ms, padlog_controller_t *pads, int max);
