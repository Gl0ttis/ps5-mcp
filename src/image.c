/* ps5-mcp - MCP server payload for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#include "../third_party/stb/stb_image.h"
#include "../third_party/stb/stb_image_write.h"
#pragma GCC diagnostic pop
/* stb declares helpers this file never uses; the warning fires at the end of the file. */
#pragma GCC diagnostic ignored "-Wunused-function"

img_kind_t img_sniff(const uint8_t *d, size_t len)
{
    if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return IMG_JPEG;
    if (len >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0)
        return IMG_PNG;
    if (len >= 2 && d[0] == 'B' && d[1] == 'M')
        return IMG_BMP;
    if (len >= 4 && d[0] == 'I' && d[1] == 'I' && d[2] == 0xBC)
        return IMG_JXR;
    return IMG_UNKNOWN;
}

const char *img_kind_mime(img_kind_t kind)
{
    switch (kind)
    {
    case IMG_JPEG: return "image/jpeg";
    case IMG_PNG: return "image/png";
    case IMG_BMP: return "image/bmp";
    case IMG_JXR: return "image/vnd.ms-photo";
    default: return "application/octet-stream";
    }
}

static void sink(void *ctx, void *data, int size)
{
    buf_append((buf_t *)ctx, data, (size_t)size);
}

/* Area-average downscale of an RGB (or grey) image: exact for integer
 * factors, smooth for the rest, and good for text, unlike nearest. */
static uint8_t *downscale(const uint8_t *src, int sw, int sh, int stride, int ch, int dw, int dh)
{
    uint8_t *dst = malloc((size_t)dw * (size_t)dh * (size_t)ch);
    uint32_t *acc = calloc((size_t)dw * (size_t)ch, sizeof(uint32_t));
    uint32_t *cnt = calloc((size_t)dw, sizeof(uint32_t));
    int *col_map = malloc(sizeof(int) * (size_t)sw);
    if (dst == NULL || acc == NULL || cnt == NULL || col_map == NULL)
    {
        free(dst);
        free(acc);
        free(cnt);
        free(col_map);
        return NULL;
    }
    for (int x = 0; x < sw; x++)
        col_map[x] = (int)((int64_t)x * dw / sw);
    int sy = 0;
    for (int dy = 0; dy < dh; dy++)
    {
        const int sy_end = (int)((int64_t)(dy + 1) * sh / dh);
        memset(acc, 0, sizeof(uint32_t) * (size_t)dw * (size_t)ch);
        memset(cnt, 0, sizeof(uint32_t) * (size_t)dw);
        for (; sy < sy_end; sy++)
        {
            const uint8_t *row = src + (size_t)sy * (size_t)stride;
            for (int x = 0; x < sw; x++)
            {
                const int dx = col_map[x];
                for (int c = 0; c < ch; c++)
                    acc[dx * ch + c] += row[x * ch + c];
                cnt[dx]++;
            }
        }
        uint8_t *out = dst + (size_t)dy * (size_t)dw * (size_t)ch;
        for (int dx = 0; dx < dw; dx++)
            for (int c = 0; c < ch; c++)
                out[dx * ch + c] = cnt[dx] ? (uint8_t)((acc[dx * ch + c] + cnt[dx] / 2) / cnt[dx]) : 0;
    }
    free(acc);
    free(cnt);
    free(col_map);
    return dst;
}

int img_process(const uint8_t *data, size_t len, const img_opts_t *opts, img_out_t *out,
                char *err, size_t err_len)
{
    memset(out, 0, sizeof(*out));
    const img_kind_t kind = img_sniff(data, len);
    if (kind == IMG_JXR)
    {
        snprintf(err, err_len, "the capture is JPEG XR (HDR is on); it cannot be decoded on the "
                               "console");
        return -1;
    }
    const int ch = opts->grayscale ? 1 : 3;
    int w = 0, h = 0, n = 0;
    uint8_t *pixels = stbi_load_from_memory(data, (int)len, &w, &h, &n, ch);
    if (pixels == NULL)
    {
        snprintf(err, err_len, "cannot decode the picture: %s", stbi_failure_reason());
        return -1;
    }
    out->src_w = w;
    out->src_h = h;

    /* Crop (clamped to the picture). */
    int cx = 0, cy = 0, cw = w, chh = h;
    if (opts->crop_w > 0 && opts->crop_h > 0)
    {
        cx = CLAMP(opts->crop_x, 0, w - 1);
        cy = CLAMP(opts->crop_y, 0, h - 1);
        cw = CLAMP(opts->crop_w, 1, w - cx);
        chh = CLAMP(opts->crop_h, 1, h - cy);
    }
    const uint8_t *view = pixels + ((size_t)cy * (size_t)w + (size_t)cx) * (size_t)ch;
    const int stride = w * ch;

    /* Scale to fit max_width x max_height, never up. */
    double scale = 1.0;
    if (opts->max_width > 0 && cw > opts->max_width)
        scale = (double)opts->max_width / cw;
    if (opts->max_height > 0 && chh * scale > opts->max_height)
        scale = (double)opts->max_height / chh;
    int dw = (int)(cw * scale + 0.5), dh = (int)(chh * scale + 0.5);
    if (dw < 1)
        dw = 1;
    if (dh < 1)
        dh = 1;

    uint8_t *scaled = NULL;
    const uint8_t *final = view;
    int final_stride = stride;
    if (dw != cw || dh != chh)
    {
        scaled = downscale(view, cw, chh, stride, ch, dw, dh);
        if (scaled == NULL)
        {
            stbi_image_free(pixels);
            snprintf(err, err_len, "out of memory scaling the picture");
            return -1;
        }
        final = scaled;
        final_stride = dw * ch;
    }

    buf_t enc;
    buf_init(&enc);
    int ok;
    if (opts->png)
        ok = stbi_write_png_to_func(sink, &enc, dw, dh, ch, final, final_stride);
    else
    {
        /* stbi_write_jpg wants a packed picture. */
        const uint8_t *packed = final;
        uint8_t *tmp = NULL;
        if (final_stride != dw * ch)
        {
            tmp = malloc((size_t)dw * (size_t)dh * (size_t)ch);
            if (tmp != NULL)
                for (int y = 0; y < dh; y++)
                    memcpy(tmp + (size_t)y * (size_t)dw * (size_t)ch,
                           final + (size_t)y * (size_t)final_stride, (size_t)dw * (size_t)ch);
            packed = tmp;
        }
        ok = packed ? stbi_write_jpg_to_func(sink, &enc, dw, dh, ch, packed,
                                             CLAMP(opts->quality, 1, 100))
                    : 0;
        free(tmp);
    }
    free(scaled);
    stbi_image_free(pixels);
    if (!ok || enc.oom || enc.len == 0)
    {
        buf_free(&enc);
        snprintf(err, err_len, "cannot encode the picture");
        return -1;
    }
    out->len = enc.len;
    out->data = (uint8_t *)buf_take(&enc);
    out->mime = opts->png ? "image/png" : "image/jpeg";
    out->out_w = dw;
    out->out_h = dh;
    return 0;
}

int img_write_png(const char *path, const uint8_t *rgb, int w, int h)
{
    buf_t enc;
    buf_init(&enc);
    if (!stbi_write_png_to_func(sink, &enc, w, h, 3, rgb, w * 3))
    {
        buf_free(&enc);
        return -1;
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL)
    {
        buf_free(&enc);
        return -1;
    }
    const size_t wrote = fwrite(enc.data, 1, enc.len, f);
    fclose(f);
    const int r = wrote == enc.len ? 0 : -1;
    buf_free(&enc);
    return r;
}
