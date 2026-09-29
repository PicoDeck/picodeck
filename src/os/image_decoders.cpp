#include "image_decoders.h"
#include <AnimatedGIF.h>
#include <JPEGDEC.h>
#include <PNGdec.h>
#include <tgx.h>

extern "C" {
#include "../drivers/sdcard.h"
#include "umm_malloc.h"
}

// RGB565 packing macro (same as display.h — kept local to avoid pulling the
// C display header into C++ where tgx::RGB565 is also a type name).
#ifndef RGB565
#define RGB565(r, g, b) ((uint16_t)(((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3))
#endif

// Decoded images are capped at 2048x2048 (as BMP and image.new already are)
// and their byte size is computed in size_t: a crafted header's w*h*2 done
// in int wraps to a tiny allocation that the decoder then overruns.
#define IMAGE_MAX_DIM 2048

static bool image_dims_ok(int w, int h, size_t *out_bytes) {
  if (w <= 0 || h <= 0 || w > IMAGE_MAX_DIM || h > IMAGE_MAX_DIM) {
    printf("[TGX] Image dimensions %dx%d rejected (limit %d)\n", w, h,
           IMAGE_MAX_DIM);
    return false;
  }
  *out_bytes = (size_t)w * (size_t)h * sizeof(uint16_t);
  return true;
}

// --- FatFS Proxy Callbacks for decoders ---

static void *my_file_open(const char *szFilename, int32_t *pFileSize) {
  sdfile_t f = sdcard_fopen(szFilename, "rb");
  if (!f)
    return NULL;
  int size = sdcard_fsize(szFilename);
  if (size < 0) {
    sdcard_fclose(f);
    return NULL;
  }
  if (pFileSize)
    *pFileSize = size;
  return (void *)f;
}

static void my_file_close(void *pHandle) {
  if (pHandle) {
    sdcard_fclose((sdfile_t)pHandle);
  }
}

// JPEG Proxy
static int32_t my_jpeg_read(JPEGFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  int32_t iBytesRead = iLen;
  if ((pFile->iSize - pFile->iPos) < iLen)
    iBytesRead = pFile->iSize - pFile->iPos;
  if (iBytesRead <= 0)
    return 0;
  int r = sdcard_fread((sdfile_t)pFile->fHandle, pBuf, iBytesRead);
  if (r > 0)
    pFile->iPos += r;
  return r < 0 ? 0 : r;
}
static int32_t my_jpeg_seek(JPEGFILE *pFile, int32_t iPosition) {
  if (iPosition < 0)
    iPosition = 0;
  else if (iPosition >= pFile->iSize)
    iPosition = pFile->iSize - 1;
  pFile->iPos = iPosition;
  sdcard_fseek((sdfile_t)pFile->fHandle, iPosition);
  return iPosition;
}

// PNG Proxy
static int32_t my_png_read(PNGFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  int32_t iBytesRead = iLen;
  if ((pFile->iSize - pFile->iPos) < iLen)
    iBytesRead = pFile->iSize - pFile->iPos;
  if (iBytesRead <= 0)
    return 0;
  int r = sdcard_fread((sdfile_t)pFile->fHandle, pBuf, iBytesRead);
  if (r > 0)
    pFile->iPos += r;
  return r < 0 ? 0 : r;
}
static int32_t my_png_seek(PNGFILE *pFile, int32_t iPosition) {
  if (iPosition < 0)
    iPosition = 0;
  else if (iPosition >= pFile->iSize)
    iPosition = pFile->iSize - 1;
  pFile->iPos = iPosition;
  sdcard_fseek((sdfile_t)pFile->fHandle, iPosition);
  return iPosition;
}

