/* Copyright (c) 2026 Tencent Inc.
 * SPDX-License-Identifier: Apache-2.0 */
/*
 * Chunk-object compression helpers (lib/s3bsdev/s3_compress.c): which keys are
 * compressed, and the liblz4 round trip the S3 client relies on. No S3, no
 * QAT, no worker threads -- s3c_init() is never called, so this is the codec
 * every build falls back to.
 */

#include "s3_compress.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK (1u << 20)

static int g_pass, g_fail;

static void
check_true(const char *what, bool ok)
{
	if (ok) {
		g_pass++;
		printf("\t[PASS] %s\n", what);
	} else {
		g_fail++;
		printf("\t[FAIL] %s\n", what);
	}
}

static void
test_keys(void)
{
	printf("[1] only block objects are compressed\n");
	check_true("data chunk",
		   s3c_key_is_chunk("rcow-1/data/0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9"));
	check_true("export chunk",
		   s3c_key_is_chunk("rcow-1/exports/0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9/00000000000000a1"));
	check_true("export manifest is not",
		   !s3c_key_is_chunk("exports/0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9.json"));
	check_true("prefixed export manifest is not",
		   !s3c_key_is_chunk("rcow-1/exports/0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9.json"));
	check_true("checkpoint is not", !s3c_key_is_chunk("rcow-1/meta/checkpoint"));
	check_true("export lease is not",
		   !s3c_key_is_chunk("rcow-1/meta/exports/0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9.lease"));
	check_true("data key without a uuid is not", !s3c_key_is_chunk("rcow-1/data/owner"));
	check_true("unprefixed data key is not",
		   !s3c_key_is_chunk("data/0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9"));
	check_true("export index must be 16 hex digits",
		   !s3c_key_is_chunk("rcow-1/exports/0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9/a1"));
	check_true("compression is off until s3c_init()", !s3c_enabled() && !s3c_decode_enabled());
}

static void
test_round_trip(void)
{
	char *src = malloc(CHUNK), *out = malloc(CHUNK);
	size_t cap = s3c_bound(CHUNK);
	char *z = malloc(cap);
	int n;

	printf("[2] compressible chunk round-trips\n");
	for (size_t i = 0; i < CHUNK; i++) {
		src[i] = (i % 4096) < 1024 ? (char)(i * 7) : 0;
	}
	n = s3c_compress(src, CHUNK, z, cap);
	check_true("compresses to a smaller object", n > 0 && (size_t)n < CHUNK);
	check_true("decompresses to the original", n > 0 &&
		   s3c_decompress(z, (size_t)n, out, CHUNK) == 0 && memcmp(src, out, CHUNK) == 0);
	check_true("a wrong raw length is an error", n > 0 &&
		   s3c_decompress(z, (size_t)n, out, CHUNK - 1) != 0);
	check_true("a truncated object is an error", n > 1 &&
		   s3c_decompress(z, (size_t)n / 2, out, CHUNK) != 0);

	printf("[3] incompressible chunk stays raw\n");
	srand(1);
	for (size_t i = 0; i < CHUNK; i++) {
		src[i] = (char)rand();
	}
	check_true("random data is not stored compressed", s3c_compress(src, CHUNK, z, cap) < 0);

	free(src);
	free(out);
	free(z);
}

int
main(void)
{
	test_keys();
	test_round_trip();
	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
