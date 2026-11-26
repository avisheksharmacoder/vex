# Current Codebase Status, Known Issues & Roadmap Gaps

This page tracks the verified state of the codebase so agents do not misread docs or assume V2 modules are wired when they are still standalone.

---

## 1. Observed Verification Baseline

* **V1 C Suites:** `test_m1_mem`–`test_m5_mem`, `test_m7_mem`–`test_m10_mem`, `test_m9_race`, and `test_m10_race` all pass.
* **V2 Standalone C Binaries (`build/`):** All 16 `test_v2_*` binaries, all 10 `test_v2_*_tsan` binaries (under `setarch -R`), `test_oom_sweep`, and `test_tsan_c_core` (under `setarch -R`) pass.
* **Python Suite (`PYTHONPATH=python pytest -v`, Python 3.14.7):**
  * Passing: `test_m6_python.py` (6/6), `test_m9_release.py` (4/4), `test_m10_audit.py` (5/5), `test_cli.py` (4/4), `test_crash_loop.py` (20/20, vacuous while failpoints/V2 storage are unwired).
  * **Failing:** [tests/test_m7_p0.py](file:///home/avisheks/Documents/dibs/vex/tests/test_m7_p0.py#L52-L79) (`test_p0_py_parameterized_writes_wal_replay`) fails with `vex.VexError: expected field name in record` on `"orders |= { ... }"`.
  * **Timing Out:** [tests/test_tsan_313t_torture.py](file:///home/avisheks/Documents/dibs/vex/tests/test_tsan_313t_torture.py) times out (`EXIT CODE: 124` at 55s).

---

## 2. Active Bugs & Shims to Be Aware Of

1. **V1 Upsert Broken by `vx_normalize_query`:**
   In [python/vex_py.c](file:///home/avisheks/Documents/dibs/vex/python/vex_py.c#L215-L295), `vx_normalize_query()` uses `strchr(in, '|')` to detect V2 pipe syntax and accidentally mangles V1's `|=` upsert operator (`orders |= { ... }`).
2. **Python Bindings Still Backed by V1 (`include/vx_db.h`):**
   * `py_vex_open()` parses `max_memory_bytes` and ignores it, opening `<dir>/wal` via V1 `vx_db_open()`.
   * `VexTxObject` implements a Python-level OCC bitmask and buffered SQL string replay (`g_tls_active_tx`, `occ_mu`) instead of `vx_tx.c`, ignores `res->ok` on commit replay ([python/vex_py.c](file:///home/avisheks/Documents/dibs/vex/python/vex_py.c#L1013-L1019)), and executes `db.put()` immediately without transaction staging.
   * `vex_result_arrow_stream_py()` ([python/vex_py.c](file:///home/avisheks/Documents/dibs/vex/python/vex_py.c#L1517-L1523)) returns a mock capsule `(void *)1` instead of calling [python/vex_arrow.c](file:///home/avisheks/Documents/dibs/vex/python/vex_arrow.c).
   * `vex_db_verify_crc()` ([python/vex_py.c](file:///home/avisheks/Documents/dibs/vex/python/vex_py.c#L849-L852)) is a no-op stub, and `_disable_indexes` is ignored.
3. **Tracker Drift:** [docs/v2-engine/README.md](file:///home/avisheks/Documents/dibs/vex/docs/v2-engine/README.md#L53-L64) marks M11/M12 `Pending` and M13/M14/M16/M17 `Completed`—actually, standalone C modules exist across M11–M17 while engine wiring is still incomplete across all milestones.

---

## 3. Unimplemented V2 Pieces & Integration Specs (`docs/v2/24`–`39`)

The following components exist as design specifications in `docs/v2/` and are **not yet created or wired** in `src/`, `python/`, or `.github/`:

| Spec File | Missing Code / Unapplied Integration |
| :--- | :--- |
| [docs/v2/24](file:///home/avisheks/Documents/dibs/vex/docs/v2/24) | Dedicated `<dir>/LOCK` and numbered WAL segment rotation (`wal_000001.log`, ...) |
| [docs/v2/25](file:///home/avisheks/Documents/dibs/vex/docs/v2/25) | Spilling Grace Hash Join (`work_mem`-bounded 64-partition spill + recursive skew fallback) |
| [docs/v2/26](file:///home/avisheks/Documents/dibs/vex/docs/v2/26) | `src/vx_mmap_guard.h` & `src/vx_mmap_guard.c` (`SIGBUS` / Win32 SEH `mmap` fault guard) |
| [docs/v2/28](file:///home/avisheks/Documents/dibs/vex/docs/v2/28) | V2 `vx_simd.c` (`vx_simd_ops_t` dispatch), `src/vx_op_slice.c`, `src/vx_src_hash_agg.c` |
| [docs/v2/29](file:///home/avisheks/Documents/dibs/vex/docs/v2/29) | Spilling `vx_sink_hash_agg.c` (`work_mem`-bounded partition eviction) |
| [docs/v2/30](file:///home/avisheks/Documents/dibs/vex/docs/v2/30), [35](file:///home/avisheks/Documents/dibs/vex/docs/v2/35), [38](file:///home/avisheks/Documents/dibs/vex/docs/v2/38) | Wiring V2 catalog, `MANIFEST`, flusher/compactor, `vx_tx`, `vx_plan` lowering, and pipeline execution into `vx_db.c` and `vx_exec.c` (eliminating `materialize_table()`) |
| [docs/v2/31](file:///home/avisheks/Documents/dibs/vex/docs/v2/31), [32](file:///home/avisheks/Documents/dibs/vex/docs/v2/32) | Remaining `vx_idx.c` / `vx_plan.c` integration updates |
| [docs/v2/37](file:///home/avisheks/Documents/dibs/vex/docs/v2/37) | Shared slice-by-8 `src/vx_crc32.h` & `src/vx_crc32.c` |
| [docs/v2/33](file:///home/avisheks/Documents/dibs/vex/docs/v2/33), [34](file:///home/avisheks/Documents/dibs/vex/docs/v2/34), [36](file:///home/avisheks/Documents/dibs/vex/docs/v2/36) | `python/vex_py.c` + `python/vex_arrow.c` wiring, V2 `vex/cli.py`, `pyproject.toml` & `cibuildwheel` free-threaded (`3.14t`/`3.15t`) CI |
| [docs/v2/39](file:///home/avisheks/Documents/dibs/vex/docs/v2/39) | Unified V2 `Makefile` with flavor-isolated build trees (`release`, `asan`, `tsan`) |

## 4. Future V3 Drafts (`docs/v3/`)

`docs/v3/` contains early design documents for a planned v3.0 roadmap (storage block-source abstraction, row-level MVCC, window operators, HyperLogLog planner, remote cache). **No v3 code exists in the repository.**

