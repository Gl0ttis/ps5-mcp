/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Turns a capture (JPEG or PNG, up to 4K) into something an agent can take:
 * cropped, scaled down and re-encoded on the console. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    IMG_UNKNOWN,
    IMG_JPEG,
    IMG_PNG,
    IMG_BMP,
    IMG_JXR,
} img_kind_t;

typedef struct img_opts
{
    int max_width;  /* 0: no limit */
    int max_height; /* 0: no limit */
    int crop_x, crop_y, crop_w, crop_h; /* in source pixels; crop_w == 0: none */
    bool png;       /* encode PNG instead of JPEG */
    int quality;    /* JPEG 1..100 */
    bool grayscale;
} img_opts_t;

typedef struct img_out
{
    uint8_t *data; /* malloc'd encoded image */
    size_t len;
    const char *mime;
    int src_w, src_h;
    int out_w, out_h;
} img_out_t;

img_kind_t img_sniff(const uint8_t *data, size_t len);
const char *img_kind_mime(img_kind_t kind);

/* Returns 0, or -1 with a reason in err. */
int img_process(const uint8_t *data, size_t len, const img_opts_t *opts, img_out_t *out,
                char *err, size_t err_len);

/* Encode raw RGB pixels as PNG (used by the host mock to make fake captures). */
int img_write_png(const char *path, const uint8_t *rgb, int w, int h);