// GIF Proxy
static int32_t my_gif_read(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  int32_t iBytesRead = iLen;
  if ((pFile->iSize - pFile->iPos) < iLen)
    iBytesRead = pFile->iSize - pFile->iPos;
  if (iBytesRead <= 0)
    return 0;
  int r = sdcard_fread((sdfile_t)pFile->fHandle, pBuf, iBytesRead);
  if (r > 0)
    pFile->iPos += r;
  return r < 0 ? 0 : r;
}
static int32_t my_gif_seek(GIFFILE *pFile, int32_t iPosition) {
  if (iPosition < 0)
    iPosition = 0;
  else if (iPosition >= pFile->iSize)
    iPosition = pFile->iSize - 1;
  pFile->iPos = iPosition;
  sdcard_fseek((sdfile_t)pFile->fHandle, iPosition);
  return iPosition;
}

static int my_JPEGDraw(JPEGDRAW *pDraw) {
  return tgx::JPEGDraw<JPEGDEC, JPEGDRAW>(pDraw);
}

static int my_PNGDraw(PNGDRAW *pDraw) {
  tgx::PNGDraw<PNG, PNGDRAW>(pDraw);
  return 1;
}

static void my_GIFDraw(GIFDRAW *pDraw) {
  tgx::GIFDraw<AnimatedGIF, GIFDRAW>(pDraw);
}

bool decode_jpeg_buffer(const uint8_t *data, size_t len,
                        image_decode_result_t *result) {
  if (!data || len == 0 || !result) {
    printf("[TGX] Invalid arguments to decode_jpeg_buffer\n");
    return false;
  }

  printf("[TGX] Decoding JPEG, buffer size: %zu bytes\n", len);

  JPEGDEC *jpeg = (JPEGDEC *)umm_malloc(sizeof(JPEGDEC));
  if (!jpeg) {
    printf("[TGX] Failed to allocate JPEGDEC object\n");
    return false;
  }

  if (jpeg->openRAM((uint8_t *)data, (int)len, my_JPEGDraw)) {
    int w = jpeg->getWidth();
    int h = jpeg->getHeight();
    size_t req_mem;
    if (!image_dims_ok(w, h, &req_mem)) {
      jpeg->close();
      umm_free(jpeg);
      return false;
    }
    printf("[TGX] JPEG openRAM success. Dimensions: %dx%d. Requesting %zu "
           "bytes from PSRAM.\n",
           w, h, req_mem);

    result->w = w;
    result->h = h;
    result->data = (uint16_t *)umm_malloc(req_mem);

    if (result->data) {
      tgx::Image<tgx::RGB565> im(result->data, w, h);
      im.clear(tgx::RGB565_Black);
      int dec_res = im.JPEGDecode(*jpeg, {0, 0}, 0);
      printf("[TGX] JPEGDecode result: %d\n", dec_res);
      jpeg->close();
      umm_free(jpeg);
      return true;
    } else {
      printf("[TGX] PSRAM OOM allocating %zu bytes for image data\n", req_mem);
    }
    jpeg->close();
  } else {
    printf("[TGX] JPEG openRAM failed with error: %d\n", jpeg->getLastError());
  }
  umm_free(jpeg);
  return false;
}

bool decode_png_buffer(const uint8_t *data, size_t len,
                       image_decode_result_t *result) {
  if (!data || len == 0 || !result)
    return false;

  PNG *png = (PNG *)umm_malloc(sizeof(PNG));
  if (!png)
    return false;

  if (png->openRAM((uint8_t *)data, (int)len, my_PNGDraw) == PNG_SUCCESS) {
    int w = png->getWidth();
    int h = png->getHeight();
    size_t req_mem;
    if (!image_dims_ok(w, h, &req_mem)) {
      png->close();
      umm_free(png);
      return false;
    }
    result->w = w;
    result->h = h;
    result->data = (uint16_t *)umm_malloc(req_mem);

    if (result->data) {
      tgx::Image<tgx::RGB565> im(result->data, w, h);
      im.clear(tgx::RGB565_Black);
      im.PNGDecode(*png, {0, 0});
      png->close();
      umm_free(png);
      return true;
    }
    png->close();
  }
  umm_free(png);
  return false;
}

