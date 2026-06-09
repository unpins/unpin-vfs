/* unpack-verify -- read a packed blob back and check every entry.
 *
 *   unpack-verify BLOB.zip [ROOTDIR]
 *
 * Opens the blob in memory (as the VFS does), auto-loads the reserved
 * ".unpin/zdict" entry if present (the dict is stored, so it reads without
 * itself), then extracts every entry. miniz verifies each entry's CRC-32 on
 * extract, so a clean pass proves correct zstd decode. With ROOTDIR, each
 * entry is also byte-compared against the original file.
 *
 * The dict-autoload logic here is the prototype for vfs.c's init path.
 */
#ifndef MINIZ_USE_ZSTD
#define MINIZ_USE_ZSTD
#endif
#include "miniz.h"
#include "unpin_zstd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZDICT_ENTRY ".unpin/zdict"

static void *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc((size_t)n ? (size_t)n : 1);
    if (b && n && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f); if (b) *len = (size_t)n; return b;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s BLOB.zip [ROOTDIR]\n", argv[0]); return 2; }
    const char *root = argc > 2 ? argv[2] : NULL;

    size_t blob_len = 0;
    void *blob = slurp(argv[1], &blob_len);
    if (!blob) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }

    mz_zip_archive zip; memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_mem(&zip, blob, blob_len, 0)) {
        fprintf(stderr, "reader init failed\n"); return 1;
    }

    /* Auto-load shared dictionary if the blob carries one. */
    void *dict = NULL;
    int di = mz_zip_reader_locate_file(&zip, ZDICT_ENTRY, NULL, 0);
    if (di >= 0) {
        size_t dlen = 0;
        dict = mz_zip_reader_extract_to_heap(&zip, (mz_uint)di, &dlen, 0);
        if (!dict) { fprintf(stderr, "dict extract failed\n"); return 1; }
        unpin_zstd_set_dict(dict, dlen);  /* held for process lifetime */
        fprintf(stderr, "loaded shared dict (%zu bytes)\n", dlen);
    }

    mz_uint n = mz_zip_reader_get_num_files(&zip);
    long ok = 0, bad = 0, dirs = 0, mismatches = 0;
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) { bad++; continue; }
        if (st.m_is_directory) { dirs++; continue; }
        if (di >= 0 && !strcmp(st.m_filename, ZDICT_ENTRY)) continue;

        size_t got_len = 0;
        void *got = mz_zip_reader_extract_to_heap(&zip, i, &got_len, 0);
        if (!got) {
            fprintf(stderr, "  EXTRACT FAIL %s: %s\n", st.m_filename,
                    mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
            bad++; continue;
        }
        ok++;
        if (root) {
            char path[8192];
            snprintf(path, sizeof path, "%s/%s", root, st.m_filename);
            size_t olen = 0; void *orig = slurp(path, &olen);
            if (!orig || olen != got_len || memcmp(orig, got, got_len) != 0) {
                fprintf(stderr, "  MISMATCH %s\n", st.m_filename); mismatches++;
            }
            free(orig);
        }
        free(got);
    }
    mz_zip_reader_end(&zip);
    free(dict); free(blob);

    fprintf(stderr, "extracted ok=%ld bad=%ld dirs=%ld%s\n", ok, bad, dirs,
            root ? "" : " (no ROOTDIR -> CRC-only check)");
    if (root) fprintf(stderr, "byte mismatches vs originals: %ld\n", mismatches);
    return (bad || mismatches) ? 1 : 0;
}
