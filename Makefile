CC ?= gcc
CFLAGS_BASE := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wpedantic -Iinclude -pthread
CFLAGS_MEM  := $(CFLAGS_BASE) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS_RACE := $(CFLAGS_BASE) -O1 -g -fsanitize=thread -fno-omit-frame-pointer
CFLAGS_OPT  := $(CFLAGS_BASE) -O3 -march=native -DNDEBUG
UNAME_M     := $(shell uname -m)
CFLAGS_PORTABLE := $(CFLAGS_BASE) -O3 -DNDEBUG
CFLAGS_AVX2     := $(if $(filter x86_64 amd64,$(UNAME_M)),-mavx2,)

BUILD_DIR := build
OBJ_AVX2_MEM  := $(BUILD_DIR)/vx_simd_avx2_mem.o
OBJ_AVX2_RACE := $(BUILD_DIR)/vx_simd_avx2_race.o
OBJ_AVX2_OPT  := $(BUILD_DIR)/vx_simd_avx2_opt.o

SRC_M1    := src/vx_arena.c src/vx_spsc.c
SRC_M2    := $(SRC_M1) src/vx_log.c src/vx_wmesh.c
SRC_M3    := $(SRC_M1) src/vx_str.c src/vx_chunk.c src/vx_simd.c src/vx_simd_avx2.c src/vx_ebr.c src/vx_wal.c
SRC_M4    := src/vx_arena.c src/vx_lexer.c src/vx_parser.c
SRC_M5    := $(SRC_M2) src/vx_str.c src/vx_chunk.c src/vx_simd.c src/vx_ebr.c src/vx_wal.c \
             src/vx_snap.c src/vx_rhmap.c src/vx_delmap.c src/vx_lexer.c src/vx_parser.c \
             src/vx_db.c src/vx_exec.c
SRC_M8    := $(SRC_M5)
SRC_M9_COMMON := $(SRC_M8)
SETARCH   := $(shell command -v setarch >/dev/null 2>&1 && echo "setarch $$(uname -m) -R")

.PHONY: all test-mem test-race bench test-m2-mem test-m2-race bench-m2 test-m3-mem test-m3-race bench-m3 test-m4-mem test-m4-race bench-m4 test-m5-mem test-m5-race bench-m5 test-m6 clean

all: test-mem test-race bench test-m2-mem test-m2-race bench-m2 test-m3-mem test-m3-race bench-m3 test-m4-mem test-m4-race bench-m4 test-m5-mem test-m5-race bench-m5 test-m6

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OBJ_AVX2_MEM): src/vx_simd_avx2.c | $(BUILD_DIR)
	$(CC) $(CFLAGS_MEM) $(CFLAGS_AVX2) -c $< -o $@

$(OBJ_AVX2_RACE): src/vx_simd_avx2.c | $(BUILD_DIR)
	$(CC) $(CFLAGS_RACE) $(CFLAGS_AVX2) -c $< -o $@

$(OBJ_AVX2_OPT): src/vx_simd_avx2.c | $(BUILD_DIR)
	$(CC) $(CFLAGS_PORTABLE) $(CFLAGS_AVX2) -c $< -o $@

test-mem: $(BUILD_DIR)
	$(CC) $(CFLAGS_MEM) $(SRC_M1) tests/test_m1_core.c -o $(BUILD_DIR)/test_m1_mem
	./$(BUILD_DIR)/test_m1_mem

test-race: $(BUILD_DIR)
	$(CC) $(CFLAGS_RACE) $(SRC_M1) tests/test_m1_core.c -o $(BUILD_DIR)/test_m1_race
	$(SETARCH) ./$(BUILD_DIR)/test_m1_race

bench: $(BUILD_DIR)
	$(CC) $(CFLAGS_OPT) $(SRC_M1) tests/test_m1_core.c -o $(BUILD_DIR)/test_m1_bench
	./$(BUILD_DIR)/test_m1_bench

test-m2-mem: $(BUILD_DIR)
	$(CC) $(CFLAGS_MEM) $(SRC_M2) tests/test_m2_queues.c -o $(BUILD_DIR)/test_m2_mem
	./$(BUILD_DIR)/test_m2_mem

test-m2-race: $(BUILD_DIR)
	$(CC) $(CFLAGS_RACE) $(SRC_M2) tests/test_m2_queues.c -o $(BUILD_DIR)/test_m2_race
	$(SETARCH) ./$(BUILD_DIR)/test_m2_race

