# Task 4 fts17 validation

Official best reference: `fts12`, `f949146be7cab774a78bfa9ce1a709b047f3bff5`, **36.549/50**. The submitted fts16 result was **21.289/50**. Its BENG build time rose from 2.122s to 5.728s despite improved query times. This candidate has not been officially evaluated and has no guaranteed score.

## Bottleneck evidence and changes

Local slow-run logs identify `TimerWK0_SystemPkgLoad` holding the runtime schema lock while package metadata is inserted. DDL executor transactions time out and retry while this lock is held. For example, the old `fts16-paged-score2` log records the system package transaction acquiring the lock at 16:13:44.462464, with DDL lock timeouts still occurring at 16:13:48.160361. Loading dbms_stats took 5.729372s in that run. This is direct evidence for a local source of build stalls; the official run supplied no execution log to prove it had the same cause.

The original mini-mode DDL builder also has one worker. FTS inverted and document-word auxiliary tables are independent after the shared document-ID mapping is ready, but their build SQL runs sequentially.

Changes on top of fts16 (retaining the prior fts12 semantics and compatible dictionary format):

- Execute two auxiliary build jobs concurrently in mini mode, with the default four PX workers per job replacing the old single-job eight-worker default. Non-mini worker count stays unchanged. Explicit higher parallelism remains respected. All existing DDL states, snapshot waits, checksums, success/failure handling and resource admission still execute.
- Batch routine parameter live/history metadata writes in the existing transaction, bounded by 64 rows or 64KiB of accumulated values. Preserve parameter IDs/schema versions, escaping, history-only handling, and affected-row validation. This reduces statements and runtime-schema-lock hold time during background package loading. No package loading is skipped or delayed to move work outside the benchmark.
- Fuse pure ASCII BENG splitting and lowercase normalization for utf8mb4_bin/general_ci/unicode_ci. Other input/collations use the existing pipeline. A direct reference oracle verifies byte classification, output order, long tokens, embedded NUL, reuse and token lifetime. Unchanged tokens can borrow caller-owned input as the existing whitespace parser does; normalizer-owned fallback buffers are copied.
- Use a single-owner node allocator for the IK arbitrator's existing map. Keep its keys, hash, ordering and arbitration decisions.
- A trial specialized union loop did not show sufficiently consistent advantage and is absent from the final source. The retrieval implementation remains the previously validated fts16 version.

Benchmark input, timing code, scoring, baseline JSON, SQL workload, workflow and build configuration are unchanged relative to fts12. No measured report is modified.

## Verification of the final source

- Complete Release build and **19 native tests passed**, including the prior dictionary/decoder/charset/ordering tests and the fused English parser reference oracle. Pure ASCII setup is also checked to avoid allocating the old pipeline.
- Four complete default benchmarks in ABBA order, followed by SQL differential tests, produced identical **75,041-byte** correctness transcripts. These include the existing FTS score/ranking/prepared-statement/collation cases, 256 real-dictionary smart/max-word fuzz calls, and routine parameter counts 0, 1, 63, 64, 65, 129 with calls and metadata checks.
- A 65,792-document ranking/LIMIT/OFFSET fallback and original invalid-input errors matched fts12 exactly.
- Concurrent construction of two FTS indexes and an ordinary index, subsequent insert/update/delete operations, forced ordinary-index scans and restart persistence matched fts12 exactly.
- Eight concurrent clients: **4,800 TOKENIZE calls per binary**, identical outputs across clients and versions.
- All official query hit counts match: **8001 / 11000 / 7332 / 20**.
- Final native production/benchmark snapshot matches **51** expected source files, with no temporary instrumentation.

## Full final benchmark measurements

Dedicated fresh instances, one active benchmark at a time, no concurrent compilation/profiling. Defaults: ROWS=20000, BATCH=500, ROUNDS=3000, QUERY_ROUNDS=200, SAMPLES=3, WARMUP=30, SKIP_LOAD=0. Baseline executable SHA256 matches the independently rebuilt fts12 recovery executable.

