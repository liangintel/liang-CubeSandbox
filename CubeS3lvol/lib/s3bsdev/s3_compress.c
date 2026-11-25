/* Copyright (c) 2026 Tencent Inc.
 * SPDX-License-Identifier: Apache-2.0 */
/*
 *   Chunk-object LZ4 compression: worker pool, codec selection, counters.
 *   See s3_compress.h.
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"

#include <lz4.h>

#include "s3lvol/s3_spawner.h"
#include "s3_compress.h"
#ifdef S3LVOL_WITH_QAT
#include "s3_qat_lz4.h"
#endif

/* Below this QAT costs more than it saves; liblz4 handles it. */
#define S3C_QAT_MIN_LEN   (64u << 10)
#define S3C_MAX_THREADS   64

struct s3c_work {
	void (*fn)(void *);
	void *arg;
	struct s3c_work *next;
};

struct s3c_counters {
	uint64_t comp_qat, comp_sw, comp_raw, comp_in, comp_out;
	uint64_t decomp_qat, decomp_sw, decomp_err, decomp_in, decomp_out;
};

static bool g_enabled;
static bool g_decode;
static bool g_use_qat;
static volatile int g_qat_ready;
static int g_nthreads = 4;
static volatile bool g_stop;
static const char *g_stats_file;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static struct s3c_work *g_head, *g_tail;

static struct s3c_counters g_cnt;

#define CNT_ADD(f, v) __atomic_fetch_add(&g_cnt.f, (v), __ATOMIC_RELAXED)

bool
s3c_enabled(void)
{
	return g_enabled;
}

bool
s3c_decode_enabled(void)
{
	return g_decode;
}

static bool
is_uuid(const char *s, size_t len)
{
	if (len != 36) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		bool dash = (i == 8 || i == 13 || i == 18 || i == 23);
		if (dash ? s[i] != '-' : !isxdigit((unsigned char)s[i])) {
			return false;
		}
	}
	return true;
}

/* <prefix>/data/<uuid> or <prefix>/exports/<uuid>/<16 hex>. Everything else
 * (meta/, checkpoints, manifests, leases) is JSON or small and stays raw. */
bool
s3c_key_is_chunk(const char *key)
{
	const char *last = strrchr(key, '/');
	const char *prev;
	size_t n;

	if (!last || last == key) {
		return false;
	}
	n = strlen(last + 1);
	for (prev = last - 1; prev > key && *prev != '/'; prev--) {
	}
	if (*prev != '/') {
		return false;
	}
	/* prev .. last is "/<component>" */
	if ((size_t)(last - prev - 1) == 4 && !strncmp(prev + 1, "data", 4)) {
		return is_uuid(last + 1, n);
	}
	if (n == 16 && is_uuid(prev + 1, (size_t)(last - prev - 1)) &&
	    prev - key >= 8 && !strncmp(prev - 8, "/exports", 8)) {
		for (size_t i = 0; i < 16; i++) {
			if (!isxdigit((unsigned char)last[1 + i])) {
				return false;
			}
		}
		return true;
	}
	return false;
}

size_t
s3c_bound(size_t len)
{
	return (size_t)LZ4_compressBound((int)len);
}

int
s3c_compress(const void *src, size_t len, void *dst, size_t cap)
{
	int n = -1;

	if (len == 0 || len > INT32_MAX || cap > INT32_MAX) {
		return -1;
	}
#ifdef S3LVOL_WITH_QAT
	if (g_use_qat && __atomic_load_n(&g_qat_ready, __ATOMIC_ACQUIRE) &&
	    len >= S3C_QAT_MIN_LEN) {
		n = qlz4_compress(src, (int)len, dst, (int)cap);
		if (n > 0) {
			CNT_ADD(comp_qat, 1);
		}
	}
#endif
	if (n <= 0) {
		n = LZ4_compress_default(src, dst, (int)len, (int)cap);
		if (n > 0) {
			CNT_ADD(comp_sw, 1);
		}
	}
	CNT_ADD(comp_in, len);
	if (n <= 0 || (size_t)n >= len) {
		CNT_ADD(comp_raw, 1);
		CNT_ADD(comp_out, len);
		return -1;
	}
	CNT_ADD(comp_out, n);
	return n;
}

int
s3c_decompress(const void *src, size_t len, void *dst, size_t raw_len)
{
	int n = -1;

	if (len == 0 || len > INT32_MAX || raw_len > INT32_MAX) {
		return -1;
	}
#ifdef S3LVOL_WITH_QAT
	if (g_use_qat && __atomic_load_n(&g_qat_ready, __ATOMIC_ACQUIRE) &&
	    raw_len >= S3C_QAT_MIN_LEN) {
		/* QAT rejects some blocks it did not write itself; liblz4 retries. */
		n = qlz4_decompress(src, (int)len, dst, (int)raw_len);
		if (n == (int)raw_len) {
			CNT_ADD(decomp_qat, 1);
		} else {
			n = -1;
		}
	}
#endif
	if (n < 0) {
		n = LZ4_decompress_safe(src, dst, (int)len, (int)raw_len);
		if (n == (int)raw_len) {
			CNT_ADD(decomp_sw, 1);
		}
	}
	if (n != (int)raw_len) {
		CNT_ADD(decomp_err, 1);
		return -1;
	}
	CNT_ADD(decomp_in, len);
	CNT_ADD(decomp_out, raw_len);
	return 0;
}

