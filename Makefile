MODULE_big = simd_asa
OBJS = src/asa_common.o src/asa_count.o src/asa_sum.o src/asa_minmax.o src/asa_list.o src/asa_lanes.o src/asa_noise.o src/asa_distinct.o src/asa_rewrite.o

EXTENSION = simd_asa
DATA = sql/simd_asa--0.1.sql

REGRESS = hash count sum minmax avg_list parallel noise rewrite release distinct
REGRESS_OPTS = --inputdir=test --outputdir=test

PG_CFLAGS = -O3 -std=gnu11 -Wall -Wno-declaration-after-statement
PG_CPPFLAGS = -I$(srcdir)/src

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
