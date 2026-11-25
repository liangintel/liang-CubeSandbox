/* Copyright (c) 2026 Tencent Inc.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef S3LVOL_QAT_LZ4_H
#define S3LVOL_QAT_LZ4_H

#include <stdint.h>

struct qlz4_stats {
    uint64_t comp_ok, comp_fallback, decomp_ok, decomp_fallback;
};

/* Starts up to max_inst QAT dc instances; returns how many (>0) or a negative error. */
int qlz4_init(int level, int max_inst);
/* Raw LZ4 block (liblz4 LZ4_compress_default compatible); <0 means use software. */
int qlz4_compress(const void *src, int src_len, void *dst, int dst_cap);
/* Decompress a raw LZ4 block; returns the decompressed size or <0 (use software). */
int qlz4_decompress(const void *src, int src_len, void *dst, int dst_cap);
void qlz4_get_stats(struct qlz4_stats *s);

#endif