bool decode_gif_buffer(const uint8_t *data, size_t len,
                       image_decode_result_t *result) {
  if (!data || len == 0 || !result)
    return false;

  AnimatedGIF *gif = (AnimatedGIF *)umm_malloc(sizeof(AnimatedGIF));
  if (!gif)
    return false;

  if (gif->open((uint8_t *)data, (int)len, my_GIFDraw)) {
    int w = gif->getCanvasWidth();
    int h = gif->getCanvasHeight();
    size_t req_mem;
    if (!image_dims_ok(w, h, &req_mem)) {
      gif->close();
      umm_free(gif);
      return false;
    }
    result->w = w;
    result->h = h;
    result->data = (uint16_t *)umm_malloc(req_mem);

    if (result->data) {
      tgx::Image<tgx::RGB565> im(result->data, w, h);
      im.clear(tgx::RGB565_Black);
      im.GIFplayFrame(*gif, {0, 0});
      gif->close();
      umm_free(gif);
      return true;
    }
    gif->close();
  }
  umm_free(gif);
  return false;
}

// Helper: read little-endian values from a buffer (unaligned safe)
static uint16_t rd_le16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

bool decode_bmp_buffer(const uint8_t *data, size_t len,
                       image_decode_result_t *result) {
  if (!data || len < 54 || !result)
    return false;
  if (data[0] != 'B' || data[1] != 'M')
    return false;

  uint32_t data_offset = rd_le32(data + 10);
  int32_t w_raw = (int32_t)rd_le32(data + 18);
  int32_t h_raw = (int32_t)rd_le32(data + 22);
  uint16_t bpp = rd_le16(data + 28);
  uint32_t compression = rd_le32(data + 30);

  if ((compression != 0 && compression != 3) ||
      (bpp != 16 && bpp != 24 && bpp != 32))
    return false;

  bool flip_y = true;
  int w = w_raw;
  int h = h_raw;
  if (h < 0) {
    h = -h;
    flip_y = false;
  }
  if (w <= 0 || h <= 0 || w > 2048 || h > 2048)
    return false;

  int row_bytes = ((w * bpp + 31) / 32) * 4;
  if (data_offset > len || (size_t)row_bytes * (size_t)h > len - data_offset)
    return false;

  result->w = w;
  result->h = h;
  result->data = (uint16_t *)umm_malloc((size_t)w * h * sizeof(uint16_t));
  if (!result->data)
    return false;

  const uint8_t *row = data + data_offset;
  for (int y = 0; y < h; y++, row += row_bytes) {
    int dest_y = flip_y ? (h - 1 - y) : y;
    uint16_t *dst = result->data + dest_y * w;
    for (int x = 0; x < w; x++) {
      uint16_t color;
      if (bpp == 24) {
        uint8_t b = row[x * 3], g = row[x * 3 + 1], r = row[x * 3 + 2];
        color = RGB565(r, g, b);
      } else if (bpp == 32) {
        uint8_t b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
        color = RGB565(r, g, b);
      } else { // 16 bpp, already RGB565
        color = rd_le16(row + x * 2);
      }
      dst[x] = color;
    }
  }
  return true;
}

