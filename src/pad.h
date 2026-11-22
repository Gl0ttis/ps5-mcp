/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The virtual controller: holds a state and re-inserts it every few
 * milliseconds (a pad that goes quiet reads as released, then as gone), with
 * presses timed on the console rather than across the network. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "backend.h"

#define PAD_INSERT_INTERVAL_MS 8
#define PAD_CREATE_SCREENSHOT_MS 1200
#define PAD_MAX_CALL_MS 120000

/* One console job at a time: pad input and system screenshots hold this. */
void console_lock(void);
void console_unlock(void);

bool pad_is_open(void);
int pad_open(const pad_open_opts_t *opts, cJSON *report);
int pad_close(cJSON *report);
void pad_status(cJSON *obj);

/* Hold a state until the next call. */
void pad_set(const pad_state_t *state);
pad_state_t pad_get(void);
void pad_neutral_state(pad_state_t *state);
/* Press, wait ms, return to neutral, wait gap_ms. */
void pad_tap(uint32_t buttons, unsigned hold_ms, unsigned gap_ms);

/* "cross", "l1+r1", "dpad_up"... into a mask. Returns false and names the
 * offending token in err when a name is unknown. */
bool pad_parse_buttons(const char *text, uint32_t *mask, char *err, size_t err_len);
/* The mask back into "a+b" names. */
void pad_mask_names(uint32_t mask, char *out, size_t out_len);
/* All accepted button names, comma separated. */
const char *pad_button_list(void);
