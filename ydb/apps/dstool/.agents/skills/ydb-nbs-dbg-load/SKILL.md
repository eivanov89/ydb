---
name: ydb-nbs-dbg-load
description: Use for dedicated NbsDbgLike load-tablet lifecycle, runs, sweeps, and artifact recovery through ydb-dstool nbs-load.
---

# NbsDbgLike load automation

1. Read ydb/docs/en/core/contributor/load-actors-nbs-dbg-like.md for configuration schemas, command recipes, and deployment requirements. Use dedicated load tablets and an explicit database and gRPC endpoint. Select the coordinator with `--node-id` when needed; a different gateway does not change a saved coordinator.
2. Use `create` with a TAllocConfig file, then `run` with a TEvLoadTestRequest containing only NbsDbgLikeLoad. Use `sweep` for ordered inflight values and trials on existing tablets. The CLI owns configuration parsing, polling, checkpointing, retries, and median selection; do not reimplement them in shell loops.
3. Preserve the artifact directory printed to stderr. `run --no-wait` returns a handle; use `results --handle DIR --wait` to retrieve a single run. Handle-based results and stop save terminal replies and can read saved results offline. Resume interrupted work with `run --resume DIR` or `sweep --resume DIR`, without new run settings. Unknown/expired handles or a lost coordinator require investigation; never launch replacement work automatically.
4. Check the execution state and the CLI verdict separately. SUCCEEDED with nonzero measured WritesErr or ReadsErr fails the verdict unless the user selected `--allow-io-errors`. That flag does not excuse execution failure, cancellation, or an unresolved outcome. Histogram latencies are in microseconds; 64-bit tablet IDs and owner indices are JSON strings; node IDs are numbers.
5. Use `stop` for the saved run identity and wait for confirmed termination before starting another trial. Coordinator loss does not prove remote workers stopped. Completion covers client-request drain, not background flush/erase.
6. Serialize create and delete for each Hive owner index, including across controllers and HTTP; reserve distinct indices across databases sharing a Hive. Concurrent lifecycle mutation is unsupported. Preflight checks do not make create atomic. After an ambiguous create or delete, inspect Hive and the reported tablet ID before deciding whether to retry. Delete allocations only when cleanup is requested. Preserve local results. Same-coordinator overlap checks are not cluster-wide locks; do not reuse these tablets through another controller while a run may be active.

Implementation and validation: ydb/apps/dstool/lib/nbs_load.py owns the reusable workflow; ydb/apps/dstool/lib/dstool_cmd_nbs_load.py exposes commands; ydb/apps/dstool/lib/ut/test_nbs_load.py tests recovery and verdicts. Follow the repository build rules for ydb/apps/dstool/lib/ut.