int
s3c_submit(void (*fn)(void *), void *arg)
{
	struct s3c_work *w = malloc(sizeof(*w));

	if (!w) {
		return -ENOMEM;
	}
	w->fn = fn;
	w->arg = arg;
	w->next = NULL;
	pthread_mutex_lock(&g_lock);
	if (g_tail) {
		g_tail->next = w;
	} else {
		g_head = w;
	}
	g_tail = w;
	pthread_cond_signal(&g_cond);
	pthread_mutex_unlock(&g_lock);
	return 0;
}

static void *
s3c_worker(void *arg)
{
	char name[16];

	pthread_detach(pthread_self());
	snprintf(name, sizeof(name), "s3c_wk%d", (int)(intptr_t)arg);
	pthread_setname_np(pthread_self(), name);

	for (;;) {
		struct s3c_work *w;

		pthread_mutex_lock(&g_lock);
		while (!g_head && !g_stop) {
			pthread_cond_wait(&g_cond, &g_lock);
		}
		if (!g_head) {
			pthread_mutex_unlock(&g_lock);
			break;
		}
		w = g_head;
		g_head = w->next;
		if (!g_head) {
			g_tail = NULL;
		}
		pthread_mutex_unlock(&g_lock);

		w->fn(w->arg);
		free(w);
	}
	return NULL;
}

static void
s3c_write_stats(void)
{
	struct s3c_counters c;
	char tmp[PATH_MAX];
	FILE *f;

	memcpy(&c, &g_cnt, sizeof(c));
	snprintf(tmp, sizeof(tmp), "%s.tmp", g_stats_file);
	f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	fprintf(f, "qat_ready %d\ncomp_qat %" PRIu64 "\ncomp_sw %" PRIu64
		"\ncomp_raw %" PRIu64 "\ncomp_in %" PRIu64 "\ncomp_out %" PRIu64
		"\ndecomp_qat %" PRIu64 "\ndecomp_sw %" PRIu64 "\ndecomp_err %" PRIu64
		"\ndecomp_in %" PRIu64 "\ndecomp_out %" PRIu64 "\n",
		g_qat_ready, c.comp_qat, c.comp_sw, c.comp_raw, c.comp_in, c.comp_out,
		c.decomp_qat, c.decomp_sw, c.decomp_err, c.decomp_in, c.decomp_out);
	fclose(f);
	rename(tmp, g_stats_file);
}

static void *
s3c_aux(void *arg)
{
	(void)arg;
	pthread_detach(pthread_self());
	pthread_setname_np(pthread_self(), "s3c_aux");

#ifdef S3LVOL_WITH_QAT
	if (g_use_qat) {
		int rc;

		/* qatlib start-up takes seconds; liblz4 covers until it is ready.
		 * One instance per worker: each call holds one for its duration. */
		rc = qlz4_init(1, g_nthreads);
		if (rc > 0) {
			__atomic_store_n(&g_qat_ready, 1, __ATOMIC_RELEASE);
			SPDK_NOTICELOG("s3c: QAT ready, %d dc instance(s)\n", rc);
		} else {
			SPDK_ERRLOG("s3c: QAT init failed (%d); using liblz4\n", rc);
		}
	}
#endif
	while (g_stats_file && !g_stop) {
		s3c_write_stats();
		sleep(1);
	}
	return NULL;
}

int
s3c_init(void)
{
	const char *e = getenv("S3LVOL_COMPRESS");
	bool write_lz4;
	int rc;

	if (!e || !strcasecmp(e, "none")) {
		return 0;
	}
	write_lz4 = !strcasecmp(e, "lz4");
	if (!write_lz4 && strcasecmp(e, "decode")) {
		SPDK_ERRLOG("s3c: S3LVOL_COMPRESS must be none|lz4|decode, got '%s'\n", e);
		return -EINVAL;
	}
	if ((e = getenv("S3LVOL_COMPRESS_THREADS"))) {
		g_nthreads = atoi(e);
	}
	if (g_nthreads < 1 || g_nthreads > S3C_MAX_THREADS) {
		SPDK_ERRLOG("s3c: S3LVOL_COMPRESS_THREADS must be 1..%d\n", S3C_MAX_THREADS);
		return -EINVAL;
	}
	e = getenv("S3LVOL_COMPRESS_QAT");
	g_use_qat = e && atoi(e) > 0;
#ifndef S3LVOL_WITH_QAT
	if (g_use_qat) {
		SPDK_WARNLOG("s3c: built without QAT; S3LVOL_COMPRESS_QAT ignored\n");
		g_use_qat = false;
	}
#endif
	g_stats_file = getenv("S3LVOL_COMPRESS_STATS_FILE");

	/* Through the spawner so the workers avoid the reactor cores. */
	for (int i = 0; i < g_nthreads; i++) {
		rc = s3_spawner_pthread_create_async(s3c_worker, (void *)(intptr_t)i, NULL, NULL);
		if (rc != 0) {
			SPDK_ERRLOG("s3c: cannot start worker %d: %d\n", i, rc);
			return rc;
		}
	}
	if (g_use_qat || g_stats_file) {
		rc = s3_spawner_pthread_create_async(s3c_aux, NULL, NULL, NULL);
		if (rc != 0) {
			return rc;
		}
	}
	g_enabled = write_lz4;
	g_decode = true;
	SPDK_NOTICELOG("s3c: LZ4 %s for chunk objects, %d worker(s), %s\n",
		       write_lz4 ? "write+read" : "read only", g_nthreads,
		       g_use_qat ? "QAT (liblz4 fallback)" : "liblz4");
	return 0;
}

void
s3c_fini(void)
{
	pthread_mutex_lock(&g_lock);
	g_stop = true;
	pthread_cond_broadcast(&g_cond);
	pthread_mutex_unlock(&g_lock);
}