Every final measurement is included. There was a large baseline build stall; it is retained and is **not used to claim a 77% speedup**. For a conservative build comparison, compare the slowest candidate with the fastest baseline:

| Build metric | Fastest fts12 seconds | Slowest candidate seconds | Conservative time reduction |
|---|---:|---:|---:|
| build_ik_all_sec | 6.077 | 4.171 | 31.36% |
| build_ik_content_sec | 4.142 | 2.840 | 31.43% |
| build_beng_en_sec | 2.335 | 1.431 | 38.72% |

| Run | IK all s | IK content s | BENG build s | IK tokenize ms | BENG tokenize ms | CN query ms | BENG query ms | Mixed query ms | LIMIT query ms | SELECT1 ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| base1 | 28.4410 | 6.9190 | 3.9570 | 0.1396 | 0.1093 | 0.6925 | 0.9264 | 0.7675 | 1.1200 | 0.0868 |
| score1 | 3.6630 | 2.4330 | 1.4300 | 0.1455 | 0.1209 | 0.7888 | 0.9700 | 0.8496 | 1.0741 | 0.0878 |
| score2 | 4.1710 | 2.8400 | 1.4310 | 0.1601 | 0.1164 | 0.7387 | 0.9192 | 0.8458 | 1.1680 | 0.0993 |
| base2 | 6.0770 | 4.1420 | 2.3350 | 0.1376 | 0.1294 | 0.8306 | 0.9440 | 0.8979 | 1.2256 | 0.1120 |

| Metric | fts12 arithmetic mean | Candidate arithmetic mean | Mean time reduction |
|---|---:|---:|---:|
| build_ik_all_sec | 17.2590 | 3.9170 | 77.30% |
| build_ik_content_sec | 5.5305 | 2.6365 | 52.33% |
| build_beng_en_sec | 3.1460 | 1.4305 | 54.53% |
| tokenize_ik_avg_ms | 0.1386 | 0.1528 | -10.25% |
| tokenize_beng_avg_ms | 0.1193 | 0.1187 | 0.59% |
| query_cn_avg_ms | 0.7615 | 0.7637 | -0.29% |
| query_beng_avg_ms | 0.9352 | 0.9446 | -1.01% |
| query_mixed_avg_ms | 0.8327 | 0.8477 | -1.80% |
| query_limit_avg_ms | 1.1728 | 1.1210 | 4.41% |

**Performance limitation:** build improvement is clearly reproduced. The final end-to-end tokenizer/query measurements remain noisy and do **not** establish that all nine wall-time metrics improve. This candidate must not be represented as a verified all-metric improvement, a verified improvement over the official 36.549 score, or a guaranteed 50-point version. The checked-in older CI baseline is not used to infer an official score. The previous official best branch is preserved.

All earlier trials are retained separately under `fts17-initial-validation`, `fts17-final-validation`, and the steady-state diagnostic directories. Those trials are not substituted for the final measurements above.

Artifacts on E:

- `E:\ckwjx\Oceanbase-seekdb-work\fts17-ready-validation`: final raw reports, exact SQL outputs and comparison JSON.
- `E:\ckwjx\Oceanbase-seekdb-work\fts17-unit-validation\unit.log`: 19 passing native tests.
- `E:\ckwjx\Oceanbase-seekdb-work\fts17-extra-results`: large corpus and error differential outputs.
- `E:\ckwjx\Oceanbase-seekdb-work\fts17-parallel-results`: concurrent FTS/ordinary DDL, mutation and restart differential.
- `E:\ckwjx\Oceanbase-seekdb-work\fts17-concurrent-results`: concurrency validation.

Final tested executable SHA256: `fac8a3c7dd6e86992b0a8db3f2303b503e64746cd78fb12a253125f1b16dfb62`.
Baseline/recovery executable SHA256: `d2491fee9f282e3a8cdbc7a062ee8fb5d2965059ae5c192c79a6c38523d5f143`.