bool decode_jpeg_file(const char *path, image_decode_result_t *result) {
  if (!path || !result) {
    printf("[TGX] Invalid arguments to decode_jpeg_file\n");
    return false;
  }

  printf("[TGX] Decoding JPEG from file: %s\n", path);

  JPEGDEC *jpeg = (JPEGDEC *)umm_malloc(sizeof(JPEGDEC));
  if (!jpeg) {
    printf("[TGX] Failed to allocate JPEGDEC object\n");
    return false;
  }

  if (jpeg->open(path, my_file_open, my_file_close, my_jpeg_read, my_jpeg_seek,
                 my_JPEGDraw)) {
    int w = jpeg->getWidth();
    int h = jpeg->getHeight();

    int scale_div = 1;
    int scale_opt = 0;

    // Use JPEG sub-sampling to natively shrink large images without fully
    // decoding them into PSRAM.  Target ~360K pixels (≈2× display area in each
    // dimension) — enough for sharp rendering on the 320×320 screen while
    // keeping allocations well under 1 MB.  The old 1 000 000-pixel threshold
    // allowed 960×960 (921 600 px → 1.84 MB) and 640×1136 (726 400 px →
    // 1.45 MB) through without downscaling, causing PSRAM OOM.
    while ((size_t)(w / scale_div) * (size_t)(h / scale_div) > 360000 &&
           scale_div < 8) {
      if (scale_div == 1) {
        scale_div = 2;
        scale_opt = 2; /* JPEG_SCALE_HALF */
      } else if (scale_div == 2) {
        scale_div = 4;
        scale_opt = 4; /* JPEG_SCALE_QUARTER */
      } else if (scale_div == 4) {
        scale_div = 8;
        scale_opt = 8; /* JPEG_SCALE_EIGHTH */
      }
    }

    // The cap applies to the decoded (downscaled) size: a 4000x3000 photo is
    // fine because JPEG_SCALE_EIGHTH brings it well under 2048.
    int out_w = w / scale_div;
    int out_h = h / scale_div;
    size_t req_mem;
    if (!image_dims_ok(out_w, out_h, &req_mem)) {
      jpeg->close();
      umm_free(jpeg);
      return false;
    }

    printf("[TGX] JPEG open success. Original: %dx%d. Downscaled 1/%d: %dx%d. "
           "Requesting %zu bytes.\n",
           w, h, scale_div, out_w, out_h, req_mem);

    result->w = out_w;
    result->h = out_h;
    result->data = (uint16_t *)umm_malloc(req_mem);

    if (result->data) {
      tgx::Image<tgx::RGB565> im(result->data, out_w, out_h);
      im.clear(tgx::RGB565_Black);
      int dec_res = im.JPEGDecode(*jpeg, {0, 0}, scale_opt);
      printf("[TGX] JPEGDecode result: %d\n", dec_res);
      jpeg->close();
      umm_free(jpeg);
      return true;
    } else {
      printf("[TGX] PSRAM OOM allocating %zu bytes for image data\n", req_mem);
    }
    jpeg->close();
  } else {
    printf("[TGX] JPEG file open failed with error: %d\n",
           jpeg->getLastError());
  }
  umm_free(jpeg);
  return false;
}

bool decode_png_file(const char *path, image_decode_result_t *result) {
  if (!path || !result)
    return false;

  PNG *png = (PNG *)umm_malloc(sizeof(PNG));
  if (!png)
    return false;

  if (png->open(path, my_file_open, my_file_close, my_png_read, my_png_seek,
                my_PNGDraw) == PNG_SUCCESS) {
    int w = png->getWidth();
    int h = png->getHeight();
    size_t req_mem = 0;

    if (!image_dims_ok(w, h, &req_mem) || req_mem > 4000000) {
      png->close();
      umm_free(png);
      return false;
    }

    result->w = w;
    result->h = h;
    result->data = (uint16_t *)umm_malloc(req_mem);

    if (result->data) {
      tgx::Image<tgx::RGB565> im(result->data, w, h);
      im.clear(tgx::RGB565_Black);
      im.PNGDecode(*png, {0, 0});
      png->close();
      umm_free(png);
      return true;
    }
    png->close();
  }
  umm_free(png);
  return false;
}

bool decode_gif_file(const char *path, image_decode_result_t *result) {
  if (!path || !result) {
    return false;
  }

  AnimatedGIF *gif = (AnimatedGIF *)umm_malloc(sizeof(AnimatedGIF));
  if (!gif) {
    return false;
  }

  if (gif->open(path, my_file_open, my_file_close, my_gif_read, my_gif_seek,
                my_GIFDraw)) {
    int w = gif->getCanvasWidth();
    int h = gif->getCanvasHeight();
    size_t req_mem = 0;

    if (!image_dims_ok(w, h, &req_mem) || req_mem > 4000000) {
      printf("[TGX] Image too large! GIF cannot be hardware downscaled: %zu "
             "bytes\n",
             req_mem);
      gif->close();
      umm_free(gif);
      return false;
    }

    result->w = w;
    result->h = h;
    result->data = (uint16_t *)umm_malloc(req_mem);

    if (result->data) {
      tgx::Image<tgx::RGB565> im(result->data, w, h);
      im.clear(tgx::RGB565_Black);
      im.GIFplayFrame(*gif, {0, 0});
      gif->close();
      umm_free(gif);
      return true;
    }
    gif->close();
  }
  umm_free(gif);
  return false;
}

