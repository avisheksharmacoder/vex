# Testing, Benchmarking & Verification Policy (No Multi-Gigabyte Runs)

> [!CAUTION]
> **STRICT MANDATE:** **NEVER** create, propose, run, or require multi-gigabyte (`>= 1 GB`) datasets, benchmarks, soak tests, or stress tests to validate Vex in local development, automated test suites, or CI pipelines.

This document defines the mandatory verification rules for all engineers and automated coding agents working on Vex.

---

## 1. Prohibited Practices

Do **NOT** under any circumstances:

1. **Generate or ingest multi-gigabyte datasets** (e.g., `1 GB`, `2 GB`, `10 GB`, `20 GB`, or `50M+` rows) for unit tests, integration tests, correctness checks, crash tests, or benchmarks.
2. **Run multi-minute or multi-hour soak/endurance tests** to trigger eviction, compaction, or spilling.
3. **Use multi-gigabyte "in-memory baseline" budgets** (e.g., `work_mem = 2 GB`) in spill-equivalence tests.
4. **Gate PRs, milestones, or release checks** on multi-gigabyte cold-start, join, sort, or ingestion benchmarks.

Any historical mentions of `2 GB`, `10 GB`, `20 GB`, `50M rows`, or multi-hour soaks in older design notes (such as [docs/v2/1-docs](file:///home/avisheks/Documents/dibs/vex/docs/v2/1-docs), [docs/v2/19](file:///home/avisheks/Documents/dibs/vex/docs/v2/19), [docs/v2-engine/README.md](file:///home/avisheks/Documents/dibs/vex/docs/v2-engine/README.md), or `docs/v3/*`) are **strictly superseded by this policy** and must **NOT** be implemented or executed.

---

## 2. Mandatory Alternative: Shrink Budgets, Not Laptops

Every out-of-core, compaction, spilling, caching, and concurrency code path must be validated by **shrinking memory/cache/chunk budgets over small, fast datasets** instead of scaling data to gigabytes.

### Required Scale Guardrails

| Parameter | Hard Limit | Recommended Target |
| :--- | :--- | :--- |
| **Test/benchmark dataset size on disk or RAM** | `<= 50 MB` | `1 MB` – `20 MB` (or `1K` – `100K` rows) |
| **Small-budget spill / eviction cap (`max_memory_bytes`, `work_mem_bytes`)** | `<= 4 MB` | `64 KB` – `2 MB` |
| **In-memory comparison baseline budget** | `<= 128 MB` | `16 MB` – `64 MB` |
| **Per-test execution duration** | Short / bounded | `< 5 s` (unit) / `<= 15 s` (concurrency) |

---

## 3. How to Validate Each Subsystem Within Guardrails

* **Hot/Cold Tier Eviction & Leveled Compaction:**
  Configure a small `max_memory_bytes` (e.g., `512 KB` – `4 MB`) and small `chunk_rows` (e.g., `64` – `512`), then ingest `2x` – `5x` that cap (`2 MB` – `15 MB` total) with targeted deletes to exercise flusher backpressure, `.vx`/`.del` rotation, compaction, and deferred `munmap` / `unlink`.
* **Out-of-Core Spilling (`ExternalSort`, `TopK`, `HashAgg`, `GraceHashJoin`):**
  Force multi-pass spilling and recursive partitioning using `work_mem_bytes = 64 KB` – `512 KB` on `5K` – `100K` rows (`<= 20 MB`), and compare bit-for-bit against an in-memory run with `work_mem_bytes = 32 MB` – `64 MB`.
* **Zero-Deserialization `mmap` Cold Start:**
  Verify `vex.open()` does not deserialize row data or rebuild full PK indexes on startup by inspecting memory/timing or structural invariants over a small multi-segment directory (a few MB across multiple `.vx` files), rather than building multi-GB databases.
* **Crash Consistency & Fault Injection:**
  Use deterministic `VX_FAILPOINT` / `VEX_CRASH_POINT` subprocess restarts, `VEX_ENABLE_OOM_INJECT` exhaustive `k = 1..N` allocation sweeps, and `SIGBUS`/SEH truncation tests on small files (`<= 1 MB`).
* **Concurrency & Sanitizer Verification:**
  Validate thread safety and memory safety using ASan + UBSan + LSan and TSan on compact multi-threaded workloads (`test_tsan_c_core`, short bounded Python worker tests) rather than heavy data-volume stress runs.

---

## 4. Enforcement Checklist for Code & Benchmark Changes

Before adding or running any test, benchmark script, or CI step, verify:

1. Total generated/loaded data stays strictly under `50 MB` (and `<= 100K` rows for routine tests).
2. No `10 GB`, `20 GB`, `2 GB`, or multi-gigabyte parameters appear in test fixtures, benchmarks, or CI workflows.
3. Out-of-core mechanics are triggered via tight `chunk_rows`, `max_memory_bytes`, `work_mem_bytes`, or `remote_cache_bytes` knobs.

