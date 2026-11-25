# Copyright (c) 2026 Tencent Inc.
# SPDX-License-Identifier: Apache-2.0
#
#  lib/s3bsdev -- the core library
#
#  Dependency constraints (enforced by the CI check):
#    1. Nothing in this directory may include a header from module/ (it must
#       stay unit-testable on its own).
#    2. Only s3_client_aws.c may include <aws/...>.
#

include ../../mk/s3lvol.common.mk

LIBNAME = s3bsdev

SRCS = s3_bs_dev.c \
       s3_local_dev.c \
       s3_spawner.c \
       s3_export_bs_dev.c \
       s3_chunk_map.c \
       s3_wal.c \
       s3_overlay.c \
       s3_cache.c \
       s3_writeback.c \
       s3_flusher.c \
       s3_checkpoint.c \
       s3_journal.c \
       s3_owner.c \
       s3_export.c \
       s3_export_ref.c \
       s3_get_budget.c \
       s3_compress.c \
       s3_client_aws.c

# S3LVOL_QAT=1 adds the QAT LZ4 path (qatlib); liblz4 is always the fallback.
ifeq ($(S3LVOL_QAT),1)
SRCS += s3_qat_lz4.c
COMMON_CFLAGS += -DS3LVOL_WITH_QAT
endif

OBJS = $(SRCS:.c=.o)
PIC_OBJS = $(SRCS:.c=.pic.o)
DEPS = $(OBJS:.o=.d) $(PIC_OBJS:.o=.d)

.PHONY: shared static clean

shared: lib$(LIBNAME).so

static: lib$(LIBNAME).a

lib$(LIBNAME).so: $(PIC_OBJS)
	$(CC) $(COMMON_CFLAGS) -shared $^ $(AWS_LIBS) -o $@

lib$(LIBNAME).a: $(OBJS)
	@# Recreated, not updated: S3LVOL_QAT=0 must drop s3_qat_lz4.o from it.
	rm -f $@
	$(AR) rcs $@ $^

%.pic.o: %.c
	$(CC) $(COMMON_CFLAGS) -fPIC -c $< -o $@

%.o: %.c
	$(CC) $(COMMON_CFLAGS) -c $< -o $@

# The build flags are not part of the .o dependencies, so switching
# S3LVOL_BUILD_TYPE has to go through this stamp (see mk/).
$(OBJS) $(PIC_OBJS): $(S3LVOL_BUILD_STAMP)

# Header dependencies recorded by -MMD (see mk/s3lvol.common.mk). Absent on a
# clean tree, hence -include: everything gets compiled that first time anyway.
-include $(DEPS)

clean:
	rm -f *.o *.pic.o *.d lib$(LIBNAME).so lib$(LIBNAME).a
