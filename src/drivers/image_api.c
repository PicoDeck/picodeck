#include "image_api.h"
#include "display.h"
#include "sdcard.h"
#include "../os/image_decoders.h"
#include "../../third_party/umm_malloc/src/umm_malloc.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// RGB565 macro: 5 bits red, 6 bits green, 5 bits blue
#ifndef RGB565
#define RGB565(r, g, b) ((uint16_t)(((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3))
#endif

pc_image_t *image_load(const char *path) {
    if (!path) return NULL;

    sdfile_t f = sdcard_fopen(path, "r");
    if (!f) {
        return NULL;
    }

    uint8_t header[16];
    if (sdcard_fread(f, header, 16) != 16) {
        sdcard_fclose(f);
        return NULL;
    }

    // Magic byte checks
    bool is_bmp  = (header[0] == 'B' && header[1] == 'M');
    bool is_jpeg = (header[0] == 0xFF && header[1] == 0xD8);
    bool is_png  = (header[0] == 0x89 && header[1] == 0x50 &&
                    header[2] == 0x4E && header[3] == 0x47);
    bool is_gif  = (header[0] == 'G' && header[1] == 'I' && header[2] == 'F');

    if (!is_bmp && !is_jpeg && !is_png && !is_gif) {
        sdcard_fclose(f);
        return NULL;
    }

    if (is_bmp) {
        sdcard_fseek(f, 0);
        uint8_t full_header[54];
        if (sdcard_fread(f, full_header, 54) != 54) {
            sdcard_fclose(f);
            return NULL;
        }

        uint32_t data_offset;
        int32_t  w_raw, h_raw;
        uint16_t bpp;
        uint32_t compression;
        memcpy(&data_offset, &full_header[10], sizeof(data_offset));
        memcpy(&w_raw,       &full_header[18], sizeof(w_raw));
        memcpy(&h_raw,       &full_header[22], sizeof(h_raw));
        memcpy(&bpp,         &full_header[28], sizeof(bpp));
        memcpy(&compression, &full_header[30], sizeof(compression));

        int w = (int)w_raw;
        int h = (int)h_raw;

        if ((compression != 0 && compression != 3) ||
            (bpp != 16 && bpp != 24 && bpp != 32)) {
            sdcard_fclose(f);
            return NULL;
        }

        bool flip_y = true;
        if (h < 0) {
            h = -h;
            flip_y = false;
        }

        if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
            sdcard_fclose(f);
            return NULL;
        }

        size_t pixel_bytes = (size_t)w * (size_t)h * sizeof(uint16_t);
        uint16_t *pixel_data = (uint16_t *)umm_malloc(pixel_bytes);
        if (!pixel_data) {
            sdcard_fclose(f);
            return NULL;
        }

        sdcard_fseek(f, data_offset);

        int row_bytes = ((w * bpp + 31) / 32) * 4;
        uint8_t *row_buf = (uint8_t *)umm_malloc(row_bytes);
        if (!row_buf) {
            umm_free(pixel_data);
            sdcard_fclose(f);
            return NULL;
        }

        for (int y = 0; y < h; y++) {
            int dest_y = flip_y ? (h - 1 - y) : y;
            if (sdcard_fread(f, row_buf, row_bytes) != row_bytes)
                break;

            for (int x = 0; x < w; x++) {
                uint16_t color = 0;
                if (bpp == 24) {
                    uint8_t b = row_buf[x * 3];
                    uint8_t g = row_buf[x * 3 + 1];
                    uint8_t r = row_buf[x * 3 + 2];
                    color = RGB565(r, g, b);
                } else if (bpp == 32) {
                    uint8_t b = row_buf[x * 4];
                    uint8_t g = row_buf[x * 4 + 1];
                    uint8_t r = row_buf[x * 4 + 2];
                    color = RGB565(r, g, b);
                } else if (bpp == 16) {
                    uint16_t p;
                    memcpy(&p, &row_buf[x * 2], sizeof(p));
                    color = p;
                }
                pixel_data[dest_y * w + x] = color;
            }
        }

        umm_free(row_buf);
        sdcard_fclose(f);

        pc_image_t *img = (pc_image_t *)umm_malloc(sizeof(pc_image_t));
        if (!img) {
            umm_free(pixel_data);
            return NULL;
        }
        img->w = w;
        img->h = h;
        img->data = pixel_data;
        img->transparent_color = 0;
        return img;
    }

    // Not BMP — close our handle so the decoders can open their own
    sdcard_fclose(f);

    image_decode_result_t res = {0, 0, NULL};
    bool success = false;

    if (is_jpeg) {
        success = decode_jpeg_file(path, &res);
    } else if (is_png) {
        success = decode_png_file(path, &res);
    } else if (is_gif) {
        success = decode_gif_file(path, &res);
    }

    if (success && res.data) {
        pc_image_t *img = (pc_image_t *)umm_malloc(sizeof(pc_image_t));
        if (!img) {
            umm_free(res.data);
            return NULL;
        }
        img->w = res.w;
        img->h = res.h;
        img->data = res.data;
        img->transparent_color = 0;
        return img;
    }

    if (res.data) {
        umm_free(res.data);
    }
    return NULL;
}

