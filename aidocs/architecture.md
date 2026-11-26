# Vex Architecture Overview (V1 Wired + V2 Standalone)

Vex is transitioning from **V1.0** (in-memory columnar engine with single-file WAL and `.snap` checkpoints) to **V2.0** (out-of-core `.vx` segments, `MANIFEST` recovery, 4,096-row vectorized push pipeline, secondary Hash/ART indexes, ACID framed WAL transactions, and Arrow C Data Interface).

Understanding both layers is essential because **both currently coexist in `src/`**.

---

## 1. Header Namespace Collision (Critical Gotcha)

Four header filenames exist in **both** `include/` (V1) and `src/` (V2) with incompatible struct layouts:

* `vx_arena.h` — V1 fixed single-buffer arena vs V2 refcounted chained-slab `vx_arena_t`
* `vx_chunk.h` — V1 `vx_chunk_t` / `vx_val_t` vs V2 `vx_chunk_t` / `vx_delmap_t` / `vx_zone_t`
* `vx_simd.h` — V1 `vx_simd_level_t` enum vs V2 `vx_simd_ops_t` vtable + `vx_builtin_eval()`
* `vx_str.h` — V1 `vx_str_t` vs V2 16-byte Arrow-compatible `vx_strview_t` + `vx_strpool_t`

Because `Makefile` and `python/setup.py` pass `-Iinclude` while C resolves `"..."` relative to the `.c` file's directory first, the V1 `.c` files ([vx_arena.c](file:///home/avisheks/Documents/dibs/vex/src/vx_arena.c), [vx_chunk.c](file:///home/avisheks/Documents/dibs/vex/src/vx_chunk.c), [vx_internal.h](file:///home/avisheks/Documents/dibs/vex/src/vx_internal.h), [vx_simd.c](file:///home/avisheks/Documents/dibs/vex/src/vx_simd.c), [vx_str.c](file:///home/avisheks/Documents/dibs/vex/src/vx_str.c)) explicitly include `../include/vx_*.h`.

