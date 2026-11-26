# Vex

**Vex** is a zero-dependency C11 columnar in-memory database engine with CRC32 WAL persistence, atomic checkpoint snapshots, a 6-keyword Pythonic query language, and first-class GIL-free Python bindings.

Unlike traditional in-memory databases that treat Python as a foreign scripting client—incurring serialization bottlenecks, coarse locking, or IPC latency—Vex is engineered as a true Python-native storage engine. It integrates directly with the CPython C-API to deliver zero-copy NumPy and `memoryview` exports via the PEP 3118 Buffer Protocol (sharing 64-byte-aligned columnar C memory with zero object allocations and safe lifetime pinning), systematically releases the GIL across analytical queries and background checkpointing to unlock multi-core parallelism for asyncio and multi-threaded servers, and employs lock-free Epoch-Based Reclamation (EBR) MVCC so high-throughput dictionary ingestion (`db.put`) never blocks analytical readers.

## Features

* **Zero-Copy NumPy Interop**: Exposes 64-byte-aligned C memory directly via PEP 3118 (`col.np()`) with zero data copying and safe lifetime pinning.
* **GIL-Free Execution**: Releases the Python GIL across all analytical scans, joins, aggregations, and background flushes for multi-core scaling.
* **Lock-Free EBR MVCC**: Epoch-Based Reclamation ensures analytical readers run lock-free without blocking or waiting on incoming writes.
* **Vectorized SIMD Kernels**: Runtime-dispatched AVX2, NEON, and portable scalar kernels drive multi-gigabyte-per-second column scans.
* **Micro-Batched Ingestion**: Per-thread SPSC write mesh queues ingest millions of rows per second via fast-path dictionaries (`db.put`).
* **Lightweight German Strings**: 16-byte Umbra-style strings inline payloads up to 12 bytes directly on the chunk, eliminating heap allocations.
* **Zone Map Pruning**: Per-chunk min/max statistics skip unneeded 4,096-row columnar blocks in $O(1)$ time to maximize memory bandwidth.
* **Pipelined Query Language**: 6-keyword Pythonic syntax (`by`, `sort`, `del`, `table`, `index`, `unique`) with slicing (`[:10]`) and compile-time guardrails.
* **O(1) Robin Hood Index**: 128-way sharded primary key map with backward-shift deletion recycles slots without tombstones.
* **CRC32 WAL & Atomic Snapshots**: Append-only durability with torn-write auto-healing and background `.snap` compaction.
* **Zero External Dependencies**: Pure C11 core compiling to a single self-contained CPython extension with zero daemons or network overhead.

## Quick Start

```python
import vex

with vex.open("app.wal", workers=4, fsync=True) as db:
    # 1. Define schemas (with optional foreign keys for auto-joins)
    db.q("table users  { id: u64 key, name: str, country: str, age: i64 }", sync=True)
    db.q("table orders { id: u64 key, user_id: u64 -> users.id, amount: f64, paid: bool }", sync=True)

    # 2. Insert via query syntax or fast-path dict put (SPSC micro-batched)
    db.q('users += [{ id: 1, name: "Aarav", country: "IN", age: 29 }, { id: 2, name: "Meera", country: "IN", age: 31 }]', sync=True)
    db.put("orders", {"id": 10, "user_id": 1, "amount": 250.5, "paid": True}, sync=True)
    db.put("orders", {"id": 11, "user_id": 1, "amount": 149.5, "paid": True}, sync=True)

    # 3. Query top-to-bottom (GIL-free, parameterized)
    res = db.q(
        'orders[paid, amount > $min] & users by users.country { n: count(), rev: sum(amount) } sort -rev [:10]',
        min=100.0,
    )
    print(res.dicts())        # [{'country': 'IN', 'n': 2, 'rev': 400.0}]

    # 4. Zero-copy NumPy / memoryview handoff (PEP 3118 Buffer Protocol)
    rev_np = res["rev"].np()  # read-only view directly over 64B-aligned C memory

    # 5. Compact tombstones + truncate WAL into an atomic binary .snap file
    db.checkpoint()
```

---

## VQL Studio

Launch the interactive browser or desktop studio (`make gui-web` or `make gui`) to write syntax-highlighted VQL, inspect live engine metrics and `VXWAL001` binary headers, and browse the built-in language reference:

![Vex VQL Studio showing VQL editor, live engine metrics, WAL hex inspector, and query output](images/Vex%20studio.png)

![Built-in Vex Query Language Reference inside VQL Studio](images/vex%20studio%20docs.png)

---

## The 6-Keyword Query Language

Start with a table name, filter/slice/lookup with `[...]`, pick columns as bare names, aggregate with `{...}`, and flow strictly left-to-right.

**Reserved keywords:** `by`, `sort`, `del`, `table`, `index`, `unique` *(plus `and`, `or`, `not`, `in`, `true`, `false`, `null`)*.

