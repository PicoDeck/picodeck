// zip_util.c — shared hardened ZIP engine for PicoDeck.
// See zip_util.h for design notes. This is the single miniz consumer in the
// OS; the Lua bridge (lua_bridge_zip.c) and the native ABI (main.c) are thin
// wrappers over it.

#include "zip_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Longest full destination path we will build (dest_dir + '/' + entry name).
// Entry names are capped at ZIP_MAX_NAME (255), so this leaves ~256 bytes for
// the destination prefix.
#define ZIP_UTIL_MAX_PATH 512

// ── Error helper ─────────────────────────────────────────────────────────────

static void zip_set_err(char err[ZIP_ERR_MAX], const char *fmt, ...) {
    if (!err) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, ZIP_ERR_MAX, fmt, ap);
    va_end(ap);
}

// ── Seek-based miniz read bridge ─────────────────────────────────────────────

// miniz m_pRead callback: stream the archive straight from the SD card.
// ── Heap plumbing ────────────────────────────────────────────────────────────
// miniz_common.h defines MZ_MALLOC unconditionally (no #ifndef guard), which
// silently overrides the build's attempted umm redirect in miniz_picodeck.h —
// miniz's DEFAULT allocator is therefore newlib malloc on the ~28KB SRAM
// heap, far too small for the ~11KB inflate state plus I/O buffers. Every
// archive gets explicit allocators instead: umm_malloc (8MB PSRAM) on
// firmware, plain malloc on the host/simulator.

#ifndef PICODECK_SIMULATOR
#include "umm_malloc.h"
#define ZIP_HEAP_ALLOC(n)      umm_malloc(n)
#define ZIP_HEAP_FREE(p)       umm_free(p)
#define ZIP_HEAP_REALLOC(p, n) umm_realloc(p, n)
#else
#include <stdlib.h>
#define ZIP_HEAP_ALLOC(n)      malloc(n)
#define ZIP_HEAP_FREE(p)       free(p)
#define ZIP_HEAP_REALLOC(p, n) realloc(p, n)
#endif

static void *zip_alloc_cb(void *opaque, size_t items, size_t size) {
    (void)opaque;
    return ZIP_HEAP_ALLOC(items * size);
}
static void zip_free_cb(void *opaque, void *p) {
    (void)opaque;
    ZIP_HEAP_FREE(p);
}
static void *zip_realloc_cb(void *opaque, void *p, size_t items, size_t size) {
    (void)opaque;
    return ZIP_HEAP_REALLOC(p, items * size);
}

void zip_reader_free(void *p) {
    if (p) ZIP_HEAP_FREE(p);
}

// Sequential reads (the common case while inflating) skip the redundant seek.
static size_t zip_read_cb(void *opaque, mz_uint64 file_ofs, void *buf, size_t n) {
    zip_reader_t *zr = (zip_reader_t *)opaque;
    if (!zr || !zr->file || n == 0) return 0;
    if (file_ofs > 0xFFFFFFFFu) return 0;  // FAT32 archives are < 4GB

    uint32_t ofs = (uint32_t)file_ofs;
    if (ofs != zr->last_ofs) {
        if (!sdcard_fseek(zr->file, ofs)) return 0;
    }
    int got = sdcard_fread(zr->file, buf, (int)n);
    if (got < 0) got = 0;
    zr->last_ofs = ofs + (uint32_t)got;
    return (size_t)got;
}

// ── Reader lifecycle ─────────────────────────────────────────────────────────

bool zip_reader_open(zip_reader_t *zr, const char *zip_path, char err[ZIP_ERR_MAX]) {
    if (!zr) return false;
    memset(zr, 0, sizeof(*zr));
    if (!zip_path || !zip_path[0]) {
        zip_set_err(err, "open failed");
        return false;
    }

    int size = sdcard_fsize(zip_path);
    if (size < 0) {
        zip_set_err(err, "open failed");
        return false;
    }

    sdfile_t f = sdcard_fopen(zip_path, "r");
    if (!f) {
        zip_set_err(err, "open failed");
        return false;
    }

    zr->file      = f;
    zr->file_size = (uint32_t)size;
    zr->last_ofs  = 0;

    mz_zip_zero_struct(&zr->mz);
    zr->mz.m_pRead      = zip_read_cb;
    zr->mz.m_pIO_opaque = zr;
    zr->mz.m_pAlloc     = zip_alloc_cb;
    zr->mz.m_pFree      = zip_free_cb;
    zr->mz.m_pRealloc   = zip_realloc_cb;

    if (!mz_zip_reader_init(&zr->mz, (mz_uint64)zr->file_size, 0)) {
        sdcard_fclose(f);
        memset(zr, 0, sizeof(*zr));
        zip_set_err(err, "bad zip");
        return false;
    }

    zr->open = true;
    return true;
}

