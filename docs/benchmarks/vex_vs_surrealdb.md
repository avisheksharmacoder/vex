# Vex vs SurrealDB (Embedded) — Small-Scale Benchmark & Engineering Nuances

This document records the reproducible small-scale head-to-head benchmark between **Vex** (active V1 C11 columnar engine) and **SurrealDB Embedded** (`surrealdb` `3.0.0` / `surrealdb-embedded` `3.0.0`, reporting `surrealdb-4.0.0`), alongside the architectural nuances developers should understand when interpreting these numbers.

> [!NOTE]
> All benchmark scales strictly obey [docs/testing_policy.md](file:///home/avisheks/Documents/dibs/vex/docs/testing_policy.md): datasets remain well under the `50 MB` and `<= 100,000` row ceiling.

---

## 1. Environment & Methodology

* **Benchmark Script:** [bench/bench_vs_surrealdb.py](file:///home/avisheks/Documents/dibs/vex/bench/bench_vs_surrealdb.py)
* **Command Executed:**
  ```bash
  PYTHONPATH=python python3 bench/bench_vs_surrealdb.py --mode both --rows 10000 --pk-lookups 500 --pk-updates 200 --iters 5
  ```
* **Dataset:** `10,000` orders (`id`, `user_id`, `bucket`, `amount`, `paid`) + `2,000` users (`id`, `country`, `tier`) = `12,000` total rows.
* **Host & Runtime:** `x86_64` (8 logical CPUs, Intel Core i5-10210U @ 1.60 GHz), Python `3.14.7`, Vex SIMD backend `avx2`, `workers=4`, `chunk_rows=4096`, `iters=5` (median ms after 1 warmup pass).
* **Correctness Verification:** Every workload asserts exact row counts, aggregate totals/buckets, top-10 sorted elements, per-country join revenue, and updated primary-key values against deterministic ground truth across both engines.

---

## 2. Benchmark Results

### 2.1 In-Memory (`Vex RAM` vs `SurrealDB mem://`)

| Benchmark | Vex (ms) | SurrealDB (ms) | Vex Metric | SurrealDB Metric | Vex Speedup |
| :--- | ---: | ---: | ---: | ---: | ---: |
| 1. Bulk Insert (12,000 rows) | 12.94 | 745.10 | 927.4k rows/s | 16.1k rows/s | **57.58x** |
| 2. PK Point Lookups (500 ops) | 2.49 | 137.38 | 201.1k ops/s | 3.6k ops/s | **55.25x** |
| 3. Filter Scan (`amount > 900.0`) | 1.04 | 18.67 | 9.58 M rows/s | 535.6k rows/s | **17.88x** |
| 4. Compound Range Filter (3 preds) | 1.09 | 8.68 | 9.21 M rows/s | 1.15 M rows/s | **7.99x** |
| 5. Multi-Agg (`count, sum, min, max`) | 0.90 | 11.74 | 11.17 M rows/s | 851.9k rows/s | **13.11x** |
| 6. Filtered Group-By (8 buckets) | 0.10 | 12.20 | 101.28 M rows/s | 819.8k rows/s | **123.54x** |
| 7. High-Card Group-By (2,000 groups) | 0.84 | 39.86 | 11.88 M rows/s | 250.9k rows/s | **47.33x** |
| 8. Top-K Filter + Sort (`[:10]`) | 4.12 | 14.53 | 2.43 M rows/s | 688.2k rows/s | **3.53x** |
| 9. Join / Link + Agg (`10,000 x 2,000`) | 2.43 | 84.64 | 4.11 M rows/s | 118.2k rows/s | **34.82x** |
| 10. Query -> NumPy + sum (`10,000 f64`) | 0.57 | 18.59 | 17.43 M rows/s | 537.8k rows/s | **32.41x** |
| 11. PK Point Updates (200 ops) | 5.43 | 72.92 | 36.9k ops/s | 2.7k ops/s | **13.44x** |

### 2.2 On-Disk (`Vex WAL fsync=True` vs `SurrealDB surrealkv://`)

| Benchmark | Vex (ms) | SurrealDB (ms) | Vex Metric | SurrealDB Metric | Vex Speedup |
| :--- | ---: | ---: | ---: | ---: | ---: |
| 1. Bulk Insert (12,000 rows) | 895.12 | 1192.81 | 13.4k rows/s | 10.1k rows/s | **1.33x** |
| 2. PK Point Lookups (500 ops) | 2.78 | 141.85 | 179.5k ops/s | 3.5k ops/s | **50.94x** |
| 3. Filter Scan (`amount > 900.0`) | 0.93 | 17.49 | 10.72 M rows/s | 571.8k rows/s | **18.75x** |
| 4. Compound Range Filter (3 preds) | 1.29 | 6.80 | 7.73 M rows/s | 1.47 M rows/s | **5.26x** |
| 5. Multi-Agg (`count, sum, min, max`) | 0.84 | 9.94 | 11.85 M rows/s | 1.01 M rows/s | **11.78x** |
| 6. Filtered Group-By (8 buckets) | 0.06 | 9.80 | 158.67 M rows/s | 1.02 M rows/s | **155.43x** |
| 7. High-Card Group-By (2,000 groups) | 0.91 | 33.04 | 10.98 M rows/s | 302.7k rows/s | **36.29x** |
| 8. Top-K Filter + Sort (`[:10]`) | 3.65 | 14.29 | 2.74 M rows/s | 699.6k rows/s | **3.92x** |
| 9. Join / Link + Agg (`10,000 x 2,000`) | 2.44 | 88.14 | 4.11 M rows/s | 113.5k rows/s | **36.19x** |
| 10. Query -> NumPy + sum (`10,000 f64`) | 0.57 | 15.65 | 17.66 M rows/s | 638.9k rows/s | **27.65x** |
| 11. PK Point Updates (200 ops) | 10575.78 | 61.33 | 19 ops/s | 3.3k ops/s | **0.01x** |

---

## 3. Engineering Nuances for Developers

### 3.1 Disk `fsync=True` vs Vex's `fsync=False` Default (Why #11 Took 10.58 s)

* **Vex's default is `fsync=False`:** Both `vx_db_default_cfg()` ([src/vx_db.c](file:///home/avisheks/Documents/dibs/vex/src/vx_db.c#L1077-L1086)) and `vex.open()` ([python/vex_py.c](file:///home/avisheks/Documents/dibs/vex/python/vex_py.c#L1590-L1633)) default to OS page-cache WAL durability (`fsync=False`).
* **Benchmark #11 hit 200 synchronous `fdatasync()` barriers:** [open_vex()](file:///home/avisheks/Documents/dibs/vex/bench/bench_vs_surrealdb.py#L168-L176) explicitly set `fsync=True` in `--mode disk`. Each `vdb.q("orders[$k] = { amount: $v }")` statement runs synchronously through `vx_db_wal_log()` $\to$ `vx_wal_append_batch(..., fsync_now=True)` ([src/vx_wal.c](file:///home/avisheks/Documents/dibs/vex/src/vx_wal.c#L407-L409)), issuing one hardware `fdatasync(fd)` per single-row update (~52.9 ms average per flush on the test host, totaling `10,575.78 ms` for 200 updates).
* **Micro-batching vs single-statement WAL writes:** In Benchmark #1 (`895.12 ms` for 12,000 rows), `db.put(..., sync=False)` + `db.flush()` pushed rows through the per-thread SPSC write mesh ([src/vx_wmesh.c](file:///home/avisheks/Documents/dibs/vex/src/vx_wmesh.c)), coalescing up to `1,024` operations per commit batch so `12,000` inserts only paid ~12 `fdatasync()` flushes.
* **SurrealKV default:** Embedded `surrealkv://` completed 200 sequential `UPDATE` calls in `61.33 ms` (~0.31 ms/op), which is faster than physical `fdatasync()` round-trips on consumer NVMe storage—meaning embedded `surrealkv://` is not blocking every single statement on a physical disk sync barrier by default.

> [!TIP]
> When running durable write workloads in Vex with `fsync=True`, batch mutations via `db.put(..., upsert=True, sync=False)` followed by `db.flush()`, or combine mutations into multi-statement `db.q(...)` batches rather than issuing individual single-row `UPDATE` queries in a loop.

---

### 3.2 V1 Wired Execution Paths: Fast Path vs `materialize_table()`

As documented in [aidocs/architecture.md](file:///home/avisheks/Documents/dibs/vex/aidocs/architecture.md), `vex._vex` currently compiles the **V1 engine** (`include/` + `src/vx_db.c` + `src/vx_exec.c`), while V2 streaming operators (`src/vx_pipeline.c`, `src/vx_op_*.c`, `src/vx_sink_*.c`, `src/vx_seg.c`, `src/vx_art.c`, `src/vx_tx.c`) are standalone tested components not yet wired into Python.

Inside [vx_exec_program()](file:///home/avisheks/Documents/dibs/vex/src/vx_exec.c#L1994-L2020), queries follow one of three distinct paths:

1. **O(1) PK Fast Path (`try_pk_fast_path`, Benchmark #2):**
   Single-stage `table[$k]` lookups probe the 128-way sharded Robin Hood primary-key hash index ([src/vx_rhmap.c](file:///home/avisheks/Documents/dibs/vex/src/vx_rhmap.c)) directly and materialize only the single matching row (`2.49 ms` for 500 lookups).
2. **Morsel SIMD Fast Path (`try_morsel_fast_path`, Benchmark #6):**
   Only activates when stage 0 is a single binary predicate on `f64`/`i64` (`col op lit`) and stage 1 is a group stage with 0 or 1 `i64` group key and `[count(), sum(col)]`. It runs across all `workers` threads with per-chunk `min`/`max` zone-map pruning and AVX2 filtering ([src/vx_simd_avx2.c](file:///home/avisheks/Documents/dibs/vex/src/vx_simd_avx2.c#L605-L655)), hitting **101–158 M rows/s** (`0.06–0.10 ms`).
3. **General Materialization Fallback (`materialize_table()`, Benchmarks #3, #4, #5, #7, #8, #9, #10):**
   Any query outside those two shapes—including plain filter+projection (`orders[amount > 900.0] id, amount`), compound `and` filters, global `min`/`max`, high-cardinality `by user_id` (`2,000` groups), Top-K sorts, and hash joins—currently calls `materialize_table()` ([src/vx_exec.c](file:///home/avisheks/Documents/dibs/vex/src/vx_exec.c#L745-L790)), copying all live rows into a temporary `vx_wtable_t` before evaluating pipeline stages.
   * Even with this V1 full-table copy, Vex stays **3.5x–47.3x** faster than SurrealDB embedded on `10,000` rows because operations stay inside contiguous C columnar arrays without document/CBOR decoding.
   * Notice in Benchmark #8 (`Top-K Filter + Sort`, `4.12 ms` vs `14.53 ms`), V1 runs a full lock-free merge sort over filtered rows before slicing `[:10]`; wiring V2's $O(N \log K)$ `TopKSink` ([src/vx_sink_topk.c](file:///home/avisheks/Documents/dibs/vex/src/vx_sink_topk.c)) and streaming `vx_batch_t` pipeline eliminates both `materialize_table()` and full sorts.

---

### 3.3 Fair Idiomatic Baseline Choices for SurrealDB

To avoid artificially penalizing SurrealDB embedded (`surrealdb-3.0.0`), the benchmark uses SurrealDB's fastest verified patterns rather than naive defaults:

* **Bulk Insert (`INSERT INTO ... $rows RETURN NONE`):** Pre-benchmark probing showed `INSERT INTO ... $rows RETURN NONE` ran faster (`70.32 ms` per 1,000 rows) than `db.insert(...)` (`85.01 ms`), because `RETURN NONE` skips serializing all inserted document objects back across the Rust/CBOR/Python boundary. Records are batched (`--surreal-batch-size 2500`) using pre-allocated `RecordID` objects.
* **PK Point Lookups (`SELECT VALUE amount FROM ONLY $id`):** Measured `47.24 ms` per 200 lookups vs `61.40 ms` for `db.select(rid).execute()`, returning only the scalar `float` field to match Vex's `orders[$k] amount`.
* **PK Point Updates (`UPDATE $id SET amount = $v RETURN NONE`):** Avoids returning updated records on mutation.
* **Relational Joins vs Native Record Links (#9):** `orders.user_id` is stored as a typed `record<users>` (`RecordID("users", uid)`), letting SurrealDB traverse `user_id.country` via direct record links (`SELECT user_id.country AS country, ... GROUP BY country`) instead of SQL subqueries. Vex executes an explicit foreign-key hash join (`orders[paid] & users`) followed by hash aggregation, finishing **34.8x–36.2x** faster (`2.43–2.44 ms` vs `84.64–88.14 ms`) due to contiguous columnar probing vs per-document field lookup.

---

### 3.4 Zero-Copy NumPy vs Document Materialization (#10)

* **Vex:** `vdb.q("orders amount")["amount"].np()` wraps the 64-byte-aligned C `double*` result buffer directly as a read-only NumPy `ndarray` via the PEP 3118 buffer protocol (`0.57 ms` including query execution + `arr.sum()`).
* **SurrealDB:** `SELECT VALUE amount FROM orders` decodes `10,000` values across the Rust/CBOR boundary into Python `list[float]` objects, and `np.asarray(..., dtype=np.float64)` copies them into a fresh array (`15.65–18.59 ms`).

---

### 3.5 Python VQL `|=` Upsert Note

In V1 Python bindings ([python/vex_py.c](file:///home/avisheks/Documents/dibs/vex/python/vex_py.c#L167-L326)), `vx_normalize_query()` detects `|` to rewrite V2 pipe syntax and currently misparses V1's `|=` upsert operator (causing 1 tracked failure in [tests/test_m7_p0.py](file:///home/avisheks/Documents/dibs/vex/tests/test_m7_p0.py#L52-L79)). `db.put(table, row_dict, upsert=True)` bypasses the VQL parser and `vx_normalize_query()` completely—and note that `db.put()` defaults to `upsert=True` in Python.

