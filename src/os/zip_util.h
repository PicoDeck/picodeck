// zip_util.h — shared hardened ZIP engine for PicoDeck.
//
// The single miniz consumer for the OS. Everything that reads ZIP archives
// (Lua bridge, native ABI, future app bundling) goes through this module.
//
// Design notes:
//   - Seek-based reader: the archive is streamed straight from the SD card via
//     sdcard_fseek/sdcard_fread (miniz m_pRead callback), so opening a ZIP
//     costs O(central directory), not O(archive size). No whole-file heap copy.
//   - Extraction is streamed (mz_zip_reader_extract_to_callback →
//     sdcard_fwrite) — constant memory regardless of entry size.
//   - Hardened: entry-count / total-uncompressed-size caps, strict entry-name
//     validation (path traversal, absolute paths, control chars), optional SD
//     free-space check.
//
// A zip_reader_t must NOT be copied or moved after zip_reader_open() — the
// embedded mz_zip_archive holds a pointer back to the enclosing struct.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// miniz build flags — must match how the miniz sources are compiled
// (see CMakeLists.txt / simulator/CMakeLists.txt).
#ifndef MINIZ_NO_STDIO
#define MINIZ_NO_STDIO
#endif
#ifndef MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#endif
#ifndef MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#endif
#include "miniz.h"

#include "sdcard.h"
#include "zip_name.h"

// ── Hard limits ──────────────────────────────────────────────────────────────

#define ZIP_MAX_ENTRIES        8192                  // entries per archive
#define ZIP_MAX_TOTAL_UNCOMP   (256u * 1024u * 1024u) // total uncompressed bytes
#define ZIP_MAX_READ_MEM       (4u * 1024u * 1024u)   // single in-memory read
#define ZIP_ERR_MAX            96                     // error string buffer
#define ZIP_READ_TOO_SMALL     (-2)   // zip_reader_read_to_buf: entry > buf_cap

// ── Types ────────────────────────────────────────────────────────────────────

typedef struct {
    mz_zip_archive mz;        // miniz state (m_pIO_opaque points back at this struct)
    sdfile_t       file;      // open SD file handle for the archive
    uint32_t       file_size; // archive size in bytes
    uint32_t       last_ofs;  // last read position — lets us skip redundant seeks
    bool           open;
} zip_reader_t;

typedef struct {
    char     name[ZIP_MAX_NAME + 1];
    uint32_t size;       // uncompressed
    uint32_t comp_size;  // compressed
    bool     is_dir;
} zip_entry_info_t;

typedef struct {
    uint32_t max_entries;      // 0 → ZIP_MAX_ENTRIES
    uint64_t max_total_uncomp; // 0 → ZIP_MAX_TOTAL_UNCOMP
    bool     check_free_space; // verify SD free space before extracting
} zip_extract_opts_t;

typedef struct {
    int files_done;
    int files_total;
    int skipped_names; // entries skipped due to invalid/unsafe names
} zip_extract_result_t;

// Called after each extracted file. Return false to abort the extraction.
// Firmware implementations should pump the watchdog here — extraction can
// run far longer than the 10s watchdog window.
typedef bool (*zip_progress_fn)(int done, int total, const char *name, void *user);

// ── Reader lifecycle ─────────────────────────────────────────────────────────

// Open a ZIP archive on the SD card. On failure returns false and writes a
// short message into err (may be NULL). On success the reader owns an open
// SD file handle until zip_reader_close().
bool zip_reader_open(zip_reader_t *zr, const char *zip_path, char err[ZIP_ERR_MAX]);
void zip_reader_close(zip_reader_t *zr);

// Number of entries in the archive (files + directories), 0 if not open.
int  zip_reader_num_entries(const zip_reader_t *zr);

// Fill *out with entry metadata. Returns false on bad index / closed reader.
bool zip_reader_stat_index(zip_reader_t *zr, int idx, zip_entry_info_t *out);

// Locate an entry by exact name. Returns index or -1.
int  zip_reader_locate(zip_reader_t *zr, const char *name);

// ── Extraction ───────────────────────────────────────────────────────────────

// Decompress one entry into a single heap buffer allocated from the ZIP
// heap (umm_malloc/PSRAM on firmware, malloc on the simulator); free it
// with zip_reader_free(). max_len of 0 means "engine default"; either way
// the read is capped at ZIP_MAX_READ_MEM.
bool zip_reader_read_to_heap(zip_reader_t *zr, int idx, void **out_data,
                             size_t *out_len, size_t max_len,
                             char err[ZIP_ERR_MAX]);

// Decompress one entry into a caller-supplied buffer. Returns the number of
// bytes written, ZIP_READ_TOO_SMALL when the entry is larger than buf_cap
// (nothing written), or -1 on any other error.
int zip_reader_read_to_buf(zip_reader_t *zr, int idx, void *buf,
                           size_t buf_cap, char err[ZIP_ERR_MAX]);

// Stream one entry to dest_path on the SD card (constant memory). Parent
// directories are created as needed. Directory entries just create the dir.
// NOTE: does not validate the entry name — callers extracting untrusted
// archives should use zip_reader_extract_all() or check zip_entry_name_valid().
bool zip_reader_extract_entry(zip_reader_t *zr, int idx, const char *dest_path,
                              char err[ZIP_ERR_MAX]);

// Extract every file entry under dest_dir. Enforces entry-count and total
// uncompressed size caps (fails fast, before writing anything), skips —
// and counts — entries with unsafe names, and calls progress after each
// file (abort by returning false). opts/progress/user/result/err may all
// be NULL.
bool zip_reader_extract_all(zip_reader_t *zr, const char *dest_dir,
                            const zip_extract_opts_t *opts,
                            zip_progress_fn progress, void *user,
                            zip_extract_result_t *result,
                            char err[ZIP_ERR_MAX]);

// ── Helpers ──────────────────────────────────────────────────────────────────

// zip_entry_name_valid() and ZIP_MAX_NAME: zip_name.h (pure, host-tested).

// Free a buffer returned by zip_reader_read_to_heap().
void zip_reader_free(void *p);

// mkdir -p for every directory component of full_path (the final component
// is treated as a file name and not created).
bool zip_ensure_parent_dirs(const char *full_path);
