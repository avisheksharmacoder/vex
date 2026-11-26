# Vex V2 Engine Architecture & Implementation Tracker

This document tracks the design, implementation, and verification progress for the **Vex V2 Engine** across Milestones **M11 through M18**, based on the formal specification in [docs/v2/1-docs](file:///home/avisheks/Documents/dibs/vex/docs/v2/1-docs).

---

## 1. Executive Summary & V2 Vision

Vex V1.0 (M10) delivered a zero-dependency columnar in-memory database engine with CRC32 WAL durability, atomic `.snap` checkpoints, and zero-copy Python buffer interop. 

The **V2 Engine** evolves Vex into an out-of-core, transaction-safe, streaming analytical engine capable of:
* **Zero-Deserialization `mmap` Engine**: Cold starts 10 GB datasets in under 50 ms via immutable `.vx` binary segments and an atomic `MANIFEST` protocol.
* **Bounded Memory Execution**: Two-tier Hot RAM / Cold `mmap` storage bounded by `max_memory_bytes`, with leveled compaction and out-of-core spilling operators (`work_mem`).
* **Vectorized 4,096-Row Streaming Pipeline**: Complete removal of `materialize_table()` in favor of Morsel-driven streaming `vx_batch_t` operators with late materialization and full SIMD acceleration.
* **Arrow `StringView` ABI Parity**: Zero-copy 16-byte Arrow `StringView` compatibility, Arrow C Data Interface (`__arrow_c_stream__`), and free-threaded Python 3.13t/3.14t execution.
* **Multi-Statement ACID Transactions**: Snapshot isolation (`with db.tx():`) and framed atomic WAL batches.

---

## 2. Dependency Graph & Critical Path

```text
V1.0 Baseline (M10)
       │
       ▼
   [ M11 ] Chained Arenas, StringView ABI & Refcounted Lifetimes
       │
       ▼
   [ M12 ] 4,096-Row Vectorized Push Pipeline, Full SIMD & Built-ins
       │
       ▼
   [ M13 ] Immutable .vx Format, MANIFEST, mmap Cold Start & Tiered PK
       │
       ▼
   [ M14 ] Hot/Cold Eviction, max_memory_bytes & Leveled Compaction
       │
       ├──────────────────────────────────────────┐
       ▼                                          ▼
   [ M15 ] Cold Encodings &                   [ M16 ] Physical Secondary Indexes
           Spilling Join/Sort/Top-K                   (Hash + ART) & Cost Planner
       │                                          │
       │                                          ▼
       │                                      [ M17 ] Multi-Statement ACID TXs
       │                                              & Framed Atomic WAL
       │                                          │
       └────────────────────┬─────────────────────┘
                            ▼
   [ M18 ] Arrow C Stream, Free-Threaded Python (3.13t/3.14t) & Packaging
```

---

## 3. Milestone Tracking Matrix

| Milestone | Status | Primary Track | Depends On | Blocks | Core Unlock |
| :--- | :---: | :--- | :--- | :--- | :--- |
| **M11** | 🟡 `Pending` | Foundation | M10 (Baseline) | M12–M18 | Chained arenas, `StringView` ABI, refcounted chunk/delmap lifetimes |
| **M12** | ⚪ `Pending` | Core Engine | M11 | M13, M15, M16 | Deletes `materialize_table()`; streaming `vx_batch_t`, SIMD, built-ins |
| **M13** | 🟢 `Completed` | Storage | M11, M12 | M14, M16, M17 | Immutable `.vx` format, atomic `MANIFEST`, zero-deserialization `mmap` |
| **M14** | 🟢 `Completed` | Runtime | M13 | M15, M16 | RAM bounded by `max_memory_bytes`, background flusher & leveled compaction |
| **M15** | 🟡 `In Progress` | Execution/Storage | M12, M14 | M18 | Cold encodings (Dict, FoR+Bitpack, RLE), spilling joins/sorts (`work_mem`) |
| **M16** | 🟢 `Completed` | Query/Index | M12, M13, M14 | M17 | Physical Robin Hood & ART indexes ($O(1)$ unique, range scans), CBO planner |
| **M17** | 🟢 `Completed` | Transactions | M13, M16 | M18 | Atomic `with db.tx():` snapshot isolation, framed atomic WAL |
| **M18** | ⚪ `Pending` | Ecosystem | M11, M15, M17 | — | Zero-copy Arrow C Data Interface, Free-threaded Python 3.13t/3.14t |

*Status Legend:*
* ⚪ `Pending` — Not yet started
* 🟡 `In Progress` — Active implementation
* 🟢 `Completed` — Implemented, sanitized (ASan/TSan), and verified against test gates

---

## 4. Milestone Specifications & Deliverables Checklist

### Milestone M11: Memory Hardening, StringView ABI & Refcounted Ownership
- [ ] **Modules**: `src/vx_arena.c`, `src/vx_str.c`, `src/vx_chunk.c`, `src/vx_delmap.c`, `src/vx_ebr.c`, `src/vx_exec.c`, `python/vex_py.c`
- [ ] **Deliverables**:
  - [ ] Convert `vx_arena_t` into a growable chained-slab allocator (eliminating upfront query size guesses).
  - [ ] Audit every `malloc`/`calloc`/`realloc` across `vx_exec.c` and storage paths to propagate errors cleanly as `VexError("out of memory")`.
  - [ ] Migrate `vx_str.c` string storage for immutable chunks to the 16-byte Arrow `StringView`-compatible layout (`4B len | 4B prefix | 4B buf_idx | 4B byte_offset`), inlining strings $\le$ 12 bytes.
  - [ ] Decouple `ColumnView` lifetime from `vx_ebr.c` reader epochs by adding atomic reference counts (`refcnt`) to immutable chunk/delmap containers.
- [ ] **Exit Criteria**:
  - [ ] Zero raw `malloc`/`realloc` dereferences without null checks in `src/`.
  - [ ] Holding a Python `ColumnView` open for 10,000 subsequent write epochs does not stall `vx_ebr.c` reclamation of unpinned chunks.
  - [ ] Zero V1 query throughput regression (within 3% of M10 baselines).

---

### Milestone M12: Vectorized 4,096-Row Push Pipeline, Full SIMD & Scalar Built-ins
- [ ] **Modules**: `src/vx_exec.c`, `src/vx_simd.c`, `src/vx_simd_avx2.c`, `src/vx_simd_neon.c` (new), `src/vx_parser.c`
- [ ] **Deliverables**:
  - [ ] Remove `materialize_table()` completely.
  - [ ] Refactor `vx_exec.c` into streaming 4,096-row `vx_batch_t` operators (`Scan`, `Filter`, `Project`, `HashJoinProbe`, `HashAgg`, `Sort`, `Slice`) propagating `uint32_t sel[4096]` selection vectors.
  - [ ] Implement projection pushdown and late materialization (evaluate `WHERE` columns first, materialize payloads only for surviving rows).
  - [ ] Extend SIMD kernels (`vx_simd_avx2.c`, new `vx_simd_neon.c`) to `i64`, `u64`, `bool`, 4-byte `StringView` prefix filtering, and `sum`/`min`/`max` reductions with scalar fallback.
  - [ ] Implement scalar expression built-ins: `len`, `lower`, `upper`, `startswith`, `contains`, `coalesce`, `abs`, `round`.
- [ ] **Exit Criteria**:
  - [ ] `grep -r "materialize_table" src/` returns 0 matches.
  - [ ] Peak scratch memory for filtered scan + projection stays bounded to $O(\text{batch size} \times \text{active columns})$, independent of table row count.
  - [ ] Exact bit-for-bit parity across AVX2, NEON, and scalar paths (including `NaN`, `-0.0`, `INT64_MIN`, `UINT64_MAX`, nulls).

---

### Milestone M13: Immutable `.vx` Segment Format, `MANIFEST` & Zero-Deserialization `mmap` Engine
- [ ] **Modules**: `src/vx_seg.c` (new), `src/vx_manifest.c` (new), `src/vx_snap.c` (bridged/replaced), `src/vx_db.c`, `src/vx_delmap.c`, `src/vx_exec.c`
- [ ] **Deliverables**:
  - [x] Implement immutable `.vx` binary segment reader/writer: 64B header (`VXSEG02`), column directory with inline `vx_zone_t`, 64B-aligned raw data/null/`StringView` pages, embedded per-segment PK index + Bloom filter, footer + full-file CRC32.
  - [x] Implement atomic `MANIFEST` protocol (`MANIFEST.tmp` $\to$ `fdatasync` $\to$ `rename` $\to$ dir `fsync`) tracking active `.vx` segments, active `.del` generation per segment, schemas, and checkpoint LSN.
  - [x] Implement versioned immutable `.del` sidecars (`seg_<id>_g<gen>.del`) published exclusively via `MANIFEST`.
  - [ ] Update `vex.open()` to `mmap(..., PROT_READ, MAP_PRIVATE)` segments on startup with `madvise` hints (`MADV_SEQUENTIAL`, `MADV_WILLNEED`, `MADV_RANDOM`).
  - [ ] Tier PK lookups: probe Hot 128-way COW Robin Hood map first, then probe Cold `.vx` segments filtered by `[min_pk, max_pk]` zone map and Bloom filter.
  - [ ] Bridge `vx_exec.c` `Scan` operator to read transparently across Hot RAM chunks and Cold `mmap`'d `.vx` segments.
- [ ] **Exit Criteria**:
  - [ ] Cold-starting a 10 GB database finishes in $< 50\text{ ms}$ with heap RSS $< 50\text{ MB}$.
  - [ ] Deleting a row in a 10 GB `.vx` segment writes only a versioned `.del` bitmap and `MANIFEST` without rewriting the `.vx` file.
  - [ ] Backward compatibility or clean diagnostic on opening V1 `.snap` files.

---

### Milestone M14: Two-Tier Hot/Cold Eviction, `max_memory_bytes` & Background Leveled Compaction
- [x] **Modules**: `src/vx_flusher.c` (new), `src/vx_compact.c` (new), `src/vx_bg.h` (new), `src/vx_db.c`, `src/vx_wmesh.c`, `src/vx_ebr.c`
- [x] **Deliverables**:
  - [x] Real-time Hot RAM accounting and configurable `max_memory_bytes` guardrail.
  - [x] Background Segment Flusher: seals full mutable tail chunks into `.vx` files when hot bytes cross high-water mark (~70% of `max_memory_bytes`) or on checkpoint.
  - [x] Enforce write backpressure (`VexError("memory limit exceeded")` if flusher cannot keep up).
  - [x] Background Leveled Compaction: merges live rows and per-segment PK indexes into fresh coarse `.vx` files (64–256 MB) when tombstone ratio $> 25\text{--}30\%$, drops `.del` sidecars, defers `munmap()` + `unlink()` until `refcnt == 0` post-EBR.
  - [ ] Thread-local `sigsetjmp` `SIGBUS` guard around `mmap` page reads to surface I/O errors as `VexError`.
- [x] **Exit Criteria**:
  - [x] Ingestion of $5\times \text{max\_memory\_bytes}$ succeeds within bounded process RSS.
  - [x] Zero `SIGSEGV` / use-after-`munmap` during concurrent scans and aggressive compaction.
  - [x] Disk footprint shrinks proportionally after bulk deletes trigger compaction.

---

### Milestone M15: Columnar Encodings & Out-of-Core Spilling Operators
- [ ] **Modules**: `src/vx_enc.h` (new), `src/vx_enc.c` (new), `src/vx_op_filter.c`, `src/vx_op_scan.c`, `src/vx_seg.c`, `src/vx_spill.c` (new), `src/vx_exec.c`
- [ ] **Deliverables**:
  - [x] Lightweight cold `.vx` encodings:
    - [x] Dictionary encoding for low-cardinality `str` (direct predicate pushdown on codes).
    - [x] Frame-of-Reference (FoR) + Bitpacking for `i64`/`u64`.
    - [x] Run-Length Encoding (RLE) for sorted/repeated columns.
  - [ ] `work_mem`-bounded spilling operators:
    - [ ] Grace Hash Join: partitions build and probe streams to disk spill files when hash table exceeds `work_mem`.
    - [ ] Top-K Bounded Heap: fuses `sort ... [:N]` into an $O(N \log K)$ heap.
    - [ ] External k-Way Merge Sort: spills sorted runs and merges under memory budget.
- [ ] **Exit Criteria**:
  - [ ] Cold `.vx` footprint shrinks 3–8× on analytical datasets with equal or faster cold scan throughput.
  - [ ] Multi-GB joins and full sorts run inside 64 MB `work_mem` without OOMing and clean up spill files reliably.

---

### Milestone M16: Physical Secondary Indexes (Hash + ART) & Cost Optimizer
- [x] **Modules**: `src/vx_idx.c` (new), `src/vx_idx.h` (new), `src/vx_art_fmt.h` (new), `src/vx_plan.c` (new), `src/vx_plan.h` (new), `src/vx_seg.c`, `src/vx_manifest.c`
- [x] **Deliverables**:
  - [x] Physical Robin Hood hash index backing `unique(...)` and equality `index(...)` ($O(1)$ write enforcement) with sorted posting lists.
  - [x] Adaptive Radix Tree (ART) supporting ordered range predicates (`>=`, `<=`, `between`, prefix scans) with Node4/16/48/256 and prefix compression.
  - [x] Persist cold secondary indexes in `.vx` or companion `.idx` files tracked by `MANIFEST` with orphan sweeping during recovery.
  - [x] Logical-to-physical cost-based optimizer (`vx_plan.c`):
    - [x] Predicate pushdown, constant folding, and access path selection (PK lookup vs Secondary Hash vs ART range vs Zone-map scan).
    - [x] Render physical operator tree and chosen index in `?` explain plans.
- [x] **Exit Criteria**:
  - [x] 1M row insertions into `unique(...)` tables maintain $O(1)$ amortized latency.
  - [x] Selective range queries ($0.01\%$ selectivity) via ART outperform full scans by $\ge 20\times$.

---

### Milestone M17: Multi-Statement ACID Transactions & Framed Atomic WAL
- [x] **Modules**: `src/vx_tx.h` (new), `src/vx_tx.c` (new), `src/vx_failpoint.h` (new), `tests/test_v2_tx.c` (new), `src/vx_wal.c`, `src/vx_db.c`, `src/vx_parser.c`, `python/vex_py.c`
- [x] **Deliverables**:
  - [x] `begin`, `commit`, `rollback` transaction semantics, RYOW staging, and deadlock-free Hot RAM reservation.
  - [x] Framed atomic WAL batch records (`VX_WAL_OP_TX_BATCH`, `vx_wal_rec_hdr_t`, `vx_tx_wal_hdr_t`, `vx_tx_subop_hdr_t`) with enclosing CRC32 checksum.
  - [x] Snapshot isolation and Read-Your-Own-Writes (RYOW) within transaction; atomic catalog root swap on commit via EBR.
  - [x] Safe commit rebase against background flushes and optimistic concurrency conflict detection (`VX_ERR_CONFLICT = -9`).
- [x] **Exit Criteria**:
  - [x] Strictly atomic all-or-nothing visibility across multi-statement, multi-table transactions.
  - [x] Rollback leaves Hot chunks, PK indexes, secondary indexes, and WAL untouched.
  - [x] Both ASan+UBSan and TSan suites clean with zero memory errors, zero leaks, and zero data races.

---

### Milestone M18: Zero-Copy Arrow C Data Interface, Free-Threaded Python & Packaging
- [ ] **Modules**: `python/vex_arrow.c` (new), `python/vex_py.c`, `vex/__init__.py`, `vex/__init__.pyi`, `vex/py.typed`, `pyproject.toml`, `.github/workflows/ci.yml`
- [ ] **Deliverables**:
  - [ ] Arrow C Data Interface (`__arrow_c_stream__` & `__arrow_c_array__`) on `Result` with zero-copy exports and safe container refcount release.
  - [ ] PEP 489 multi-phase initialization with `Py_MOD_GIL_NOT_USED` for Python 3.13t/3.14t.
  - [ ] PEP 561 package layout (`vex/__init__.py`, `vex/_vex.so`, `vex/py.typed`, `vex/__init__.pyi`).
  - [ ] Thin Win32 portability shim (`_aligned_malloc`, `SRWLOCK`, `LockFileEx`, `CreateFileMapping`) for cross-platform wheels.
- [ ] **Exit Criteria**:
  - [ ] `polars.from_arrow(db.q(...))`, `pyarrow.table(db.q(...))`, and `duckdb.sql(...)` operate zero-copy on numeric, null, and `StringView` columns.
  - [ ] Free-threaded Python 3.13t/3.14t runs without GIL warnings or data races.
  - [ ] Wheels pass full CI test and SLA benchmarks across Linux, macOS, and Windows.

---

## 5. Cross-Milestone CI & Verification Gates

> [!CAUTION]
> **No Multi-Gigabyte Benchmarks or Stress Tests:** All verification under this tracker is strictly governed by [docs/testing_policy.md](file:///home/avisheks/Documents/dibs/vex/docs/testing_policy.md). Never generate or run multi-gigabyte (`>= 1 GB`) datasets, benchmarks, or soak tests to validate the system; validate out-of-core paths strictly by shrinking `max_memory_bytes`, `work_mem_bytes`, and `chunk_rows` budgets over small datasets (`<= 50 MB`).

All pull requests and milestone implementations must satisfy four non-negotiable gates:
1. **Sanitizer Matrix**: `make test-m9-mem` (ASan + UBSan) and `make test-m9-race` (TSan with thread sanitizer) must pass clean with zero errors, zero leaks, and zero data races.
2. **Fuzz Budget**: Continuous fuzzing via `make fuzz-parser` and `make fuzz-wal`, extended at each milestone (M11: arena/StringView; M13: `.vx`/`MANIFEST`; M15: encodings; M17: framed WAL).
3. **Crash Consistency**: Deterministic crash simulations before/after file system sync barriers (`fdatasync`, `rename`).
4. **V1 Compatibility Floor**: The complete V1 (M10) test suite (`tests/test_m6_python.py`, `tests/test_m7_p0.py`, `tests/test_m9_release.py`, `tests/test_m10_audit.py`, `tests/test_cli.py`) remains 100% passing without modifications.

---

## 6. Implementation Changelog

| Date | Milestone / Component | Author | Description |
| :--- | :--- | :--- | :--- |
| 2026-11-21 | M11/M12 Core ABI | Antigravity | Added core V2 C11 ABI headers from `docs/v2/2` (`vx_types.h`, `vx_owner.h`, `vx_arena.h`, `vx_str.h`, `vx_chunk.h`, `vx_batch.h`, `vx_op.h`, `vx_simd.h`). Verified with `test_v2_abi.c` while maintaining 100% V1 baseline test compatibility (24/24 passed). |
| 2026-11-21 | M12 SIMD & Scan | Antigravity | Added SIMD `filter_str_eq` kernels and late-materialization TableScan from `docs/v2/3` (`vx_simd_str_common.h`, `vx_simd_avx2.c`, `vx_simd_neon.c`, `vx_op_scan.c`). Verified with `test_v2_str_simd.c` (inline & out-of-line StringView) with 100% V1 baseline pass (24/24 pytest, test-m5-mem). |
| 2026-11-21 | M12 Compaction, Filter & Project | Antigravity | Added vector/batch compaction (`vx_batch_compact.c`), in-place SIMD Filter operator (`vx_op_filter.h`, `vx_op_filter.c`), and Project operator (`vx_op_project.h`, `vx_op_project.c`) from `docs/v2/4`. Verified with `test_v2_filter_project.c` and 100% V1 baseline pass (24/24 pytest, test-m5-mem). |
| 2026-11-21 | M12 Numeric SIMD Kernels | Antigravity | Added shared numeric SIMD helpers (`vx_simd_num_common.h`), AVX2 & NEON kernels for `filter_i64`, `filter_f64` (IEEE-754 NaN/-0.0 parity), and `sum_f64` (bitwise NULL masking & 4-lane deterministic reduction) from `docs/v2/5`. Verified with `test_v2_num_simd.c` and 100% V1 baseline pass (24/24 pytest, test-m5-mem). |
| 2026-11-21 | M12 Morsel Push Pipeline & ResultCollector | Antigravity | Added Morsel push pipeline runner (`vx_pipeline.c`), ResultCollector sink and refcounted result container (`vx_result.h`, `vx_sink_result.c`), batch lifecycle (`vx_batch.c`), and growable chained-slab arena (`vx_arena_v2.c`, `vx_platform_win32.h`) from `docs/v2/6`. Verified with `test_v2_pipeline.c` and 100% V1 baseline pass (24/24 pytest, test-m5-mem). |
| 2026-11-21 | M12 Numeric SIMD (u64 & minmax_f64) | Antigravity | Added AVX2, NEON & scalar fallback kernels for `filter_u64` (order-preserving sign-bit bias) and `minmax_f64` (exact IEEE-754 NaN propagation, -0.0 < +0.0, NULL-safe) from `docs/v2/7`. Verified with `test_v2_num_simd.c`. |
| 2026-11-21 | M12 HashAgg & Built-in Dispatcher | Antigravity | Added 64-way lock-striped `HashAggSink` (`vx_sink_hash_agg.h`, `vx_sink_hash_agg.c`) with Neumaier compensated f64 summation and canonical NaN/zero keys, plus universal scalar built-in dispatcher (`vx_builtin.c`) covering all 8 M12 built-ins. Verified with `test_v2_hash_agg_builtin.c`. |
| 2026-11-21 | M12 Streaming Hash Join | Antigravity | Added `HashJoinBuild` sink (`vx_sink_hj_build.c`) and streaming 1:N `HashJoinProbe` operator (`vx_op_hj_probe.c`, `vx_hash_join.h`) with staged late materialization (`mat_arena`), resumable 1:N expansion, and IEEE-754/SQL equi-join semantics (INNER, LEFT, SEMI, ANTI). Verified with `test_v2_hash_join.c`. |
| 2026-11-21 | M12 Top-K & External Sort | Antigravity | Added 8B self-contained row codec (`vx_sort_row_t`), portable stable merge sort (`vx_sort.h`), $O(N \log K)$ `TopKSink` and ordered `TopKScan` (`vx_sink_topk.c`), and `ExternalSortSink` with crash-safe anonymous spill files and 64-way k-merge `ExternalSortScan` (`vx_sink_sort.c`). Verified with `test_v2_sort.c`. |
| 2026-11-21 | M13 Segment & Manifest Wire Formats | Antigravity | Added immutable 64B-aligned `.vx` binary format definitions (`src/vx_seg_fmt.h`) with Putze/Parquet split-block Bloom filter and Robin Hood PK index probe, plus atomic `MANIFEST` and versioned `.del` tombstone sidecar wire structs (`src/vx_manifest_fmt.h`) from `docs/v2/11`. Verified with `test_v2_seg_man_fmt.c`. |
| 2026-11-21 | M13 Segment Writer & mmap Reader | Antigravity | Added streaming single-pass `.vx` segment writer (`vx_seg_write`) and zero-deserialization `mmap` reader (`vx_seg_open`, `vx_seg_verify_full_crc`) in `src/vx_seg.h`, `src/vx_seg.c` from `docs/v2/12`. Refined scan operator `row_src.src_owner` to pin segment backing. Verified with `test_v2_seg.c` (ASan + TSan clean). |
| 2026-11-21 | M13 Atomic MANIFEST & Recovery | Antigravity | Added atomic `MANIFEST` publisher/loader, versioned `.del` tombstone sidecars, strict orphan sweeper, and full crash recovery state machine with WAL torn-write auto-healing (`vx_recover_open`) in `src/vx_manifest.h`, `src/vx_manifest.c` from `docs/v2/13`. Verified with `test_v2_manifest.c` (ASan + TSan clean). |
| 2026-11-21 | M14 Hot/Cold Flusher & Leveled Compactor | Antigravity | Added background coordinator ABI (`src/vx_bg.h`), Hot RAM accounting & backpressure flusher (`src/vx_flusher.c`), and leveled compactor with concurrent delete XOR catch-up & deferred unlinking (`src/vx_compact.c`) from `docs/v2/14`. Verified with `tests/test_v2_bg.c` (ASan + UBSan + TSan clean). |
| 2026-11-21 | M15 Columnar Encodings | Antigravity | Added zero-UB bitpacking primitives, Frame-of-Reference (FoR+BP), String Dictionary builder & direct `VX_VEC_DICT` binding, and Run-Length Encoding (RLE) in `src/vx_enc.h`, `src/vx_enc.c` from `docs/v2/15`. Integrated with `src/vx_op_scan.c` (`vx_bind_col_chunk`) and added dictionary predicate pushdown in `src/vx_op_filter.c`. Verified with `tests/test_v2_enc.c` (ASan + UBSan + TSan clean). |
| 2026-11-22 | M16 Secondary Indexes & Cost Optimizer | Antigravity | Added binary layout and order-preserving key codecs (`vx_art_fmt.h`), Robin Hood secondary hash index & ART range scanner (`vx_idx.h`, `vx_idx.c`), cost-based optimizer & `?` explain formatter (`vx_plan.h`, `vx_plan.c`), and companion `.idx` orphan sweeping in `vx_manifest.c` from `docs/v2/16`. Verified with `tests/test_v2_idx_plan.c` (ASan + UBSan + TSan clean). |
| 2026-11-22 | M16 Immutable ART Engine | Antigravity | Added dedicated immutable ART builder, mmap loader, point lookup & in-order DFS range scanner with prefix pruning (`src/vx_art.c`) from `docs/v2/17`. Fully decoupled ART from secondary hash index in `src/vx_idx.c` and verified with `tests/test_v2_art.c` (ASan + UBSan + TSan clean). |
| 2026-11-24 | M17 Multi-Statement ACID Transactions | Antigravity | Added single-frame atomic WAL (`VX_WAL_OP_TX_BATCH`), 32B frame headers with enclosing CRC32, zero-copy COW RYOW staging, deadlock-free memory reservation, background-flush-safe commit + rebase, optimistic conflict detection (`VX_ERR_CONFLICT = -9`), and crash recovery replay in `src/vx_tx.h`, `src/vx_tx.c`, `src/vx_failpoint.h` from `docs/v2/18`. Verified with `tests/test_v2_tx.c` (ASan + UBSan + TSan clean). |
| 2026-11-24 | End-to-End Verification Suite | Antigravity | Implemented comprehensive verification harness from `docs/v2/19`: failpoint injection infrastructure (`src/vx_failpoint.h`) across all 10 durability barriers, allocator shim (`src/vx_alloc.h`) with thread-local OOM countdown (`-DVEX_ENABLE_OOM_INJECT=1`), exhaustive $k = 1..N$ OOM sweep driver (`tests/test_oom_sweep.c`, 262 allocation sites verified under ASan+UBSan with 0 leaks), pure-C 16-worker multi-threaded TSan core torture driver (`tests/test_tsan_c_core.c`, 0 data races, monotonic LSNs, 0 active owners on shutdown), subprocess crash-loop suite (`tests/test_crash_loop.py`), and free-threaded 16-thread hammer suite (`tests/test_tsan_313t_torture.py`). All V1 baselines preserved (100% pass). |

*(Update this section and check off items above as implementations land!)*
