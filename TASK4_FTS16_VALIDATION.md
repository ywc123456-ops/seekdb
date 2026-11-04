# Task 4: fts16 validation

Parent and official comparison: `fts12` / `f949146be7cab774a78bfa9ce1a709b047f3bff5`, official score **36.549/50**.
The candidate has **no official score yet**. This report makes no claim of 50 points or a guaranteed official improvement.

## Implementation

- Immutable, arena-owned paged BMP character-code index for the existing double-array trie. Only occupied 256-character pages allocate storage. The serialized dictionary, character codes and legacy hash lookup remain unchanged. Supplementary, malformed and multi-character strings retain the legacy path; optional-index allocation failure also retains it.
- Inline the existing character-map accessors and lookup. Validate/decode common UTF-8 characters once for length and classification, preserving the original malformed-input fallback.
- Keep CJK pending matches in a small contiguous array with stable compaction and dynamic growth. Relocation explicitly preserves the dictionary pointer, which `ObDATrieHit::operator=` otherwise omits.
- Remove the single-owner word map node allocator's spinlock. Preserve its node representation, hash, equality and iteration order.
- Keep the measured query improvements: direct binary-ID comparison; set projected/evaluated flags once per batch; expand shared bytes once for batches of fixed 16-byte binary IDs.
- Start from fts12's upstream pdqsort implementation. The fts15 abbreviated-sort path and memory extension are absent.

The benchmark, scoring code, baseline data, input SQL, workflow, build configuration and result semantics have no changes relative to fts12.

## Correctness

- Complete Release seekdb build passed. The native production/benchmark snapshot matched 48 expected source files; diagnostic hooks absent.
- **17 native tests passed**, including every valid BMP character against the original dictionary reader, supplementary/malformed/multi-character fallbacks, legacy prime/power-of-two maps, character validation/classification for five collations, pending-match growth beyond inline capacity, mixed dictionary identities, error/reuse, binary decoder NULL/hex/boundary cases and previous fts12 regressions.
- Six full SQL differential runs produced identical 71,442-byte transcripts, including 256 real-dictionary smart/max-word fuzz calls and the existing membership, relevance, ranking, prepared-statement, collation and sort regression suites.
- 65,792-document natural/Boolean ranking and LIMIT/OFFSET fallbacks matched fts12 exactly. Original invalid-input error codes/messages also matched.
- Eight concurrent clients, **4,800 TOKENIZE calls per binary**, produced identical results across clients and between versions.
- All official hit counts matched: CN **8,001**, BENG **11,000**, mixed **7,332**, LIMIT **20**.

## Complete same-host default benchmark

Run order: base1, candidate1, candidate2, base2, base3, candidate3. Each run starts a separate fresh E-backed native-ext4 instance. The unmodified default benchmark runs before additional correctness SQL. No compilation or profiling ran concurrently.

Configuration: rows=20000, batch=500, rounds=3000, query_rounds=200, samples=3, warmup=30, skip_load=0. Baseline binary equals the independently rebuilt fts12 recovery binary (SHA256 below).

All measurements are retained. In particular, **candidate2 had a substantial build stall**, also affecting the BENG index. Build performance remains uncertain; it is not evidence of a universal speedup. Medians reduce the influence of this outlier but do not prove that a future official run will avoid stalls.