| Operation | Vex Syntax | Notes |
| --- | --- | --- |
| **Full scan** | `users` | All rows & columns |
| **Select columns** | `users name, age` | Comma-separated fields |
| **Computed alias** | `orders id, total: price * qty` | `alias: expr` |
| **O(1) PK lookup** | `users[42] name, age` | Robin Hood hash index |
| **Filter** | `users[age >= $min, country == "IN"]` | Comma inside `[...]` means `and` |
| **Slice / limit** | `users[:10]` / `users[20:30]` | Python slicing |
| **Sort** | `users sort -age, name` | `-` = descending |
| **Left outer join** | `orders + users(user_id == id)` | Shorthand: `orders + users(user_id)` |
| **Full outer join** | `orders ++ users(user_id == id)` | Unmatched rows kept on both sides |
| **Inner join** | `orders & users` | Auto-joins via declared `->` FK |
| **Distinct** | `users by country` | `by` without `{}` |
| **Aggregate** | `orders by country { n: count(), rev: sum(amount) } [rev > 5000]` | `count`, `sum`, `avg`, `min`, `max` |
| **Multi-step bind** | `vips = orders[amount > 500]` followed by `vips sort -amount [:5]` | Replaces nested subqueries |
| **Insert / Upsert** | `users += { id: 1, name: "A" }` / `users |= { id: 1, age: 30 }` | |
| **Update / Delete** | `users[42] = { age: 31 }` / `del users[age < 18]` | Also supports `users[42] -=` |
| **DDL** | `table t { id: u64 key, uid: u64 -> users.id }` / `del table t` | Types: `i64`, `u64`, `f64`, `bool`, `str` |
| **Explain plan** | `? users[age > 25] name` | Populates `res.explain` |

**Built-in safety guards:**

* `del users` and `users = {...}` are **syntax errors**—full-table mutations require explicit `[:]` (`del users[:]`).
* Auto-joins (`a + b` without `(lcol == rcol)`) require a declared foreign key to prevent accidental Cartesian explosions.

---

## Python API Reference

* `vex.open(path=None, workers=4, chunk_rows=4096, max_readers=64, flush_interval_us=2000, fsync=False)` — Opens an in-memory (`path=None`) or WAL-backed database with an exclusive OS `flock`.
* `db.q(query, sync=False, **params) -> Result` — Executes 1..N statements with the GIL released. Parameters bind `$name` safely.
* `db.put(table, row_dict, upsert=False, sync=False)` — Bypasses the parser and pushes into the per-thread SPSC write mesh.
* `db.flush()` — Blocks until all staged async writes commit to the MVCC snapshot and WAL.
* `db.checkpoint()` — Writes an atomic CRC32-verified `<path>.snap` snapshot (stripping tombstones) and truncates the WAL.
* `db.stats() -> dict` — Returns `queries_executed`, `mutations_committed`, `wal_lsn`, `chunks_pruned`, `logs_emitted`, `logs_dropped`, `checkpoints`.
* `vex.simd_backend() -> str` — Returns the active runtime SIMD backend (`"avx2"`, `"neon"`, or `"scalar"`).
* `Result`: `len(res)`, `res.columns`, `res.rows()`, `res.dicts()`, `res["col"] -> ColumnView`, `res.explain`.
* `ColumnView`: `memoryview(col)`, `col.np()`, `col.to_list()`, `col.dtype`, `col.has_nulls` (zero-copy views pin the underlying C arena via reference counting).

---

## Architecture Highlights

* **Columnar Chunks & Zone Maps:** 64-byte-aligned column arrays with per-chunk `min`/`max` zone maps for $O(1)$ chunk skipping and Umbra-style 16-byte inline/prefix German strings.
* **Portable Runtime SIMD Dispatch:** Separate `-mavx2` translation unit selected once at runtime via CPU feature detection (falls back cleanly to scalar/NEON with zero `-march=native` wheel crashes).
* **O(1) PK Index & Tombstones:** 128-way sharded Copy-On-Write Robin Hood hash index paired with refcounted per-chunk deletion bitmaps (`vx_delmap_t`), O(N+M) hash joins, O(N) hash group-by, and lock-free merge sort.
* **Scaled-by-Default Concurrency:** Lock-free Epoch-Based Reclamation (EBR) MVCC snapshots with TLS thread-exit slot recycling, morsel-driven multicore worker pool, per-thread SPSC write mesh with hybrid count/latency triggers, and a non-blocking 64-byte binary SPSC logger.
* **Durability:** IEEE CRC32 Write-Ahead Log with torn-write auto-healing, atomic `.snap.tmp -> fdatasync -> rename` checkpoint compaction, and exclusive `flock` process locking.

---

## Build, Sanitize & Test

```bash
# Build native Python extension (portable SIMD dispatch)
make build-py

# Run full Python test suite (M6 + M7 + M9)
PYTHONPATH=python pytest -v tests/test_m6_python.py tests/test_m7_p0.py tests/test_m9_release.py

# C11 Memory safety (ASan + UBSan) & Race detection (TSan)
make test-m9-mem
make test-m9-race

# Release-mode throughput & latency SLA benchmarks
make bench-m8 bench-m9

# libFuzzer parser & WAL crash-recovery fuzzers (requires clang)
make fuzz-parser fuzz-wal
```

---

## License

This project is licensed under the MIT License - see the [LICENSE](file:///home/avisheks/Documents/dibs/vex/LICENSE) file for details.


