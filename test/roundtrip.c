/* roundtrip.c -- validates zstd (ZIP method 93) through the patched miniz.
 *
 * Proves, end to end and in one process:
 *   1. A buffer compressed with unpin_zstd_compress and written with
 *      MZ_ZIP_FLAG_COMPRESSED_DATA|MZ_ZIP_FLAG_ZSTD_DATA reads back byte-identical
 *      via mz_zip_reader_extract_to_heap (the exact API the VFS uses).
 *   2. zstd and deflate and stored entries coexist in ONE archive -- so existing
 *      deflate blobs keep working after the patch.
 *   3. The shared-dictionary path (compress+decompress usingDict) round-trips.
 *   4. The result is a structurally valid .zip (written to argv[1] if given, for
 *      an external `unzip -l` cross-check).
 *
 * Exit 0 = all assertions passed.
 */
#ifndef MINIZ_USE_ZSTD
#define MINIZ_USE_ZSTD
#endif
#include "miniz.h"
#include "unpin_zstd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); failures++; } \
} while (0)

/* Add `data` to the writer as a zstd (method 93) entry. */
static mz_bool add_zstd(mz_zip_archive *zip, const char *name,
                        const void *data, size_t len, int level) {
    size_t bound = unpin_zstd_bound(len);
    void *comp = malloc(bound ? bound : 1);
    if (!comp) return MZ_FALSE;
    size_t clen = unpin_zstd_compress(comp, bound, data, len, level);
    if (clen == 0 && len != 0) { free(comp); return MZ_FALSE; }
    mz_uint32 crc = (mz_uint32)mz_crc32(MZ_CRC32_INIT, (const mz_uint8 *)data, len);
    mz_bool ok = mz_zip_writer_add_mem_ex_v2(
        zip, name, comp, clen, NULL, 0,
        MZ_ZIP_FLAG_COMPRESSED_DATA | MZ_ZIP_FLAG_ZSTD_DATA,
        (mz_uint64)len, crc, NULL, NULL, 0, NULL, 0);
    free(comp);
    return ok;
}

/* Read entry `name` from an in-memory zip and compare to expected bytes. */
static void verify(const void *blob, size_t blob_len, const char *name,
                   const void *expect, size_t expect_len, const char *label) {
    mz_zip_archive zip; memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_mem(&zip, blob, blob_len, 0)) {
        printf("  FAIL %s (reader init)\n", label); failures++; return;
    }
    size_t got_len = 0;
    void *got = mz_zip_reader_extract_file_to_heap(&zip, name, &got_len, 0);
    int ok = got && got_len == expect_len && memcmp(got, expect, expect_len) == 0;
    if (!ok)
        printf("  FAIL %s (name=%s got_len=%zu want=%zu err=%s)\n",
               label, name, got_len, expect_len,
               mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
    else
        printf("  ok   %s (%zu bytes -> read back identical)\n", label, expect_len);
    if (!ok) failures++;
    free(got);
    mz_zip_reader_end(&zip);
}

int main(int argc, char **argv) {
    (void)argc;  /* argv[1] is an optional dump path */
    /* A payload with heavy cross-line redundancy so zstd visibly engages. */
    static char big[200000];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = "the quick brown perl module requires strict and warnings;\n"[i % 58];
    const char *small = "package Foo; use strict; 1;\n";
    const char *stored = "x";  /* tiny -> miniz stores instead of deflating */

    printf("== build a mixed archive (zstd + deflate + stored) in memory ==\n");
    mz_zip_archive zip; memset(&zip, 0, sizeof zip);
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) { printf("writer init failed\n"); return 2; }

    CHECK(add_zstd(&zip, "big.pm", big, sizeof big, 19), "write big.pm as zstd (method 93)");
    CHECK(add_zstd(&zip, "small.pm", small, strlen(small), 19), "write small.pm as zstd");
    /* a normal deflate entry via the unmodified path */
    CHECK(mz_zip_writer_add_mem(&zip, "defl.pm", small, strlen(small), MZ_BEST_COMPRESSION),
          "write defl.pm as deflate (unchanged path)");
    CHECK(mz_zip_writer_add_mem(&zip, "stored.txt", stored, strlen(stored), MZ_NO_COMPRESSION),
          "write stored.txt as stored");

    void *blob = NULL; size_t blob_len = 0;
    CHECK(mz_zip_writer_finalize_heap_archive(&zip, &blob, &blob_len), "finalize archive");
    mz_zip_writer_end(&zip);
    printf("  archive size: %zu bytes (big.pm raw was %zu)\n", blob_len, sizeof big);

    printf("== read every entry back ==\n");
    verify(blob, blob_len, "big.pm",     big,    sizeof big,     "zstd big.pm round-trip");
    verify(blob, blob_len, "small.pm",   small,  strlen(small),  "zstd small.pm round-trip");
    verify(blob, blob_len, "defl.pm",    small,  strlen(small),  "deflate entry still reads");
    verify(blob, blob_len, "stored.txt", stored, strlen(stored), "stored entry still reads");

    if (argv[1]) {  /* dump for an external unzip -l cross-check */
        FILE *f = fopen(argv[1], "wb");
        if (f) { fwrite(blob, 1, blob_len, f); fclose(f);
                 printf("  wrote %s for external zip-tool inspection\n", argv[1]); }
    }
    free(blob);

    printf("== shared-dictionary path ==\n");
    /* Train-free smoke: any raw bytes work as a dict for the API round-trip. */
    static const unsigned char dict[4096];
    unpin_zstd_set_dict(dict, sizeof dict);
    memset(&zip, 0, sizeof zip);
    mz_zip_writer_init_heap(&zip, 0, 0);
    CHECK(add_zstd(&zip, "dicted.pm", big, sizeof big, 19), "write zstd entry using shared dict");
    void *dblob = NULL; size_t dblob_len = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &dblob, &dblob_len);
    mz_zip_writer_end(&zip);
    verify(dblob, dblob_len, "dicted.pm", big, sizeof big, "dict round-trip (decompress_usingDict)");
    free(dblob);
    unpin_zstd_set_dict(NULL, 0);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