// Builds a tgx sub-image view of dst_fb covering the half-open clip rect.
// The sub-image shares the framebuffer memory with stride == dst_w, so tgx
// physically cannot write pixels outside the clip.  Returns an invalid image
// (isValid() == false, all draws no-op) when the clip is empty.
static tgx::Image<tgx::RGB565> tgx_clip_view(uint16_t *dst_fb, int dst_w,
                                             int dst_h, int clip_x0,
                                             int clip_y0, int clip_x1,
                                             int clip_y1) {
  tgx::Image<tgx::RGB565> full(dst_fb, dst_w, dst_h);
  // iBox2 takes inclusive bounds; the sub-image ctor intersects with the
  // image box, so an oversized clip degrades to the full framebuffer.
  return tgx::Image<tgx::RGB565>(
      full, tgx::iBox2(clip_x0, clip_x1 - 1, clip_y0, clip_y1 - 1));
}

extern "C" void tgx_draw_image_scaled(uint16_t *dst_fb, int dst_w, int dst_h,
                                      int clip_x0, int clip_y0, int clip_x1,
                                      int clip_y1,
                                      const uint16_t *src_data, int src_w,
                                      int src_h, float dst_x, float dst_y,
                                      float scale, float angle) {
  if (!dst_fb || !src_data)
    return;

  // Clamp the clip origin so the anchor translation below matches the actual
  // sub-view origin (the sub-image ctor clamps the box the same way).
  if (clip_x0 < 0) clip_x0 = 0;
  if (clip_y0 < 0) clip_y0 = 0;

  tgx::Image<tgx::RGB565> dst_im =
      tgx_clip_view(dst_fb, dst_w, dst_h, clip_x0, clip_y0, clip_x1, clip_y1);
  // Since tgx::Image requires non-const pointer for its constructor, we cast
  // away const. The blitScaledRotated method takes the source image by value or
  // const reference, so it won't modify the source pixels.
  tgx::Image<tgx::RGB565> src_im((uint16_t *)src_data, src_w, src_h);

  // Anchor at the center of the source image to draw it at the (dst_x, dst_y)
  // center point.  The anchor is in sub-image coordinates, hence the clip
  // origin subtraction.
  dst_im.blitScaledRotated(src_im, {src_w / 2.0f, src_h / 2.0f},
                           {(float)(dst_x - clip_x0), (float)(dst_y - clip_y0)},
                           scale, angle);
}

extern "C" void tgx_draw_image_scaled_masked(uint16_t *dst_fb, int dst_w, int dst_h,
                                            int clip_x0, int clip_y0,
                                            int clip_x1, int clip_y1,
                                            const uint16_t *src_data, int src_w,
                                            int src_h, float dst_x, float dst_y,
                                            float scale, float angle,
                                            uint16_t transparent_color) {
  if (!dst_fb || !src_data)
    return;

  if (clip_x0 < 0) clip_x0 = 0;
  if (clip_y0 < 0) clip_y0 = 0;

  tgx::Image<tgx::RGB565> dst_im =
      tgx_clip_view(dst_fb, dst_w, dst_h, clip_x0, clip_y0, clip_x1, clip_y1);
  tgx::Image<tgx::RGB565> src_im((uint16_t *)src_data, src_w, src_h);

  // Anchor at the center of the source image to draw it at the (dst_x, dst_y)
  // center point, with transparency
  tgx::RGB565 mask_color(transparent_color);
  dst_im.blitScaledRotatedMasked(src_im, mask_color,
                                 {src_w / 2.0f, src_h / 2.0f},
                                 {(float)(dst_x - clip_x0),
                                  (float)(dst_y - clip_y0)},
                                 scale, angle);
}