void zip_reader_close(zip_reader_t *zr) {
    if (!zr) return;
    if (zr->open)
        mz_zip_reader_end(&zr->mz);
    if (zr->file)
        sdcard_fclose(zr->file);
    memset(zr, 0, sizeof(*zr));
}

int zip_reader_num_entries(const zip_reader_t *zr) {
    if (!zr || !zr->open) return 0;
    // mz_zip_reader_get_num_files takes a non-const pointer but doesn't mutate.
    return (int)mz_zip_reader_get_num_files((mz_zip_archive *)&zr->mz);
}

bool zip_reader_stat_index(zip_reader_t *zr, int idx, zip_entry_info_t *out) {
    if (!zr || !zr->open || !out || idx < 0) return false;
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zr->mz, (mz_uint)idx, &st)) return false;
    // Deliberate truncation: miniz names can be up to 511 bytes, ours cap at
    // ZIP_MAX_NAME. Over-long names fail zip_entry_name_valid() anyway.
    size_t nlen = strlen(st.m_filename);
    if (nlen > ZIP_MAX_NAME) nlen = ZIP_MAX_NAME;
    memcpy(out->name, st.m_filename, nlen);
    out->name[nlen] = '\0';
    out->size      = (uint32_t)st.m_uncomp_size;
    out->comp_size = (uint32_t)st.m_comp_size;
    out->is_dir    = mz_zip_reader_is_file_a_directory(&zr->mz, (mz_uint)idx) != 0;
    return true;
}

int zip_reader_locate(zip_reader_t *zr, const char *name) {
    if (!zr || !zr->open || !name) return -1;
    return mz_zip_reader_locate_file(&zr->mz, name, NULL, 0);
}

// ── Iterator-based decompression core ────────────────────────────────────────
// miniz's one-shot extract paths (extract_to_mem / _to_heap / _to_callback)
// declare an ~11KB tinfl_decompressor ON THE STACK. Core 0's main stack is
// 4KB of SCRATCH memory, so on hardware any DEFLATED entry silently smashed
// the stack and failed with a corrupted inflate state — the simulator's 8MB
// host stack hid it completely (stored entries worked, which is what made
// the sim tests green). Everything below therefore uses the iterator API,
// whose state — decompressor included — is heap-allocated through the
// archive's allocator (umm_malloc/PSRAM on firmware via miniz_picodeck.h).

// Decompress entry idx into dst (which must hold st.m_uncomp_size bytes).
// Returns true only when the entry decompressed completely with a good CRC
// (mz_zip_reader_extract_iter_free performs both checks).
static bool iter_read_full(zip_reader_t *zr, int idx, uint8_t *dst,
                           size_t dst_cap) {
    mz_zip_reader_extract_iter_state *it =
        mz_zip_reader_extract_iter_new(&zr->mz, (mz_uint)idx, 0);
    if (!it) return false;
    size_t total = 0;
    while (total < dst_cap) {
        size_t n = mz_zip_reader_extract_iter_read(it, dst + total,
                                                   dst_cap - total);
        if (n == 0) break;
        total += n;
    }
    return mz_zip_reader_extract_iter_free(it) == MZ_TRUE;
}

// ── In-memory read ───────────────────────────────────────────────────────────

bool zip_reader_read_to_heap(zip_reader_t *zr, int idx, void **out_data,
                             size_t *out_len, size_t max_len,
                             char err[ZIP_ERR_MAX]) {
    if (out_data) *out_data = NULL;
    if (out_len)  *out_len  = 0;
    if (!zr || !zr->open || !out_data || !out_len || idx < 0) {
        zip_set_err(err, "bad zip");
        return false;
    }

    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zr->mz, (mz_uint)idx, &st)) {
        zip_set_err(err, "bad zip");
        return false;
    }

    size_t cap = ZIP_MAX_READ_MEM;
    if (max_len && max_len < cap) cap = max_len;
    if (st.m_uncomp_size > (mz_uint64)cap) {
        zip_set_err(err, "size cap");
        return false;
    }

    // Allocated with the archive's (umm-backed) allocator — free with
    // zip_reader_free(), NOT mz_free() (which is the std-malloc pair).
    size_t len = (size_t)st.m_uncomp_size;
    void *data = zr->mz.m_pAlloc(zr->mz.m_pAlloc_opaque, 1, len ? len : 1);
    if (!data) {
        zip_set_err(err, "out of memory");
        return false;
    }
    if (!iter_read_full(zr, idx, (uint8_t *)data, len)) {
        zr->mz.m_pFree(zr->mz.m_pAlloc_opaque, data);
        zip_set_err(err, "extract failed");
        return false;
    }

    *out_data = data;  // free with zip_reader_free()
    *out_len  = len;
    return true;
}