bench-m2: $(BUILD_DIR)
	$(CC) $(CFLAGS_OPT) $(SRC_M2) tests/test_m2_queues.c -o $(BUILD_DIR)/test_m2_bench
	./$(BUILD_DIR)/test_m2_bench

test-m3-mem: $(BUILD_DIR)
	$(CC) $(CFLAGS_MEM) $(SRC_M3) tests/test_m3_storage.c -o $(BUILD_DIR)/test_m3_mem
	./$(BUILD_DIR)/test_m3_mem

test-m3-race: $(BUILD_DIR)
	$(CC) $(CFLAGS_RACE) $(SRC_M3) tests/test_m3_storage.c -o $(BUILD_DIR)/test_m3_race
	$(SETARCH) ./$(BUILD_DIR)/test_m3_race

bench-m3: $(BUILD_DIR)
	$(CC) $(CFLAGS_OPT) $(SRC_M3) tests/test_m3_storage.c -o $(BUILD_DIR)/test_m3_bench
	./$(BUILD_DIR)/test_m3_bench

test-m4-mem: $(BUILD_DIR)
	$(CC) $(CFLAGS_MEM) $(SRC_M4) tests/test_m4_parser.c -o $(BUILD_DIR)/test_m4_mem
	./$(BUILD_DIR)/test_m4_mem

test-m4-race: $(BUILD_DIR)
	$(CC) $(CFLAGS_RACE) $(SRC_M4) tests/test_m4_parser.c -o $(BUILD_DIR)/test_m4_race
	$(SETARCH) ./$(BUILD_DIR)/test_m4_race

bench-m4: $(BUILD_DIR)
	$(CC) $(CFLAGS_OPT) $(SRC_M4) tests/test_m4_parser.c -o $(BUILD_DIR)/test_m4_bench
	./$(BUILD_DIR)/test_m4_bench

.PHONY: test-m5-mem test-m5-race bench-m5

test-m5-mem: $(BUILD_DIR) $(OBJ_AVX2_MEM)
	$(CC) $(CFLAGS_MEM) $(SRC_M5) $(OBJ_AVX2_MEM) tests/test_m5_engine.c -o $(BUILD_DIR)/test_m5_mem -lm
	./$(BUILD_DIR)/test_m5_mem

test-m5-race: $(BUILD_DIR) $(OBJ_AVX2_RACE)
	$(CC) $(CFLAGS_RACE) $(SRC_M5) $(OBJ_AVX2_RACE) tests/test_m5_engine.c -o $(BUILD_DIR)/test_m5_race -lm
	$(SETARCH) ./$(BUILD_DIR)/test_m5_race

bench-m5: $(BUILD_DIR) $(OBJ_AVX2_OPT)
	$(CC) $(CFLAGS_OPT) $(SRC_M5) $(OBJ_AVX2_OPT) tests/test_m5_engine.c -o $(BUILD_DIR)/test_m5_bench -lm
	./$(BUILD_DIR)/test_m5_bench

PYTHON ?= python3

.PHONY: build-py test-m6

build-py:
	cd python && $(PYTHON) setup.py build_ext --inplace

test-m6: build-py
	PYTHONPATH=python $(PYTHON) -m pytest -v tests/test_m6_python.py

.PHONY: test-m7-mem test-m7-race test-m7-py

test-m7-mem: $(BUILD_DIR) $(OBJ_AVX2_MEM)
	$(CC) $(CFLAGS_MEM) $(SRC_M5) $(OBJ_AVX2_MEM) tests/test_m7_p0.c -o $(BUILD_DIR)/test_m7_mem -lm
	./$(BUILD_DIR)/test_m7_mem

test-m7-race: $(BUILD_DIR) $(OBJ_AVX2_RACE)
	$(CC) $(CFLAGS_RACE) $(SRC_M5) $(OBJ_AVX2_RACE) tests/test_m7_p0.c -o $(BUILD_DIR)/test_m7_race -lm
	$(SETARCH) ./$(BUILD_DIR)/test_m7_race

test-m7-py: build-py
	PYTHONPATH=python $(PYTHON) -m pytest -v tests/test_m7_p0.py

.PHONY: test-m8-mem test-m8-race bench-m8

