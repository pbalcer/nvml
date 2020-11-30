# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2020, Intel Corporation
#

#
# src/test/pmem2_source_numa/Makefile -- build pmem2_source_numa unit test
#
TOP = ../../..

vpath %.c $(TOP)/src/test/unittest
vpath %.c $(TOP)/src/libpmem2
# vpath %.c $(TOP)/src/common/

INCS += -I$(TOP)/src/libpmem2
TARGET = pmem2_source_numa
OBJS += errormsg.o\
	pmem2_source_numa.o\
	ut_pmem2_config.o\
	ut_pmem2_source.o\
	ut_pmem2_utils.o

LIBPMEMCOMMON=y
include ../Makefile.inc
