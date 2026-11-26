# Build, Sanitizer & Test Commands

> [!CAUTION]
> All testing and benchmarking is strictly subject to [docs/testing_policy.md](file:///home/avisheks/Documents/dibs/vex/docs/testing_policy.md): **never** generate or execute multi-gigabyte (`>= 1 GB`) datasets, benchmarks, or endurance soak tests. Validate out-of-core code paths by shrinking `max_memory_bytes`, `work_mem_bytes`, and `chunk_rows` over small datasets (`<= 50 MB`).

---

## 1. Wired V1 Build & Test Targets (`Makefile`)

### C11 Memory Safety (ASan + UBSan)
```bash
make test-mem test-m2-mem test-m3-mem test-m4-mem \
     test-m5-mem test-m7-mem test-m8-mem test-m9-mem test-m10-mem
```

### C11 ThreadSanitizer (TSan)
Always ensure `setarch $(uname -m) -R` wraps TSan execution on Linux (`Makefile` sets `SETARCH` automatically for `test-*-race` targets):
```bash
make test-race test-m2-race test-m3-race test-m4-race \
     test-m5-race test-m7-race test-m8-race test-m9-race test-m10-race
```

### Python Extension & Pytest Suites
```bash
# Build vex._vex in-place into python/vex/
make build-py

# Run V1 Python compatibility + CLI tests
PYTHONPATH=python pytest -v \
  tests/test_m6_python.py \
  tests/test_m7_p0.py \
  tests/test_m9_release.py \
  tests/test_m10_audit.py \
  tests/test_cli.py
```

### libFuzzer Smoke (Requires `clang`)
```bash
make fuzz-parser fuzz-wal
```

---

## 2. Standalone V2 C Binaries (`build/`)

[Makefile](file:///home/avisheks/Documents/dibs/vex/Makefile) does **not** yet define rules for `tests/test_v2_*.c`, `tests/test_oom_sweep.c`, or `tests/test_tsan_c_core.c`. Prebuilt binaries exist in `build/`:

* **Unit & ASan+UBSan binaries:**
  `build/test_v2_abi`, `build/test_v2_str_simd`, `build/test_v2_filter_project`, `build/test_v2_num_simd`, `build/test_v2_pipeline`, `build/test_v2_hash_agg_builtin`, `build/test_v2_hash_join`, `build/test_v2_sort`, `build/test_v2_seg_man_fmt`, `build/test_v2_seg`, `build/test_v2_manifest`, `build/test_v2_bg`, `build/test_v2_enc`, `build/test_v2_idx_plan`, `build/test_v2_art`, `build/test_v2_tx`, `build/test_oom_sweep`
* **TSan binaries (must run with `setarch $(uname -m) -R`):**
  `build/test_v2_*_tsan` (`art`, `bg`, `enc`, `hash_join`, `idx_plan`, `manifest`, `seg_man_fmt`, `seg`, `sort`, `tx`) and `build/test_tsan_c_core`:
  ```bash
  setarch "$(uname -m)" -R ./build/test_tsan_c_core
  ```
* If recompiling V2 unit tests manually, pass `-Isrc` (with `-Iinclude` only when a test explicitly includes `include/vx_ast.h`, e.g. `tests/test_v2_idx_plan.c`) and compile `src/vx_simd_avx2.c` with `-mavx2` on `x86_64`. See [docs/v2/39](file:///home/avisheks/Documents/dibs/vex/docs/v2/39) for the planned unified V2 `Makefile`.

---

## 3. V2 Python Integration Suites

* [tests/test_crash_loop.py](file:///home/avisheks/Documents/dibs/vex/tests/test_crash_loop.py): Invokes subprocesses with `VEX_CRASH_POINT` / `VEX_CRASH_HIT`. Until `VEX_ENABLE_FAILPOINTS=1` and V2 storage are wired into `vex._vex`, subprocesses exit `0` without hitting failpoints.
* [tests/test_tsan_313t_torture.py](file:///home/avisheks/Documents/dibs/vex/tests/test_tsan_313t_torture.py): Exercises 16 concurrent threads (`tx_transfer_worker`, `churn_worker`, `view_pinner_worker`, `verifier_worker`) and checks `sys._is_gil_enabled() is False` when `sys.flags.gil == 0`.