// Header-only probe: returns a static format string ("bmp"/"jpeg"/"png"/"gif")
// and fills *out_w/*out_h, or NULL if the file isn't a recognized image.
const char *image_probe(const char *path, int *out_w, int *out_h) {
    if (!path || !out_w || !out_h) return NULL;

    sdfile_t f = sdcard_fopen(path, "r");
    if (!f) return NULL;

    uint8_t header[33];
    int got = sdcard_fread(f, header, sizeof(header));
    if (got < 10) {
        sdcard_fclose(f);
        return NULL;
    }

    // BMP: 'BM', w/h little-endian int32 at 18/22
    if (header[0] == 'B' && header[1] == 'M' && got >= 26) {
        int32_t w, h;
        memcpy(&w, &header[18], 4);
        memcpy(&h, &header[22], 4);
        if (h < 0) h = -h;
        if (w <= 0 || h <= 0) {
            sdcard_fclose(f);
            return NULL;
        }
        *out_w = (int)w;
        *out_h = (int)h;
        sdcard_fclose(f);
        return "bmp";
    }

    // PNG: 8-byte magic, then IHDR length(4)+type(4), w/h big-endian at 16/20
    if (got >= 24 && header[0] == 0x89 && header[1] == 0x50 &&
        header[2] == 0x4E && header[3] == 0x47) {
        uint32_t w = ((uint32_t)header[16] << 24) | ((uint32_t)header[17] << 16) |
                     ((uint32_t)header[18] << 8) | header[19];
        uint32_t h = ((uint32_t)header[20] << 24) | ((uint32_t)header[21] << 16) |
                     ((uint32_t)header[22] << 8) | header[23];
        if (w == 0 || h == 0) {
            sdcard_fclose(f);
            return NULL;
        }
        *out_w = (int)w;
        *out_h = (int)h;
        sdcard_fclose(f);
        return "png";
    }

    // GIF: 'GIF', w/h little-endian uint16 at 6/8
    if (header[0] == 'G' && header[1] == 'I' && header[2] == 'F') {
        *out_w = header[6] | (header[7] << 8);
        *out_h = header[8] | (header[9] << 8);
        sdcard_fclose(f);
        return "gif";
    }

    // JPEG: walk markers to the first SOF (0xC0-0xCF except DHT/DAC/RST),
    // from just after SOI. The header read left the file 33 bytes in, which
    // can be inside a segment (an APP2 ICC profile) whose bytes look like
    // markers.
    if (header[0] == 0xFF && header[1] == 0xD8) {
        uint8_t seg[7];
        sdcard_fseek(f, 2);
        for (int guard = 0; guard < 256; guard++) {
            // Find 0xFF marker prefix (skip fill bytes)
            uint8_t b = 0;
            do {
                if (sdcard_fread(f, &b, 1) != 1) {
                    sdcard_fclose(f);
                    return NULL;
                }
            } while (b != 0xFF);
            do {
                if (sdcard_fread(f, &b, 1) != 1) {
                    sdcard_fclose(f);
                    return NULL;
                }
            } while (b == 0xFF);

            if (b == 0xD8 || b == 0x01 || (b >= 0xD0 && b <= 0xD7))
                continue;  // standalone markers (SOI/TEM/RSTn)

            if (b >= 0xC0 && b <= 0xCF && b != 0xC4 && b != 0xC8 && b != 0xCC) {
                // SOFn: length(2) precision(1) height(2) width(2), big-endian
                if (sdcard_fread(f, seg, 7) != 7) {
                    sdcard_fclose(f);
                    return NULL;
                }
                *out_h = (seg[3] << 8) | seg[4];
                *out_w = (seg[5] << 8) | seg[6];
                sdcard_fclose(f);
                return "jpeg";
            }

            // Other segment: read big-endian length and skip payload
            if (sdcard_fread(f, seg, 2) != 2) {
                sdcard_fclose(f);
                return NULL;
            }
            uint16_t seglen = (seg[0] << 8) | seg[1];
            if (seglen < 2) {
                sdcard_fclose(f);
                return NULL;
            }
            sdcard_fseek(f, sdcard_ftell(f) + seglen - 2);
        }
        sdcard_fclose(f);
        return NULL;
    }

    sdcard_fclose(f);
    return NULL;
}

pc_image_t *image_new_blank(int width, int height) {
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048)
        return NULL;

    size_t bytes = (size_t)width * (size_t)height * sizeof(uint16_t);
    if (bytes > 8u * 1024u * 1024u)
        return NULL;

    pc_image_t *img = (pc_image_t *)umm_malloc(sizeof(pc_image_t));
    if (!img) return NULL;

    img->data = (uint16_t *)umm_malloc(bytes);
    if (!img->data) {
        umm_free(img);
        return NULL;
    }

    img->w = width;
    img->h = height;
    img->transparent_color = 0;
    memset(img->data, 0, bytes);
    return img;
}

void image_free(pc_image_t *img) {
    if (!img) return;
    if (img->data) {
        umm_free(img->data);
        img->data = NULL;
    }
    umm_free(img);
}

void image_draw(const pc_image_t *img, int x, int y) {
    if (!img || !img->data) return;
    display_draw_image_partial(x, y, img->w, img->h, img->data,
                               0, 0, img->w, img->h,
                               false, false, img->transparent_color);
}

void image_draw_region(const pc_image_t *img,
                       int sx, int sy, int sw, int sh,
                       int dx, int dy) {
    if (!img || !img->data) return;
    display_draw_image_partial(dx, dy, img->w, img->h, img->data,
                               sx, sy, sw, sh,
                               false, false, img->transparent_color);
}

void image_draw_scaled(const pc_image_t *img, int x, int y, int dst_w, int dst_h) {
    if (!img || !img->data) return;
    display_draw_image_scaled_nn(x, y, img->data,
                                 img->w, img->h, dst_w, dst_h,
                                 img->transparent_color);
}
