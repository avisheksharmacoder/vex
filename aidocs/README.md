# Vex AI Agent Orientation (`aidocs/`)

Welcome. This directory is a concise, code-verified map for AI coding agents exploring **Vex** (a zero-dependency C11 columnar database engine with Python C-API bindings).

> [!IMPORTANT]
> **Read These Two Facts Before Making Any Edits:**
> 1. **Dual-Engine State:** The active build ([Makefile](file:///home/avisheks/Documents/dibs/vex/Makefile) and [python/setup.py](file:///home/avisheks/Documents/dibs/vex/python/setup.py)) still compiles and runs the **V1.0 engine** (`include/` + `src/vx_db.c` + `src/vx_exec.c`). Most **V2.0 modules** (M11–M17) live alongside it in `src/` as standalone, unit-tested components that are **not yet wired** into `vx_db.c`, `vx_exec.c`, or `python/vex_py.c`.
> 2. **Strict Testing Policy:** **NEVER** create, propose, or execute multi-gigabyte (`>= 1 GB`) datasets, benchmarks, or soak tests. Follow [docs/testing_policy.md](file:///home/avisheks/Documents/dibs/vex/docs/testing_policy.md) strictly by shrinking memory/chunk budgets over small datasets (`<= 50 MB`).

---

## Recommended Reading Order

1. **[docs/testing_policy.md](file:///home/avisheks/Documents/dibs/vex/docs/testing_policy.md)** — Mandatory limits on dataset sizes, memory budgets, and benchmark/soak prohibitions.
2. **[architecture.md](file:///home/avisheks/Documents/dibs/vex/aidocs/architecture.md)** — Active V1 execution/storage architecture, V2 modular architecture, and the `include/` vs `src/` header-name collision pitfall.
3. **[module_map.md](file:///home/avisheks/Documents/dibs/vex/aidocs/module_map.md)** — Complete inventory of `include/`, `src/`, `python/`, `tests/`, `fuzz/`, `docs/`, and `.github/`, labeled as **Wired (V1)** vs **Standalone (V2)**.
4. **[build_and_test.md](file:///home/avisheks/Documents/dibs/vex/aidocs/build_and_test.md)** — Exact build, ASan/UBSan, TSan (`setarch -R`), Python `pytest`, and standalone V2 binary commands.
5. **[current_status.md](file:///home/avisheks/Documents/dibs/vex/aidocs/current_status.md)** — Observed test failures, V1-to-V2 shims/stubs in `python/vex_py.c`, and remaining integration specs (`docs/v2/24`–`39`).

---

## Repository Snapshot

| Area | Path | Purpose |
| :--- | :--- | :--- |
| **V1 Public Headers** | [include/](file:///home/avisheks/Documents/dibs/vex/include) | Headers for the currently wired V1 C11 engine |
| **C Engine Sources** | [src/](file:///home/avisheks/Documents/dibs/vex/src) | Active V1 engine files + standalone V2 modules and V2 headers |
| **Python Package & Extension** | [python/](file:///home/avisheks/Documents/dibs/vex/python) | CPython extension ([vex_py.c](file:///home/avisheks/Documents/dibs/vex/python/vex_py.c)), Arrow exporter ([vex_arrow.c](file:///home/avisheks/Documents/dibs/vex/python/vex_arrow.c)), `vex` PEP 561 package, CLI |
| **Test Suite** | [tests/](file:///home/avisheks/Documents/dibs/vex/tests) | V1 C/Python tests (`test_m1`–`test_m10`), CLI tests, V2 C unit/TSan/OOM tests, V2 Python harness |
| **Fuzz Targets** | [fuzz/](file:///home/avisheks/Documents/dibs/vex/fuzz) | libFuzzer targets for parser (`fuzz_parser.c`) and WAL (`fuzz_wal.c`) |
| **Docs & Specs** | [docs/](file:///home/avisheks/Documents/dibs/vex/docs) | User/V1 docs, [v2-engine tracker](file:///home/avisheks/Documents/dibs/vex/docs/v2-engine/README.md), V2 design specs (`docs/v2/`), future V3 drafts (`docs/v3/`) |
| **Build & CI** | [Makefile](file:///home/avisheks/Documents/dibs/vex/Makefile), [.github/workflows/ci.yml](file:///home/avisheks/Documents/dibs/vex/.github/workflows/ci.yml) | Currently builds V1 targets (`m1`–`m10`, `build-py`, `fuzz-*`) |

---

## Fast-Path Rules for Editing Code

* **Before editing a C file in `src/`**, check whether it is **V1-wired** (compiled by `Makefile` / `setup.py` with `-Iinclude`) or **V2-standalone** (compiled with `-Isrc` or relative `"vx_*.h"`). Never mix `include/vx_chunk.h` and `src/vx_chunk.h` types in the same translation unit.
* **When running TSan binaries on Linux**, prefix execution with `setarch $(uname -m) -R` to prevent `FATAL: ThreadSanitizer: unexpected memory mapping` caused by ASLR.
* **Preserve V1 compatibility**: All M6, M7, M9, M10, and CLI tests form the compatibility floor; check `make test-m9-mem`, `make test-m10-mem`, and `PYTHONPATH=python pytest -v tests/test_m6_python.py tests/test_m7_p0.py tests/test_m9_release.py tests/test_m10_audit.py tests/test_cli.py`.