Meanwhile, [src/vx_simd_avx2.c](file:///home/avisheks/Documents/dibs/vex/src/vx_simd_avx2.c) includes V2 `"vx_simd_str_common.h"` and `"vx_simd_num_common.h"` (defining V2 AVX2 kernels) and keeps the V1 `vx_filter_f64_avx2_dispatch()` entrypoint at the bottom of the file.

---

## 2. Active V1 Architecture (Compiled & Running Today)

```text
Python (python/vex_py.c, vex._vex)
   │  GIL released via Py_BEGIN_ALLOW_THREADS; PEP 3118 zero-copy ColumnView
   ├──► Query path: vx_db_query() ──► vx_lexer.c + vx_parser.c ──► vx_exec_program()
   │       • Acquires EBR reader snapshot (vx_ebr.c)
   │       • Tries PK fast path / morsel fast path (vx_pool_run)
   │       • General queries call materialize_table() in src/vx_exec.c
   │
   └──► Write path: vx_db_put_row() ──► Per-thread SPSC Write Mesh (vx_wmesh.c + vx_spsc.c)
           • Micro-batch commit worker acquires write_mu
           • Clones COW catalog + 128-way sharded PK Robin Hood map (vx_rhmap.c)
           • Updates chunks / COW deletion bitmaps (vx_delmap.c)
           • Appends CRC32 WAL frame (vx_wal.c), publishes via EBR
           • db.checkpoint() writes atomic .snap (vx_snap.c), compacts RAM, resets WAL
```

### Key V1 Characteristics
* **Lock-free reads via EBR (`vx_ebr.c`)**: Readers never block writers; retired `vx_catalog_t` versions are freed once all reader epochs advance.
* **Write ingestion (`vx_wmesh.c`)**: Per-thread SPSC ring buffers (`vx_spsc.c`) coalesce `db.put()` operations into batched WAL writes under `write_mu`.
* **In-memory ceiling**: All live table data stays in RAM; `materialize_table()` copies rows into working tables (`vx_wtable_t`) for multi-stage queries.
* **Python shims in `python/vex_py.c`**: Accepts `max_memory_bytes` (ignored), provides `db.tx()` via a Python-side OCC mask and SQL replay buffer, translates some V2 pipe/index syntax in `vx_normalize_query()`, and stubs `__arrow_c_stream__` and `_verify_all_segments_crc`.

---

## 3. Standalone V2 Architecture (Implemented in `src/`, Not Yet Wired)

### A. Unified Ownership & Memory (`M11`)
* **`vx_owner_t` ([vx_owner.h](file:///home/avisheks/Documents/dibs/vex/src/vx_owner.h))**: Intrusive atomic refcount header shared by arenas, chunks, delmaps, string buffers, mmap segments, catalogs, results, hash-join tables, and secondary indexes (`VX_OWN_*`).
* **Chained Arena ([vx_arena_v2.c](file:///home/avisheks/Documents/dibs/vex/src/vx_arena_v2.c))**: Growable 64B-aligned slab chain with `bytes_limit` caps and `vx_arena_mark()` / `vx_arena_rewind()`.
* **Arrow-Aligned Strings ([vx_str_v2.c](file:///home/avisheks/Documents/dibs/vex/src/vx_str_v2.c))**: 16-byte `vx_strview_t` (inline `<= 12` bytes; out-of-line `{len, prefix[4], buf_idx, offset}` backed by `vx_strpool_t` + refcounted `vx_strbuf_t`).
* **Immutable Delmaps ([vx_delmap_v2.c](file:///home/avisheks/Documents/dibs/vex/src/vx_delmap_v2.c))**: Fixed 4,096-row COW deletion bitsets with branchless nibble expansion (`vx_delmap_live_sel`).

### B. 4,096-Row Streaming Push Pipeline (`M12`) & Spilling (`M15`)
* **Batch & Vectors ([vx_batch.h](file:///home/avisheks/Documents/dibs/vex/src/vx_batch.h), [vx_batch.c](file:///home/avisheks/Documents/dibs/vex/src/vx_batch.c), [vx_batch_compact.c](file:///home/avisheks/Documents/dibs/vex/src/vx_batch_compact.c))**: `vx_batch_t` carries up to 4,096 rows, `vx_sel_t` selection vector, and columns marked `VX_VEC_DEFERRED`, `VX_VEC_FLAT`, `VX_VEC_CONST`, or `VX_VEC_DICT`.
* **Morsel Pipeline ([vx_op.h](file:///home/avisheks/Documents/dibs/vex/src/vx_op.h), [vx_pipeline.c](file:///home/avisheks/Documents/dibs/vex/src/vx_pipeline.c))**: Source -> `vx_op_t` operators -> `vx_sink_t` (`init_global` / `init_local` / `sink` / `combine` / `finalize`):
  * **Operators:** `VX_SOURCE_TABLE_SCAN` ([vx_op_scan.c](file:///home/avisheks/Documents/dibs/vex/src/vx_op_scan.c)), `VX_OP_FILTER` ([vx_op_filter.c](file:///home/avisheks/Documents/dibs/vex/src/vx_op_filter.c)), `VX_OP_PROJECT` ([vx_op_project.c](file:///home/avisheks/Documents/dibs/vex/src/vx_op_project.c)), `VX_OP_HASH_JOIN_PROBE` ([vx_op_hj_probe.c](file:///home/avisheks/Documents/dibs/vex/src/vx_op_hj_probe.c)).
  * **Sinks:** `ResultCollector` ([vx_sink_result.c](file:///home/avisheks/Documents/dibs/vex/src/vx_sink_result.c)), 64-way `HashAggSink` with Neumaier `f64` summation ([vx_sink_hash_agg.c](file:///home/avisheks/Documents/dibs/vex/src/vx_sink_hash_agg.c)), `HashJoinBuild` ([vx_sink_hj_build.c](file:///home/avisheks/Documents/dibs/vex/src/vx_sink_hj_build.c)), $O(N \log K)$ `TopKSink` ([vx_sink_topk.c](file:///home/avisheks/Documents/dibs/vex/src/vx_sink_topk.c)), and disk-spilling 64-way k-merge `ExternalSortSink` ([vx_sink_sort.c](file:///home/avisheks/Documents/dibs/vex/src/vx_sink_sort.c)).
  * **Scalar Built-ins & SIMD:** [vx_builtin.c](file:///home/avisheks/Documents/dibs/vex/src/vx_builtin.c) implements `len`, `lower`, `upper`, `startswith`, `contains`, `coalesce`, `abs`, `round`; [vx_simd_avx2.c](file:///home/avisheks/Documents/dibs/vex/src/vx_simd_avx2.c) and [vx_simd_neon.c](file:///home/avisheks/Documents/dibs/vex/src/vx_simd_neon.c) implement AVX2/NEON kernels with shared scalar helpers in [vx_simd_num_common.h](file:///home/avisheks/Documents/dibs/vex/src/vx_simd_num_common.h) and [vx_simd_str_common.h](file:///home/avisheks/Documents/dibs/vex/src/vx_simd_str_common.h).

### C. Out-of-Core Storage, Encodings & Compaction (`M13`–`M15`)
* **`.vx` Segments ([vx_seg_fmt.h](file:///home/avisheks/Documents/dibs/vex/src/vx_seg_fmt.h), [vx_seg.c](file:///home/avisheks/Documents/dibs/vex/src/vx_seg.c))**: Immutable `VXSEG02` binary files with 64B header, column/chunk descriptors with inline `vx_zone_t`, 64B-aligned data/null/StringView pages, embedded split-block Bloom filter + Robin Hood PK table, and full-file CRC32. Opened via zero-deserialization `mmap`.
* **Encodings ([vx_enc.h](file:///home/avisheks/Documents/dibs/vex/src/vx_enc.h), [vx_enc.c](file:///home/avisheks/Documents/dibs/vex/src/vx_enc.c))**: Frame-of-Reference + Bitpacking (`VX_ENC_FOR_BP`), String Dictionary (`VX_ENC_DICT`, with dictionary predicate pushdown in `vx_op_filter.c`), and RLE (`VX_ENC_RLE`).
* **Atomic `MANIFEST` & `.del` Sidecars ([vx_manifest_fmt.h](file:///home/avisheks/Documents/dibs/vex/src/vx_manifest_fmt.h), [vx_manifest.c](file:///home/avisheks/Documents/dibs/vex/src/vx_manifest.c))**: Atomic `MANIFEST.tmp -> fdatasync -> rename -> dir fsync` protocol, immutable `seg_<id>_g<gen>.del` sidecars, orphan sweeping, and `vx_recover_open()`.
* **Background Runtime ([vx_bg.h](file:///home/avisheks/Documents/dibs/vex/src/vx_bg.h), [vx_flusher.c](file:///home/avisheks/Documents/dibs/vex/src/vx_flusher.c), [vx_compact.c](file:///home/avisheks/Documents/dibs/vex/src/vx_compact.c))**: `hot_bytes` accounting, `max_memory_bytes` backpressure (`VX_ERR_MEMLIM`), background flush to `.vx`, leveled compaction with concurrent delete catch-up, and deferred `munmap`+`unlink` via `vx_owner_t`.

### D. Secondary Indexes, Cost Planner & ACID Transactions (`M16`–`M17`)
* **Secondary Indexes ([vx_art_fmt.h](file:///home/avisheks/Documents/dibs/vex/src/vx_art_fmt.h), [vx_art.c](file:///home/avisheks/Documents/dibs/vex/src/vx_art.c), [vx_idx.c](file:///home/avisheks/Documents/dibs/vex/src/vx_idx.c))**: Immutable `mmap`-able companion `.idx` files for both Robin Hood equality indexes and Adaptive Radix Tree (Node4/16/48/256 + prefix compression) range scans, using order-preserving key encoding.
* **Optimizer ([vx_plan.h](file:///home/avisheks/Documents/dibs/vex/src/vx_plan.h), [vx_plan.c](file:///home/avisheks/Documents/dibs/vex/src/vx_plan.c))**: Logical IR, constant folding, predicate/projection pushdown, empirical zone-map cost evaluation, access path selection (`PK_POINT`, `SEC_HASH`, `SEC_ART_RANGE`, `TABLE_SCAN`), and `?` explain rendering.
* **Transactions ([vx_tx.h](file:///home/avisheks/Documents/dibs/vex/src/vx_tx.h), [vx_tx.c](file:///home/avisheks/Documents/dibs/vex/src/vx_tx.c))**: RYOW staged sub-ops, `VX_WAL_OP_TX_BATCH` 32B WAL frames (`VXW2`) with enclosing CRC32, deadlock-free memory reservation, optimistic conflict detection (`VX_ERR_CONFLICT`), commit rebase across background flushes, and recovery replay.

### E. Ecosystem & Portability (`M18` Partial)
* **Arrow C Interface ([python/vex_arrow.h](file:///home/avisheks/Documents/dibs/vex/python/vex_arrow.h), [python/vex_arrow.c](file:///home/avisheks/Documents/dibs/vex/python/vex_arrow.c))**: Zero-dependency `ArrowSchema`, `ArrowArray`, and `ArrowArrayStream` (`"vu"` StringView, numeric, bit-packed `"b"` bool, and GIL-free `vx_owner_release` destructors). Currently not linked into `setup.py`.
* **Win32 Shim ([src/vx_platform_win32.h](file:///home/avisheks/Documents/dibs/vex/src/vx_platform_win32.h))**: Abstracts `_aligned_malloc`, 64-bit file stats/seeks, and atomic `MoveFileExW` replacement.

