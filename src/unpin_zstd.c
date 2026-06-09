/* unpin_zstd.c -- libzstd-backed implementation of the unpin_zstd.h shim.
 *
 * This file is the ONE place that includes <zstd.h>. The runtime needs only
 * decompression; unpin_zstd_compress/bound exist for the build-time packer and
 * can be compiled out (UNPIN_ZSTD_NO_COMPRESS) when linking the shipped binary.
 *
 * Contexts are created lazily and reused; they are not thread-safe, which
 * matches the VFS (single-threaded init/open path). The shared dictionary is a
 * raw zstd dictionary (from `zstd --train`), applied via the *_usingDict APIs.
 */
#include "unpin_zstd.h"
#include <zstd.h>

static const void *g_dict;
static size_t g_dict_len;

void unpin_zstd_set_dict(const void *dict, size_t dictlen) {
    g_dict = dict;
    g_dict_len = dictlen;
}

size_t unpin_zstd_decompress(void *dst, size_t dstcap, const void *src, size_t srclen) {
    static ZSTD_DCtx *dctx;
    if (!dctx) {
        dctx = ZSTD_createDCtx();
        if (!dctx) return 0;
    }
    size_t r = g_dict
        ? ZSTD_decompress_usingDict(dctx, dst, dstcap, src, srclen, g_dict, g_dict_len)
        : ZSTD_decompressDCtx(dctx, dst, dstcap, src, srclen);
    return ZSTD_isError(r) ? 0 : r;
}

#ifndef UNPIN_ZSTD_NO_COMPRESS
size_t unpin_zstd_bound(size_t srclen) {
    return ZSTD_compressBound(srclen);
}

size_t unpin_zstd_compress(void *dst, size_t dstcap, const void *src, size_t srclen, int level) {
    static ZSTD_CCtx *cctx;
    if (!cctx) {
        cctx = ZSTD_createCCtx();
        if (!cctx) return 0;
    }
    size_t r = g_dict
        ? ZSTD_compress_usingDict(cctx, dst, dstcap, src, srclen, g_dict, g_dict_len, level)
        : ZSTD_compressCCtx(cctx, dst, dstcap, src, srclen, level);
    return ZSTD_isError(r) ? 0 : r;
}
#endif /* UNPIN_ZSTD_NO_COMPRESS */