| Run | IK all s | IK content s | BENG build s | IK tokenize ms | BENG tokenize ms | CN query ms | BENG query ms | Mixed query ms | LIMIT query ms | SELECT 1 ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| base1 | 6.0800 | 4.8480 | 2.2320 | 0.1612 | 0.1050 | 0.6998 | 1.0164 | 0.7881 | 1.2902 | 0.1046 |
| score1 | 5.8900 | 3.9410 | 2.3370 | 0.1446 | 0.1018 | 0.6274 | 0.9707 | 0.7750 | 1.2766 | 0.1177 |
| score2 | 21.6310 | 9.0090 | 5.4180 | 0.1606 | 0.1212 | 0.6872 | 0.8968 | 0.7248 | 1.1548 | 0.0993 |
| base2 | 6.1800 | 4.0390 | 2.1340 | 0.1672 | 0.1209 | 0.7253 | 1.0649 | 0.8403 | 1.2149 | 0.0930 |
| base3 | 5.6700 | 4.1400 | 2.3380 | 0.1511 | 0.1170 | 0.7151 | 0.9538 | 0.7677 | 1.1491 | 0.0829 |
| score3 | 5.6700 | 3.7380 | 2.2350 | 0.1226 | 0.1189 | 0.6334 | 0.8549 | 0.7308 | 1.0289 | 0.0949 |

| Metric | fts12 median | Candidate median | Reduction (positive is faster) |
|---|---:|---:|---:|
| build_ik_all_sec | 6.0800 | 5.8900 | 3.13% |
| build_ik_content_sec | 4.1400 | 3.9410 | 4.81% |
| build_beng_en_sec | 2.2320 | 2.3370 | -4.70% |
| tokenize_ik_avg_ms | 0.1612 | 0.1446 | 10.30% |
| tokenize_beng_avg_ms | 0.1170 | 0.1189 | -1.62% |
| query_cn_avg_ms | 0.7151 | 0.6334 | 11.42% |
| query_beng_avg_ms | 1.0164 | 0.8968 | 11.77% |
| query_mixed_avg_ms | 0.7881 | 0.7308 | 7.27% |
| query_limit_avg_ms | 1.2149 | 1.1548 | 4.95% |
| select1_avg_ms | 0.0930 | 0.0993 | -6.77% |

Averaging the relative median reductions within build/tokenize/query categories, then across those three categories, gives **4.76% lower time relative to fts12**. This is a local comparison, **not an official score**. Some individual metrics regress slightly. SQL round-trip and build scheduling noise remain material. The checked-in old CI baseline is not used to claim a score.

## Isolated tokenizer calculation

To reduce SQL round-trip noise, an additional ABBA comparison evaluates `SUM(JSON_LENGTH(TOKENIZE(CONCAT(text, MOD(id,7)), parser)))` over 10,000 rows. The text is the corresponding unmodified benchmark text, with variable input to prevent constant folding. Six samples per query are taken; the first warms the code and dictionary, and the median of the remaining five is reported. Outputs match exactly: IK 410000, BENG 160000.

| Run | IK seconds | BENG seconds |
|---|---:|---:|
| base1 | 0.208784 | 0.059160 |
| score1 | 0.170792 | 0.056499 |
| score2 | 0.169971 | 0.057109 |
| base2 | 0.218216 | 0.061791 |

ik: mean of run medians 0.213500s -> 0.170382s, **20.20% lower time**.

beng: mean of run medians 0.060476s -> 0.056804s, **6.07% lower time**.

## Artifacts and identity

Full reports, SQL transcripts and comparison JSON: `E:\ckwjx\Oceanbase-seekdb-work\fts16-paged-validation`.
Native test log: `E:\ckwjx\Oceanbase-seekdb-work\fts16-unit-validation\unit.log`.
Isolated tokenizer samples: `E:\ckwjx\Oceanbase-seekdb-work\fts16-paged-token-cpu\results.json`.
Boundary/error outputs: `E:\ckwjx\Oceanbase-seekdb-work\fts16-extra-results`.
Baseline source worktree remains clean at the original fts12 commit. Existing submitted branches are preserved.

Binary SHA256:

- fts12 batch-topk and independently rebuilt recovery: `d2491fee9f282e3a8cdbc7a062ee8fb5d2965059ae5c192c79a6c38523d5f143`
- fts16 tested executable: `0134afa2d1d4477b80df93eedbae2fe6be5579a1a79453a959879f5fe672b958`
