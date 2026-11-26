# Codebase Module Map

Status legend:
* **Wired (V1)**: Compiled into `Makefile` / `python/setup.py` and active in `vex._vex`.
* **Standalone (V2)**: Implemented and unit-tested under `build/`, not yet linked into `vx_db.c` / `vx_exec.c` / `setup.py`.
* **Mixed**: Modified to carry V2 code alongside V1 entrypoints.

---

## 1. Public V1 Headers (`include/`)

| File | Status | Responsibility |
| :--- | :---: | :--- |
| [include/vx_arena.h](file:///home/avisheks/Documents/dibs/vex/include/vx_arena.h) | Wired (V1) | Single-slab bump arena interface |
| [include/vx_ast.h](file:///home/avisheks/Documents/dibs/vex/include/vx_ast.h) | Wired (V1) | VQL AST nodes, expressions, statements, stage types |
| [include/vx_chunk.h](file:///home/avisheks/Documents/dibs/vex/include/vx_chunk.h) | Wired (V1) | V1 column types (`vx_type_t`), zone maps, columnar chunks |
| [include/vx_db.h](file:///home/avisheks/Documents/dibs/vex/include/vx_db.h) | Wired (V1) | V1 C API (`vx_db_open`, `vx_db_query`, `vx_db_put_row`, `vx_db_checkpoint`, `vx_result_t`) |
| [include/vx_delmap.h](file:///home/avisheks/Documents/dibs/vex/include/vx_delmap.h) | Wired (V1) | Refcounted COW deletion bitmap |
| [include/vx_ebr.h](file:///home/avisheks/Documents/dibs/vex/include/vx_ebr.h) | Wired (V1) | Epoch-Based Reclamation for catalog snapshots |
| [include/vx_lexer.h](file:///home/avisheks/Documents/dibs/vex/include/vx_lexer.h) | Wired (V1) | VQL token types and lexer interface |
| [include/vx_log.h](file:///home/avisheks/Documents/dibs/vex/include/vx_log.h) | Wired (V1) | Async binary SPSC logger API |
| [include/vx_parser.h](file:///home/avisheks/Documents/dibs/vex/include/vx_parser.h) | Wired (V1) | LL(1) parser entrypoint (`vx_parse`) |
| [include/vx_rhmap.h](file:///home/avisheks/Documents/dibs/vex/include/vx_rhmap.h) | Wired (V1) | Robin Hood hash map for sharded PK index |
| [include/vx_simd.h](file:///home/avisheks/Documents/dibs/vex/include/vx_simd.h) | Wired (V1) | V1 SIMD level detection & `vx_filter_f64_avx2_dispatch` |
| [include/vx_snap.h](file:///home/avisheks/Documents/dibs/vex/include/vx_snap.h) | Wired (V1) | Binary `.snap` checkpoint reader/writer |
| [include/vx_spsc.h](file:///home/avisheks/Documents/dibs/vex/include/vx_spsc.h) | Wired (V1) | Lock-free SPSC ring buffer |
| [include/vx_str.h](file:///home/avisheks/Documents/dibs/vex/include/vx_str.h) | Wired (V1) | V1 16B German strings (`vx_str_t`) |
| [include/vx_wal.h](file:///home/avisheks/Documents/dibs/vex/include/vx_wal.h) | Wired (V1) | V1 append-only CRC32 WAL |
| [include/vx_wmesh.h](file:///home/avisheks/Documents/dibs/vex/include/vx_wmesh.h) | Wired (V1) | Per-thread SPSC write mesh coordinator |

---

## 2. Engine Sources & V2 Headers (`src/`)

### Active V1 Engine Files (Wired)
* [vx_arena.c](file:///home/avisheks/Documents/dibs/vex/src/vx_arena.c), [vx_spsc.c](file:///home/avisheks/Documents/dibs/vex/src/vx_spsc.c), [vx_log.c](file:///home/avisheks/Documents/dibs/vex/src/vx_log.c), [vx_wmesh.c](file:///home/avisheks/Documents/dibs/vex/src/vx_wmesh.c) — M1/M2 queues and async ingestion.
* [vx_str.c](file:///home/avisheks/Documents/dibs/vex/src/vx_str.c), [vx_chunk.c](file:///home/avisheks/Documents/dibs/vex/src/vx_chunk.c), [vx_ebr.c](file:///home/avisheks/Documents/dibs/vex/src/vx_ebr.c), [vx_wal.c](file:///home/avisheks/Documents/dibs/vex/src/vx_wal.c) — M3 storage primitives.
* [vx_simd.c](file:///home/avisheks/Documents/dibs/vex/src/vx_simd.c) — V1 runtime SIMD level detection (`vx_simd_detect` / `vx_simd_active`).
* [vx_simd_avx2.c](file:///home/avisheks/Documents/dibs/vex/src/vx_simd_avx2.c) (**Mixed**) — Contains both V2 AVX2 kernels (lines 1–600) and V1 `vx_filter_f64_avx2_dispatch` (lines 602–680).
* [vx_lexer.c](file:///home/avisheks/Documents/dibs/vex/src/vx_lexer.c), [vx_parser.c](file:///home/avisheks/Documents/dibs/vex/src/vx_parser.c) — M4 lexer & LL(1) parser.
* [vx_snap.c](file:///home/avisheks/Documents/dibs/vex/src/vx_snap.c), [vx_rhmap.c](file:///home/avisheks/Documents/dibs/vex/src/vx_rhmap.c), [vx_delmap.c](file:///home/avisheks/Documents/dibs/vex/src/vx_delmap.c), [vx_internal.h](file:///home/avisheks/Documents/dibs/vex/src/vx_internal.h), [vx_db.c](file:///home/avisheks/Documents/dibs/vex/src/vx_db.c), [vx_exec.c](file:///home/avisheks/Documents/dibs/vex/src/vx_exec.c) — V1 database lifecycle, worker pool, query executor (still contains `materialize_table()`).

### Standalone V2 Foundation & Execution (`M11`–`M12`)
* [vx_types.h](file:///home/avisheks/Documents/dibs/vex/src/vx_types.h), [vx_owner.h](file:///home/avisheks/Documents/dibs/vex/src/vx_owner.h), [vx_platform_win32.h](file:///home/avisheks/Documents/dibs/vex/src/vx_platform_win32.h), [vx_alloc.h](file:///home/avisheks/Documents/dibs/vex/src/vx_alloc.h) — Base ABI, intrusive atomic refcount, OS shims, OOM injection shim.
* [vx_arena.h](file:///home/avisheks/Documents/dibs/vex/src/vx_arena.h) + [vx_arena_v2.c](file:///home/avisheks/Documents/dibs/vex/src/vx_arena_v2.c) — Refcounted chained-slab arena.
* [vx_str.h](file:///home/avisheks/Documents/dibs/vex/src/vx_str.h) + [vx_str_v2.c](file:///home/avisheks/Documents/dibs/vex/src/vx_str_v2.c) — 16B `vx_strview_t` and `vx_strpool_t`.
* [vx_chunk.h](file:///home/avisheks/Documents/dibs/vex/src/vx_chunk.h) + [vx_delmap_v2.c](file:///home/avisheks/Documents/dibs/vex/src/vx_delmap_v2.c) — V2 chunk/delmap structs + branchless live selection extractor.
* [vx_batch.h](file:///home/avisheks/Documents/dibs/vex/src/vx_batch.h) + [vx_batch.c](file:///home/avisheks/Documents/dibs/vex/src/vx_batch.c) + [vx_batch_compact.c](file:///home/avisheks/Documents/dibs/vex/src/vx_batch_compact.c) — 4,096-row batch containers & compaction.
* [vx_op.h](file:///home/avisheks/Documents/dibs/vex/src/vx_op.h) + [vx_pipeline.c](file:///home/avisheks/Documents/dibs/vex/src/vx_pipeline.c) — Morsel push pipeline runner.
* [vx_op_scan.c](file:///home/avisheks/Documents/dibs/vex/src/vx_op_scan.c), [vx_op_filter.h](file:///home/avisheks/Documents/dibs/vex/src/vx_op_filter.h), [vx_op_filter.c](file:///home/avisheks/Documents/dibs/vex/src/vx_op_filter.c), [vx_op_project.h](file:///home/avisheks/Documents/dibs/vex/src/vx_op_project.h), [vx_op_project.c](file:///home/avisheks/Documents/dibs/vex/src/vx_op_project.c) — Scan, Filter, Project operators.
* [vx_result.h](file:///home/avisheks/Documents/dibs/vex/src/vx_result.h) + [vx_sink_result.c](file:///home/avisheks/Documents/dibs/vex/src/vx_sink_result.c) — Refcounted multi-batch result collector & contiguous column materializer.
* [vx_simd.h](file:///home/avisheks/Documents/dibs/vex/src/vx_simd.h), [vx_simd_num_common.h](file:///home/avisheks/Documents/dibs/vex/src/vx_simd_num_common.h), [vx_simd_str_common.h](file:///home/avisheks/Documents/dibs/vex/src/vx_simd_str_common.h), [vx_simd_neon.c](file:///home/avisheks/Documents/dibs/vex/src/vx_simd_neon.c), [vx_builtin.c](file:///home/avisheks/Documents/dibs/vex/src/vx_builtin.c) — V2 SIMD vtable interface, scalar fallbacks, NEON kernels, and 8 scalar built-ins.
* [vx_sink_hash_agg.h](file:///home/avisheks/Documents/dibs/vex/src/vx_sink_hash_agg.h) + [vx_sink_hash_agg.c](file:///home/avisheks/Documents/dibs/vex/src/vx_sink_hash_agg.c) — 64-way partitioned in-memory hash aggregation.
* [vx_hash_join.h](file:///home/avisheks/Documents/dibs/vex/src/vx_hash_join.h) + [vx_sink_hj_build.c](file:///home/avisheks/Documents/dibs/vex/src/vx_sink_hj_build.c) + [vx_op_hj_probe.c](file:///home/avisheks/Documents/dibs/vex/src/vx_op_hj_probe.c) — Streaming 1:N hash join (`INNER`, `LEFT`, `SEMI`, `ANTI`).
* [vx_sort.h](file:///home/avisheks/Documents/dibs/vex/src/vx_sort.h) + [vx_sink_topk.c](file:///home/avisheks/Documents/dibs/vex/src/vx_sink_topk.c) + [vx_sink_sort.c](file:///home/avisheks/Documents/dibs/vex/src/vx_sink_sort.c) — Bounded Top-K heap and disk-spilling k-way merge sort.

### Standalone V2 Storage, Indexes, Optimizer & Transactions (`M13`–`M17`)
* [vx_seg_fmt.h](file:///home/avisheks/Documents/dibs/vex/src/vx_seg_fmt.h), [vx_seg.h](file:///home/avisheks/Documents/dibs/vex/src/vx_seg.h), [vx_seg.c](file:///home/avisheks/Documents/dibs/vex/src/vx_seg.c) — `.vx` (`VXSEG02`) segment writer & zero-deserialization `mmap` reader.
* [vx_manifest_fmt.h](file:///home/avisheks/Documents/dibs/vex/src/vx_manifest_fmt.h), [vx_manifest.h](file:///home/avisheks/Documents/dibs/vex/src/vx_manifest.h), [vx_manifest.c](file:///home/avisheks/Documents/dibs/vex/src/vx_manifest.c) — Atomic `MANIFEST`, versioned `.del` tombstones, orphan sweeper, recovery state machine.
* [vx_bg.h](file:///home/avisheks/Documents/dibs/vex/src/vx_bg.h), [vx_flusher.c](file:///home/avisheks/Documents/dibs/vex/src/vx_flusher.c), [vx_compact.c](file:///home/avisheks/Documents/dibs/vex/src/vx_compact.c) — `max_memory_bytes` hot RAM accounting/backpressure, background flusher, leveled compactor.
* [vx_enc.h](file:///home/avisheks/Documents/dibs/vex/src/vx_enc.h), [vx_enc.c](file:///home/avisheks/Documents/dibs/vex/src/vx_enc.c) — FoR+Bitpack, String Dictionary, and RLE compression.
* [vx_art_fmt.h](file:///home/avisheks/Documents/dibs/vex/src/vx_art_fmt.h), [vx_art.c](file:///home/avisheks/Documents/dibs/vex/src/vx_art.c), [vx_idx.h](file:///home/avisheks/Documents/dibs/vex/src/vx_idx.h), [vx_idx.c](file:///home/avisheks/Documents/dibs/vex/src/vx_idx.c) — Companion `.idx` Robin Hood hash and ART secondary indexes.
* [vx_plan.h](file:///home/avisheks/Documents/dibs/vex/src/vx_plan.h), [vx_plan.c](file:///home/avisheks/Documents/dibs/vex/src/vx_plan.c) — Logical IR, optimizer passes, access path selection, `?` explain formatter.
* [vx_tx.h](file:///home/avisheks/Documents/dibs/vex/src/vx_tx.h), [vx_tx.c](file:///home/avisheks/Documents/dibs/vex/src/vx_tx.c), [vx_failpoint.h](file:///home/avisheks/Documents/dibs/vex/src/vx_failpoint.h) — Framed atomic WAL transactions (`VX_WAL_OP_TX_BATCH`), OCC conflict detection, crash failpoint macros.

---

## 3. Python Layer (`python/`)

| File | Status | Purpose |
| :--- | :---: | :--- |
| [python/setup.py](file:///home/avisheks/Documents/dibs/vex/python/setup.py) | Wired (V1) | Builds `vex._vex` from V1 sources + `vex_py.c` with isolated `-mavx2` on `vx_simd_avx2.c` |
| [python/vex_py.c](file:///home/avisheks/Documents/dibs/vex/python/vex_py.c) | Hybrid | PEP 489 / `Py_MOD_GIL_NOT_USED` module wrapping V1 `vx_db.h` + Python-side V2 shims |
| [python/vex_arrow.h](file:///home/avisheks/Documents/dibs/vex/python/vex_arrow.h), [python/vex_arrow.c](file:///home/avisheks/Documents/dibs/vex/python/vex_arrow.c) | Standalone | Zero-copy Arrow C Data & Stream interface (unwired from `setup.py` and `vex_py.c`) |
| [python/vex/__init__.py](file:///home/avisheks/Documents/dibs/vex/python/vex/__init__.py), [python/vex/__init__.pyi](file:///home/avisheks/Documents/dibs/vex/python/vex/__init__.pyi), [python/vex/py.typed](file:///home/avisheks/Documents/dibs/vex/python/vex/py.typed) | Wired | PEP 561 typed wrapper package re-exporting `vex._vex` |
| [python/vex.pyi](file:///home/avisheks/Documents/dibs/vex/python/vex.pyi), [python/py.typed](file:///home/avisheks/Documents/dibs/vex/python/py.typed) | Legacy | Top-level stubs (identical to `python/vex/__init__.pyi`; referenced by CI & `test_m9_release.py`) |
| [python/vex_cli.py](file:///home/avisheks/Documents/dibs/vex/python/vex_cli.py) | Wired (V1) | Zero-dependency interactive REPL & batch CLI |
| [python/pyproject.toml](file:///home/avisheks/Documents/dibs/vex/python/pyproject.toml) | V1 | Package metadata (`version = "0.1.0"`, `requires-python = ">=3.10"`) |

---

## 4. Tests, Fuzzers & Benchmarks

* **V1 C Tests (`Makefile`):** `tests/test_m1_core.c`, `test_m2_queues.c`, `test_m3_storage.c`, `test_m4_parser.c`, `test_m5_engine.c`, `test_m7_p0.c`, `test_m8_perf.c`, `test_m9_release.c`, `test_m10_audit.c` (using [tests/vx_test.h](file:///home/avisheks/Documents/dibs/vex/tests/vx_test.h)).
* **V1 Python & CLI Tests:** `tests/test_m6_python.py`, `test_m7_p0.py`, `test_m9_release.py`, `test_m10_audit.py`, `test_cli.py`.
* **V2 Standalone C Tests (no `Makefile` targets yet; compiled into `build/`):** `tests/test_v2_abi.c`, `test_v2_str_simd.c`, `test_v2_filter_project.c`, `test_v2_num_simd.c`, `test_v2_pipeline.c`, `test_v2_hash_agg_builtin.c`, `test_v2_hash_join.c`, `test_v2_sort.c`, `test_v2_seg_man_fmt.c`, `test_v2_seg.c`, `test_v2_manifest.c`, `test_v2_bg.c`, `test_v2_enc.c`, `test_v2_idx_plan.c`, `test_v2_art.c`, `test_v2_tx.c`, `test_oom_sweep.c`, `test_tsan_c_core.c`.
* **V2 Python Torture & Crash Suites:** [tests/test_crash_loop.py](file:///home/avisheks/Documents/dibs/vex/tests/test_crash_loop.py), [tests/test_tsan_313t_torture.py](file:///home/avisheks/Documents/dibs/vex/tests/test_tsan_313t_torture.py).
* **Fuzz & Benchmarks:** [fuzz/](file:///home/avisheks/Documents/dibs/vex/fuzz), [bench/](file:///home/avisheks/Documents/dibs/vex/bench), [benchmarks/](file:///home/avisheks/Documents/dibs/vex/benchmarks).

