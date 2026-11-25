/* Copyright (c) 2026 Tencent Inc.
 * SPDX-License-Identifier: Apache-2.0 */
/*
 *   Per-object LZ4 compression of chunk objects (<prefix>/data/<uuid> and
 *   <prefix>/exports/<uuid>/<idx>), done inside the S3 client so every caller
 *   still sees raw chunk bytes.
 *
 *   Off unless S3LVOL_COMPRESS=lz4. A compressed object carries the user
 *   metadata x-amz-meta-s3lvol-codec: lz4 and x-amz-meta-s3lvol-rawlen; an
 *   object without it is raw, so volumes written before compression was turned
 *   on stay readable. The (de)compression runs on a small worker pool that
 *   lives on the non-reactor cores; S3LVOL_COMPRESS_QAT=1 offloads it to QAT
 *   (qatlib) with liblz4 as the fallback. Both produce a standard LZ4 block.
 *
 *   Environment:
 *     S3LVOL_COMPRESS=none|lz4|decode default none; decode reads LZ4 objects
 *                                     but writes raw
 *     S3LVOL_COMPRESS_QAT=0|1         default 0
 *     S3LVOL_COMPRESS_THREADS=N       worker threads, default 4
 *     S3LVOL_COMPRESS_STATS_FILE=path counters rewritten once a second
 */

#ifndef S3LVOL_COMPRESS_H
#define S3LVOL_COMPRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int s3c_init(void);
void s3c_fini(void);
/* Write chunk objects as LZ4 (S3LVOL_COMPRESS=lz4). */
bool s3c_enabled(void);
/* Read chunk objects whole and decode LZ4 ones (S3LVOL_COMPRESS=lz4|decode). */
bool s3c_decode_enabled(void);
bool s3c_key_is_chunk(const char *key);

/* Run fn(arg) on a worker thread. 0 when queued. */
int s3c_submit(void (*fn)(void *), void *arg);

size_t s3c_bound(size_t len);

/* Compressed length, or < 0 when the data does not compress or on error. */
int s3c_compress(const void *src, size_t len, void *dst, size_t cap);

/* 0 when exactly raw_len bytes were produced. */
int s3c_decompress(const void *src, size_t len, void *dst, size_t raw_len);

#endif /* S3LVOL_COMPRESS_H */