int zip_reader_read_to_buf(zip_reader_t *zr, int idx, void *buf,
                           size_t buf_cap, char err[ZIP_ERR_MAX]) {
    if (!zr || !zr->open || !buf || idx < 0) {
        zip_set_err(err, "bad zip");
        return -1;
    }
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zr->mz, (mz_uint)idx, &st)) {
        zip_set_err(err, "bad zip");
        return -1;
    }
    if (st.m_uncomp_size > (mz_uint64)buf_cap) {
        zip_set_err(err, "buffer too small (%u needed)",
                    (unsigned)st.m_uncomp_size);
        return ZIP_READ_TOO_SMALL;
    }
    if (!iter_read_full(zr, idx, (uint8_t *)buf, (size_t)st.m_uncomp_size)) {
        zip_set_err(err, "extract failed");
        return -1;
    }
    return (int)st.m_uncomp_size;
}

// ── Streamed extraction ──────────────────────────────────────────────────────

// Chunk size for SD writes during streamed extraction (heap, not stack).
#define ZIP_STREAM_CHUNK (16u * 1024u)

// Core streamed extract — no entry-name validation (callers do that).
// Iterator API for the same stack-size reason as iter_read_full above.
static bool extract_streamed(zip_reader_t *zr, int idx, const char *dest_path,
                             char err[ZIP_ERR_MAX]) {
    if (!zip_ensure_parent_dirs(dest_path)) {
        zip_set_err(err, "mkdir failed");
        return false;
    }

    uint8_t *chunk = (uint8_t *)zr->mz.m_pAlloc(zr->mz.m_pAlloc_opaque, 1,
                                                ZIP_STREAM_CHUNK);
    if (!chunk) {
        zip_set_err(err, "out of memory");
        return false;
    }

    sdfile_t f = sdcard_fopen(dest_path, "w");
    if (!f) {
        zr->mz.m_pFree(zr->mz.m_pAlloc_opaque, chunk);
        zip_set_err(err, "write failed");
        return false;
    }

    mz_zip_reader_extract_iter_state *it =
        mz_zip_reader_extract_iter_new(&zr->mz, (mz_uint)idx, 0);
    bool write_failed = false;
    if (it) {
        for (;;) {
            size_t n = mz_zip_reader_extract_iter_read(it, chunk,
                                                       ZIP_STREAM_CHUNK);
            if (n == 0) break;
            if (sdcard_fwrite(f, chunk, (int)n) != (int)n) {
                write_failed = true;
                break;
            }
        }
    }
    bool ok = it && mz_zip_reader_extract_iter_free(it) == MZ_TRUE &&
              !write_failed;

    sdcard_fclose(f);
    zr->mz.m_pFree(zr->mz.m_pAlloc_opaque, chunk);

    if (!ok) {
        sdcard_delete(dest_path);  // don't leave a truncated file behind
        zip_set_err(err, write_failed ? "write failed" : "extract failed");
        return false;
    }
    return true;
}

bool zip_reader_extract_entry(zip_reader_t *zr, int idx, const char *dest_path,
                              char err[ZIP_ERR_MAX]) {
    if (!zr || !zr->open || !dest_path || !dest_path[0]) {
        zip_set_err(err, "bad zip");
        return false;
    }
    // Refuse before touching dest: extract_streamed opens it "w" (truncating
    // an existing file) well before miniz would notice a bad index.
    if (idx < 0 || idx >= zip_reader_num_entries(zr)) {
        zip_set_err(err, "no such entry");
        return false;
    }
    if (mz_zip_reader_is_file_a_directory(&zr->mz, (mz_uint)idx)) {
        if (!zip_ensure_parent_dirs(dest_path) || !sdcard_mkdir(dest_path)) {
            zip_set_err(err, "mkdir failed");
            return false;
        }
        return true;
    }
    return extract_streamed(zr, idx, dest_path, err);
}

// ── Extract-all with hardening ───────────────────────────────────────────────

