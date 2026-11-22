/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>

#include "image.h"
#include "mcp.h"

typedef enum
{
    SHOT_NATIVE, /* fresh 4K compose via pt_call into SceShellUI + compositor shm read */
    SHOT_SYSTEM, /* the console's own screenshot: hold Create, take the new file */
    SHOT_TITLE,  /* ask a title with the grab hook for its frame */
    SHOT_LATEST, /* the newest existing capture, nothing new is taken */
} shot_mode_t;

typedef struct shot_opts
{
    shot_mode_t mode;
    img_opts_t img;
    bool keep;         /* leave the capture on the console */
    int timeout_ms;
    int hold_ms;       /* Create hold for SHOT_SYSTEM */
    char request[256]; /* SHOT_TITLE: the request file the title removes */
    char folder[256];  /* SHOT_TITLE: where the title writes its picture */
} shot_opts_t;

/* Parse the screenshot arguments (shared by screenshot and pad_sequence).
 * Returns false with an error in the result. */
bool shot_opts_parse(const cJSON *args, shot_opts_t *opts, tool_result_t *r);
/* Take the picture and add it to the result. The caller holds the console
 * lock for SHOT_SYSTEM. Returns 0 or -1 (an error is in the result). */
int shot_take(const shot_opts_t *opts, tool_result_t *r);

/* The screenshot arguments as JSON Schema properties (single-quoted). */
#define SHOT_SCHEMA_PROPS                                                                          \
    "'mode':{'type':'string','enum':['native','system','title','latest'],'default':'native',"      \
    "'description':'native (default): triggers a fresh 4K compose by pt_call\\'ing "              \
    "sceScreenshotCaptureForCoreDump inside SceShellUI (~200 ms SceShellUI freeze; no pad, no "   \
    "gallery file); the composed BGRA frame is read directly from the compositor shm. "            \
    "Firmware-agnostic (all offsets signature-scanned). Returns the current frame, not a cached " \
    "one. system: the whole screen by holding Create on the virtual pad (needs pad_open, ~3 s; "   \
    "captures are JPEG XR with HDR on and unreadable). title: the running title own frame via "    \
    "the grab hook (~1 s). latest: the newest existing capture, nothing new is taken.'},"          \
    "'max_width':{'type':'integer','default':1280,'minimum':0,'description':'Scale down to at "   \
    "most this width (0 keeps the source size).'},"                                                \
    "'max_height':{'type':'integer','default':0,'minimum':0},"                                     \
    "'crop':{'type':'object','description':'Region of the source picture, in source pixels "      \
    "(3840x2160 on a 4K output), applied before scaling: use it to read small text.',"             \
    "'properties':{'x':{'type':'integer'},'y':{'type':'integer'},'width':{'type':'integer'},"     \
    "'height':{'type':'integer'}},'required':['x','y','width','height']},"                        \
    "'format':{'type':'string','enum':['jpeg','png'],'default':'jpeg'},"                           \
    "'quality':{'type':'integer','minimum':1,'maximum':100,'default':80},"                         \
    "'grayscale':{'type':'boolean','default':false},"                                              \
    "'keep_on_console':{'type':'boolean','default':false,'description':'Leave the capture in the " \
    "console gallery instead of deleting it.'},"                                                   \
    "'title_id':{'type':'string','description':'mode=title: homebrew title id; the request is "   \
    "/data/homebrew/ID/ps5-mcp-grab and pictures go to /data/homebrew/ID/screenshots.'},"          \
    "'request_path':{'type':'string','description':'mode=title: request file, overrides title_id.'}," \
    "'folder':{'type':'string','description':'mode=title: picture folder, overrides title_id.'},"  \
    "'timeout_ms':{'type':'integer','minimum':500,'maximum':30000}"

