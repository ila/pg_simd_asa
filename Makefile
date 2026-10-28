MODULE_big = simd_asa
OBJS = src/asa_common.o src/asa_count.o src/asa_sum.o src/asa_minmax.o src/asa_list.o src/asa_lanes.o src/asa_rewrite.o

EXTENSION = simd_asa
DATA = simd_asa--0.1.sql simd_asa--0.1--0.2.sql
DATA_built = simd_asa--0.2.sql

REGRESS = hash count sum minmax avg_list parallel rewrite
REGRESS_OPTS = --inputdir=test --outputdir=test

PG_CFLAGS = -O3 -std=gnu11 -Wall -Wno-declaration-after-statement
PG_CPPFLAGS = -I$(srcdir)/src

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# The 0.2 install script is the 0.1 script followed by the 0.1 -> 0.2 upgrade.
simd_asa--0.2.sql: simd_asa--0.1.sql simd_asa--0.1--0.2.sql
	cat $^ > $@