bool zip_reader_extract_all(zip_reader_t *zr, const char *dest_dir,
                            const zip_extract_opts_t *opts,
                            zip_progress_fn progress, void *user,
                            zip_extract_result_t *result,
                            char err[ZIP_ERR_MAX]) {
    zip_extract_result_t res = { 0, 0, 0 };
    if (result) *result = res;

    if (!zr || !zr->open || !dest_dir || !dest_dir[0]) {
        zip_set_err(err, "bad zip");
        return false;
    }

    uint32_t max_entries = (opts && opts->max_entries) ? opts->max_entries
                                                       : ZIP_MAX_ENTRIES;
    uint64_t max_total   = (opts && opts->max_total_uncomp) ? opts->max_total_uncomp
                                                            : (uint64_t)ZIP_MAX_TOTAL_UNCOMP;

    int n = zip_reader_num_entries(zr);
    if ((uint32_t)n > max_entries) {
        zip_set_err(err, "entry count cap");
        return false;
    }

    // Pre-scan: validate names, sum uncompressed sizes — fail fast before
    // touching the SD card. The central directory is in memory, so this loop
    // does no I/O.
    uint64_t total_uncomp = 0;
    for (int i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zr->mz, (mz_uint)i, &st)) {
            zip_set_err(err, "bad zip");
            return false;
        }
        if (mz_zip_reader_is_file_a_directory(&zr->mz, (mz_uint)i))
            continue;  // dirs are created implicitly per file
        if (!zip_entry_name_valid(st.m_filename)) {
            res.skipped_names++;
            continue;
        }
        total_uncomp += st.m_uncomp_size;
        res.files_total++;
    }

    if (total_uncomp > max_total) {
        if (result) *result = res;
        zip_set_err(err, "size cap");
        return false;
    }

    if (opts && opts->check_free_space) {
        uint32_t free_kb = 0, total_kb = 0;
        if (sdcard_disk_info(&free_kb, &total_kb)) {
            // +64KB slack for FAT cluster rounding / directory metadata.
            uint64_t need_kb = (total_uncomp / 1024u) + 64u;
            if (need_kb > (uint64_t)free_kb) {
                if (result) *result = res;
                zip_set_err(err, "no space");
                return false;
            }
        }
    }

    // Create dest_dir and every missing parent. sdcard_mkdir is single-level
    // (FatFS f_mkdir does not create parents), so walk the whole chain — the
    // trailing '/' makes zip_ensure_parent_dirs create dest_dir itself.
    {
        char dpath[ZIP_UTIL_MAX_PATH];
        int w = snprintf(dpath, sizeof(dpath), "%s/", dest_dir);
        if (w < 0 || w >= (int)sizeof(dpath)) {
            if (result) *result = res;
            zip_set_err(err, "path too long");
            return false;
        }
        if (!zip_ensure_parent_dirs(dpath)) {
            if (result) *result = res;
            zip_set_err(err, "mkdir failed");
            return false;
        }
    }

    size_t dest_len = strlen(dest_dir);
    bool needs_slash = dest_dir[dest_len - 1] != '/';

    for (int i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zr->mz, (mz_uint)i, &st)) {
            if (result) *result = res;
            zip_set_err(err, "bad zip");
            return false;
        }
        if (mz_zip_reader_is_file_a_directory(&zr->mz, (mz_uint)i))
            continue;
        if (!zip_entry_name_valid(st.m_filename))
            continue;  // already counted in the pre-scan

        char full_path[ZIP_UTIL_MAX_PATH];
        int written = snprintf(full_path, sizeof(full_path), "%s%s%s",
                               dest_dir, needs_slash ? "/" : "", st.m_filename);
        if (written < 0 || written >= (int)sizeof(full_path)) {
            if (result) *result = res;
            zip_set_err(err, "path too long");
            return false;
        }

        if (!extract_streamed(zr, i, full_path, err)) {
            if (result) *result = res;
            return false;  // err already set
        }
        res.files_done++;

        if (progress &&
            !progress(res.files_done, res.files_total, st.m_filename, user)) {
            if (result) *result = res;
            zip_set_err(err, "aborted");
            return false;
        }
    }

    if (result) *result = res;
    return true;
}

// ── Parent directory creation (moved from lua_bridge_zip.c) ─────────────────

bool zip_ensure_parent_dirs(const char *full_path) {
    if (!full_path || !full_path[0]) return false;

    char tmp[ZIP_UTIL_MAX_PATH];
    if (strlen(full_path) >= sizeof(tmp)) return false;
    snprintf(tmp, sizeof(tmp), "%s", full_path);

    // Walk forward through the path, creating each directory component.
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            sdcard_stat_t st;
            if (!sdcard_stat(tmp, &st)) {
                if (!sdcard_mkdir(tmp)) {
                    *p = '/';
                    return false;
                }
            }
            *p = '/';
        }
    }
    return true;
}