test-m8-mem: $(BUILD_DIR) $(OBJ_AVX2_MEM)
	$(CC) $(CFLAGS_MEM) $(SRC_M8) $(OBJ_AVX2_MEM) tests/test_m8_perf.c -o $(BUILD_DIR)/test_m8_mem -lm
	./$(BUILD_DIR)/test_m8_mem

test-m8-race: $(BUILD_DIR) $(OBJ_AVX2_RACE)
	$(CC) $(CFLAGS_RACE) $(SRC_M8) $(OBJ_AVX2_RACE) tests/test_m8_perf.c -o $(BUILD_DIR)/test_m8_race -lm
	$(SETARCH) ./$(BUILD_DIR)/test_m8_race

bench-m8: $(BUILD_DIR) $(OBJ_AVX2_OPT)
	$(CC) $(CFLAGS_OPT) $(SRC_M8) $(OBJ_AVX2_OPT) tests/test_m8_perf.c -o $(BUILD_DIR)/test_m8_bench -lm
	./$(BUILD_DIR)/test_m8_bench

.PHONY: test-m9-mem test-m9-race bench-m9 test-m9-py fuzz-parser fuzz-wal

test-m9-mem: $(BUILD_DIR) $(OBJ_AVX2_MEM)
	$(CC) $(CFLAGS_MEM) $(SRC_M9_COMMON) $(OBJ_AVX2_MEM) tests/test_m9_release.c -o $(BUILD_DIR)/test_m9_mem -lm
	./$(BUILD_DIR)/test_m9_mem

test-m9-race: $(BUILD_DIR) $(OBJ_AVX2_RACE)
	$(CC) $(CFLAGS_RACE) $(SRC_M9_COMMON) $(OBJ_AVX2_RACE) tests/test_m9_release.c -o $(BUILD_DIR)/test_m9_race -lm
	$(SETARCH) ./$(BUILD_DIR)/test_m9_race

bench-m9: $(BUILD_DIR) $(OBJ_AVX2_OPT)
	$(CC) $(CFLAGS_PORTABLE) $(SRC_M9_COMMON) $(OBJ_AVX2_OPT) tests/test_m9_release.c -o $(BUILD_DIR)/test_m9_bench -lm
	./$(BUILD_DIR)/test_m9_bench

test-m9-py: build-py
	PYTHONPATH=python $(PYTHON) -m pytest -v tests/test_m9_release.py

fuzz-parser: $(BUILD_DIR)
	clang -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Iinclude -fsanitize=fuzzer,address,undefined \
		src/vx_arena.c src/vx_lexer.c src/vx_parser.c fuzz/fuzz_parser.c -o $(BUILD_DIR)/fuzz_parser
	./$(BUILD_DIR)/fuzz_parser -max_total_time=15

fuzz-wal: $(BUILD_DIR)
	clang -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Iinclude -fsanitize=fuzzer,address,undefined -pthread \
		src/vx_wal.c fuzz/fuzz_wal.c -o $(BUILD_DIR)/fuzz_wal
	./$(BUILD_DIR)/fuzz_wal -max_total_time=15

.PHONY: test-m10-mem test-m10-race test-m10-py

test-m10-mem: $(BUILD_DIR) $(OBJ_AVX2_MEM)
	$(CC) $(CFLAGS_MEM) $(SRC_M9_COMMON) $(OBJ_AVX2_MEM) tests/test_m10_audit.c -o $(BUILD_DIR)/test_m10_mem -lm
	./$(BUILD_DIR)/test_m10_mem

test-m10-race: $(BUILD_DIR) $(OBJ_AVX2_RACE)
	$(CC) $(CFLAGS_RACE) $(SRC_M9_COMMON) $(OBJ_AVX2_RACE) tests/test_m10_audit.c -o $(BUILD_DIR)/test_m10_race -lm
	$(SETARCH) ./$(BUILD_DIR)/test_m10_race

test-m10-py: build-py
	PYTHONPATH=python $(PYTHON) -m pytest -v tests/test_m10_audit.py

.PHONY: repl test-cli bench-py bench-single-core

repl: build-py
	PYTHONPATH=python $(PYTHON) python/vex_cli.py $(WAL)

test-cli: build-py
	PYTHONPATH=python $(PYTHON) -m pytest -v tests/test_cli.py

bench-py: build-py
	PYTHONPATH=python $(PYTHON) bench/bench_vs_sqlite.py

bench-single-core: build-py
	PYTHONPATH=python $(PYTHON) bench/bench_single_core.py

clean:
	rm -rf $(BUILD_DIR)
