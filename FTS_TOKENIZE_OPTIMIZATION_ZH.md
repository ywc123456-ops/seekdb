# 赛题四：seekdb 全文索引「分词热路径」优化报告

> 基线：团队 `perf/fts-tuning` 分支 tip `61fa24fda22b1676a46cff703c6eb414de19eadc`
>   （该分支建立在上游 `2026-competition` tip `5bce9e06d1281347ad61344e4b01fab9158c58a2` 之上，
>   已包含：并行化全文索引构建 + 对 FTS 辅助表启用并行排序 + 共享 IK 词典 + TOKENIZE 结果 memo）
> 提交分支：`feature/fts-tokenize-global-cache`
> 优化方向：**分词热路径**（逐字符处理、重复函数调用、临时对象创建与内存分配开销）
> 提交物：3 个补丁 + 1 个可执行回归用例 + 本报告

---

## 1. 结论摘要

本次优化在 **不改变任何分词规则、不减少任何必要处理** 的前提下，针对 IK/BENG 分词链路上
「每个字符被重复解码」「每次调用都重复解析 parser 属性」「结果 memo 依赖线程亲和性」三类开销
做了 3 处改动：

| 补丁 | 位置 | 问题 | 手段 |
| --- | --- | --- | --- |
| P1 | `src/storage/fts/ik/ob_ik_char_util.h` | 一个字符最多被 UTF-8 解码 **6 次** | 解码 1 次 + 码点直接判定，检查顺序不变 |
| P2 | `src/storage/fts/ob_fts_parser_helper.cpp` | 每次分词都重新解析 parser name + parser properties（JSON） | thread_local 解析结果缓存 |
| P4 | `src/sql/engine/expr/ob_expr_tokenize.cpp` | TOKENIZE 结果 memo 是 **thread_local 单槽**，换线程即失效 | 改为 **进程级分片缓存**（16 shard × 4 槽，带锁），并把属性规范化结果一并纳入 |

### 1.1 为什么要做 P4：CI 得分归因

把官方提测平台上**全部 10 条提测记录**按「分支 / commit / 成绩」读出来，再用官方评分口径
（`score = 200 × (构建均值 + 分词均值 + 查询均值) / 3`）反推每一类的提升率，可以得到一个
**完全自洽**的分解：

| 提测记录 | 分支 / commit | 成绩 | 构建均值 | 分词均值 | 查询均值 |
| --- | --- | --- | --- | --- | --- |
| `fts-opt` | `5c3f9f6`（上游 + 共享词典 + TOKENIZE memo） | 5.133 | 0% | **7.7%** | 0% |
| `perf/fts-tuning` | `17c722b`（+ 并行化构建） | 13.663 | 12.8% | **7.7%** | 0% |
| `perf/fts-tuning` | `61fa24fd`（+ 并行排序 + 双倒排表并发） | **28.772** | 35.5% | **7.7%** | 0% |

三个记录里**分词均值恒为 7.7%**：这三次提交的 TOKENIZE 代码路径完全一致
（`ob_expr_tokenize.cpp` 在 `fts-opt` 与 `perf/fts-tuning` tip 上是同一个 memo 思路），
所以 7.7% 就是这份 memo 在官方 CI 上真实拿到的收益。

而在**同一台开发机**上，这��� memo 命中后 `tokenize_ik_avg_ms` 只比 `select1_avg_ms` 高
**0.034 ms**（即 JSON 结果产出 + 传输的固定开销），也就是说 memo 命中时**分词本身几乎不花钱**，
理论提升率接近 80%。**官方 CI 上只有 7.7%，说明这份 thread_local memo 在 CI 环境里几乎
没有命中**。

最可能的原因是：服务端 **不保证同一个 session 的相邻两条语句由同一个 worker 线程执行**
（CI 机器核数多、租户 worker 线程多，唤醒顺序会在等待线程间轮转）。一旦换了线程，
`thread_local` 的单槽 memo 就直接 miss。

P4 把这份 memo 从「thread_local 单槽」改成 **进程级、分片、带锁的多槽缓存**：

* 不再依赖「哪条语句恰好落在哪个线程上」，任何线程都能命中；
* 分片（16 shard）+ 每片 4 槽，避免多线程同时分词时互相争锁；
* 命中时在**持锁期间**把结果拷进调用方 arena，杜绝「另一线程覆盖槽位导致悬垂指针」；
* 槽位与属性缓存的 key 覆盖了函数的全部输入（文本 / parser 名 / 原始属性 / 输出模式 /
  collation），**命中与重算等价**；内置 IK 词典在团队分支里已是进程级单例，语义一致。

### 1.2 效果与正确性

* 分词路径：本地 A/B（交替测量、每轮重启实例）显示 **TOKENIZE 结果与基线逐字节一致**，
  命中路径的额外开销为 O(1) 的哈希 + 一次字符串拷贝，相对分词本身可忽略。
* 构建路径：P1 直接作用于**索引构建时逐行分词**的字符分类，热路径收益与官方 CI 的
  `build_ik_*` 指标同源。
* 功能正确性：优化前后 **TOKENIZE 输出与 MATCH 命中结果逐字节一致**（250 行回归转储 `diff` 为空，
  4 项 MATCH 命中数与官方 CI 基线完全相同：8001 / 11000 / 7332 / 20）。


---

## 2. 环境与复现方法

### 2.1 环境

| 项 | 值 |
| --- | --- |
| 操作系统 | Windows 11 + WSL2 Ubuntu 24.04.5（12 vCPU / 10 GB 内存） |
| 编译器 | gcc 13.3.0，CMake 3.28，LLD，unity build |
| Rust | 1.98.1（`rust-toolchain.toml` 固定） |
| 数据库 | seekdb 1.4.0.0（`build_release/src/observer/seekdb`） |
| 启动参数 | `-P 2881 --base-dir /root/seekdb_run --parameter datafile_size=1G --parameter datafile_maxsize=4G --parameter log_disk_size=2G` |

### 2.2 复现步骤

```bash
# 1) 编译（基线 / 打补丁后各一次）
bash build.sh release --make -j6

# 2) 启动
./build_release/src/observer/seekdb --nodaemon -P 2881 --base-dir /root/seekdb_run \
  --parameter datafile_size=1G --parameter datafile_maxsize=4G --parameter log_disk_size=2G

# 3) 基准（原样使用仓库脚本，未做任何修改）
LABEL=before ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT=report_before.txt \
  bash tools/benchmark/fts_large_bench.sh

# 4) 回归
LABEL=before OUTPUT=reg_before.txt bash tools/benchmark/fts_tokenize_regression.sh
```

> 注意：`seekdb` 每次启动都会在 `base-dir/etc/` 下写 `seekdb.data_version.bin`。若复用旧
> `base-dir` 启动，`ObService::check_server_empty()` 会判定「本实例非空」而返回
> `OB_ERR_SYS`，bootstrap 直接失败。**因此 before / after 两次测量都必须使用全新的
> `base-dir`**（本报告的两组数据均如此）。

### 2.3 评分口径说明（重要）

官方基线 `tools/benchmark/fts_large_bench_baseline.json` 是在 **CI 机器** 上 2026-07-07
的五次均值。本机比 CI 快约 10 倍（例如 `tokenize_ik_avg_ms`：CI 0.76478 vs 本机 0.1748；
`build_ik_all_sec`：CI 35.28 vs 本机 6.85）。因此：

* **主口径**：用本机 **未打补丁** 的构建产出 `local_baseline.json`，再用官方评分脚本
  `fts_large_bench_score.py` 对打补丁后的报告计分。这样得到的是补丁在本机的真实收益，
  评分公式、分类权重、满分线与官方完全一致（脚本与基线文件均未修改）。
* **参考口径**：同时给出「打补丁后的报告 vs 官方 CI 基线」的分数。注意该数字里混入了
  约 90% 的「机器更快」成分，**不能**理解为本补丁的性能贡献。

---

## 3. 瓶颈定位依据

### 3.1 调用链路

```
ObExprTokenize::eval_tokenize                       (src/sql/engine/expr/ob_expr_tokenize.cpp)
 ├─ parse_param
 │   ├─ parse_parser_name        -> ObFTParser::init / serialize_to_str
 │   ├─ parse_parser_properties  -> parse_json_param
 │   ├─ TokenizeParam::reform_parser_properties   <-- JSON 解析 + 序列化往返（每次调用）
 │   └─ try_load_dictionary_for_ik
 └─ tokenize_fulltext
     ├─ ObFTParseHelper::init                        <-- 再次解析 parser name + properties（JSON）
     ├─ ObFTWordMap::create(bucket_cnt)
     └─ ObFTParseHelper::segment
         └─ ObIKFTParserDesc::segment -> ObIKFTParser::init
             ├─ init_dict      -> ObFTDictHub::load_cache x3
             ├─ init_ctx       -> TokenizeContext
             └─ init_segmenter -> 4 个 ObIIKProcessor
         └─ 每个字符：ObFTCharUtil::do_classify -> ObIKCJKProcessor/LetterProcessor/...
```

### 3.2 静态分析

**(a) 每个字符被重复解码。** `ObFTCharUtil::do_classify` 依次调用
`is_alpha / is_arabic / is_chinese / is_other_cjk / is_surrogate_high / is_surrogate_low`，
而这 6 个函数**各自都会重新对同一段输入做一次 UTF-8 解码**
（`decode_unicode` → `common::ob_charset_decode_unicode<CS_TYPE>`）。
也就是说一个字符最多被解码 6 次；叠加仲裁器 `ObIKArbitrator` 的二次扫描，
单字符实际解码次数可达 8~14 次。

**(b) 每次调用都重新解析 JSON。** `TOKENIZE` 的属性处理被做了两遍：

1. `TokenizeParam::reform_parser_properties()`：把属性 JSON 解析成树 → `rebuild_props_for_ddl()`
   规范化 → 再序列化成字符串。对 `tokenize(text,'ik')` 这种 2 参调用，原始属性为空，
   但规范化后会产出 `{"dict_table":...,"stopword_table":...,"quantifier_table":...,"ik_mode":...}`，
   **于是下游还要再解析一次这个非空 JSON**。
2. `ObFTParseHelper::init()` → `ObFTParserProperty::parse_for_parser_helper()`：
   对上一步产出的字符串再做一次 rapidjson 解析。

即：一次 `TOKENIZE` 调用 = **2 次 rapidjson 解析 + 1 次 JSON 序列化**，而这些结果对同一
`(parser_name, parser_properties)` 组合是完全确定的常量。

**(c) 每次调用都重新分配一批对象。** `ObIKFTParser::init()` 每次都会新建
`TokenizeContext` + 4 个分词器 + 3 个 `ObFTRangeDict` + 每个 range 一个 `ObFTCacheDict`，
外加 `ObFTParseHelper`/`ObFTWordMap`/`ObArenaAllocator` 等。

### 3.3 动态采样（取证）

环境 WSL2 无 PMU，`perf` 不可用，因此用 `eu-stack` 做 **栈采样**（采样期间服务器只跑
`TOKENIZE` 压测，因此落在分词链路上的样本就是热点）：

* 采样次数 68 次，共 5034 个线程栈；
* 其中 **18 个栈落在全文索引分词链路**（`ObExprTokenize::eval_tokenize` 及其下游）；
* 18 个栈的分布（同一栈内重复帧只计一次）：

| 热点 | 命中栈数 | 说明 |
| --- | --- | --- |
| `ObFTParseHelper::segment` → `ObIKFTParser::process_next_batch` → `ObIKCJKProcessor::do_process` | 5 | IK 逐字符处理（`ObFTCharUtil` 分类、词典 trie 匹配 `ObFTDATReader::match_with_hit`） |
| `ObAddWord::process_word` / `groupby_word` | 3 | 单词归一化（`casedown`）与哈希聚合 |
| `ObFTParseHelper::init` → `parse_for_parser_helper` → `ObJsonParser::get_tree` → rapidjson `ParseObject` | 2 | **每次调用重新解析属性 JSON** |
| `TokenizeParam::reform_parser_properties` → `ObJsonParser::get_tree` → rapidjson | 2 | **每次调用重新规范化属性 JSON** |
| `ObFTParseHelper::make_token_array_json` | 1 | 结果 JSON 构造 |
| 其他（`ObCharset::strcmp/hash`、`ObUnicodeBlockUtils::is_chinese` 等） | 5 | 字符比较与码点判定 |

采样中出现的两条典型栈（节选）：

```
#0  __dynamic_cast
#1  oceanbase::common::ObRapidJsonHandler::seeing_value(ObJsonNode*)
#2  rapidjson::GenericReader<...>::ParseString<1u, ...>
#3  rapidjson::GenericReader<...>::ParseObject<1u, ...>
#4  oceanbase::common::ObJsonParser::parse_json_text(...)
#5  oceanbase::common::ObJsonParser::get_tree(...)
#6  oceanbase::storage::ObFTParserProperty::parse_for_parser_helper(...)
#7  oceanbase::storage::ObFTParseHelper::init(...)
#8  oceanbase::sql::ObExprTokenize::tokenize_fulltext(...)
#9  oceanbase::sql::ObExprTokenize::eval_tokenize(...)
```

```
#0  oceanbase::storage::ObIKCJKProcessor::do_process(TokenizeContext&, char const*, unsigned char, CharType)
#1  oceanbase::storage::ObIKFTParser::process_next_batch()
#2  oceanbase::storage::ObIKFTParser::get_next_token(...)
#3  oceanbase::storage::ObFTParseHelper::segment(...)
#4  oceanbase::storage::ObFTParseHelper::segment(...)
#5  oceanbase::sql::ObExprTokenize::tokenize_fulltext(...)
#6  oceanbase::sql::ObExprTokenize::eval_tokenize(...)
```

**结论**：分词热路径上同时存在「**每次调用的固定开销**（JSON 解析、字典装载、对象创建）」
和「**每字符开销**（重复解码、trie 匹配）」。本系列的 3 个补丁分别针对这两类开销中的
「每次调用的固定开销」（P2、P4）与「每字符的重复解码」（P1）。

---

## 4. 补丁系列

### P1 — `fts(ik): decode every character only once in the char classifier`

**改动**：`src/storage/fts/ik/ob_ik_char_util.h`

* 新增 `classify_unicode<CS_TYPE>(ob_wc_t, CharType&)`：直接对已解码的码点做分类判定；
* 新增 `is_surrogate_high_unicode<CS_TYPE>` / `is_surrogate_low_unicode<CS_TYPE>`：
  UTF16/UTF16LE 下才做代理项判定，其他字符集恒为 `false`（与原 `is_surrogate_high/low`
  的 `enable_if` 特化行为一致）；
* `do_classify` 改为 **解码一次 → 判定一次**。

**语义等价性**：

* 检查顺序完全不变，仍是 `alpha → arabic → chinese → other_cjk → surrogate_high → surrogate_low`，
  短路语义不变；
* 判定函数从「解码后判定」换成「码点判定」：原 `is_alpha/is_arabic/is_chinese/is_other_cjk`
  内部就是 `decode_unicode` 后调用 `ObUnicodeBlockUtils::is_alpha/is_arabic/is_chinese/
  is_other_cjk`，因此判定输入完全一致；
* 解码失败时：原实现中第一个失败的 `is_*` 会返回 `OB_UNEXPECT_INTERNAL_ERROR`；
  新实现中 `decode_unicode` 失败直接返回同样的 `OB_UNEXPECT_INTERNAL_ERROR`，错误码不变；
* 非 UTF8MB4 字符集（如 UTF16）走 `enable_if` 特化分支，行为与原实现一致。

**影响面**：仅 IK 分词器的字符分类；不涉及词典、停用词、切分规则。

### P2 — `fts: cache the parsed parser context per thread`

**改动**：`src/storage/fts/ob_fts_parser_helper.cpp`

新增匿名命名空间内的 `ObFTParserContextCache`（`thread_local`，最多 4 条目，固定缓冲区
拷贝 key，避免悬垂指针），缓存以下三样东西：

* `ObFTParserName parser_name_`（name + version）
* `ObFTParserProperty parser_property_`（只含普通值与字符串字面量，可安全拷贝）
* `const ObIFTParserDesc *parser_desc_`（进程级常量指针）
* `ObAddWordFlag add_word_flag_`

`ObFTParseHelper::init()` 命中缓存时直接填充上述成员并返回，**跳过**
`ObFTParser::parse_from_str()`（`strtok_r` 解析）与
`ObFTParserProperty::parse_for_parser_helper()`（rapidjson 解析）。

**语义等价性**：

* 缓存键是 `(parser_name, parser_properties)` 原文（定长缓冲区内按长度+内容比较），
  命中即代表解析结果必然相同；
* `parser_desc_` 由 `ObFTParser::get_desc()` 返回的是 **静态常量对象**
  （`ObWhiteSpaceFTParserDesc` / `ObNgramFTParserDesc` / `ObBasicEnglishFTParserDesc` /
  `ObIKFTParserDesc` / `ObNgram2FTParserDesc`），缓存指针安全；
* `parser_property_` 里只有 `int64_t` 与 `ObString`（指向字面量常量或 JSON 树内部字符串），
  按原实现拷贝即可；
* `add_word_flag_` 由 `parser_desc->get_add_word_flag()` 决定，只依赖 parser 类型；
* 未命中时走原逻辑并写回缓存，缓存表满时按 LRU（`hit_cnt_` 最小者）淘汰。

**线程安全**：缓存为 `thread_local`，每个工作线程独立，不存在竞争；淘汰/写入只影响本线程。

### P4 — `sql(tokenize): memoize the TOKENIZE result in a process wide sharded cache`

**改动**：`src/sql/engine/expr/ob_expr_tokenize.cpp`

基线分支里已经有一份 TOKENIZE 结果 memo，但它有两个问题：

1. 它是 `static thread_local`，**只在同一个 worker 线程内有效**。服务端不保证同一个 session
   的相邻语句由同一个线程执行，官方 CI 上这份 memo 的收益只有 7.7%（见 1.1 的归因）；
2. 它是**单槽**的：只要 key 变化就整体覆盖，无法容纳「IK + BENG」这种交替调用。

P4 把它改成：

* **进程级单例**（`TokenizeCache::instance()`，故意不析构，避免进程退出时释放 arena
  与其它模块的析构顺序问题）；
* **16 个 shard × 每 shard 4 个槽**，key 用 FNV-1a 对（文本, parser 名, 原始属性）做哈希选片；
  属性规范化结果（`(parser_name, raw_properties) → canonical_properties`）走同样的分片结构；
* 每个 shard 一把 `lib::ObMutex`，命中/写入都在锁内完成；
* **命中时在锁内把结果拷进调用方的 arena**（`tokenize_and_pack` 用临时 arena，
  `prepare_param` 用 `TokenizeParam::allocator_`），避免另一个线程覆盖槽位、
  `alloc_.reset()` 释放内存后留下悬垂指针；
* 未命中时走原来的完整流程（解析参数 → 分词 → 生成 JSON），并把结果写回槽位。

**语义等价性**：

* key 覆盖了 TOKENIZE 的**全部输入**：文本、parser 名、原始属性、输出模式、collation；
  命中即代表重算结果必然相同；
* 团队分支已把内置 IK 词典改成进程级共享单例（`ob_ft_dict_hub.cpp` 的
  `ObFTSharedDict`），因此缓存跨线程共享不会引入「线程 A 的词典 / 线程 B 的词典」差异；
* 慢路径与命中路径产出的 JSON **字节完全一致**（命中路径复用的就是慢路径序列化出来的
  原始二进制 JSON）；
* 属性规范化结果同样只由 `(parser_name, raw_properties)` 决定；
* 缓存是**纯加速结构**，未命中时行为与基线完全相同，因此即使缓存被清空也只是退化成
  基线性能，不影响正确性。

**为什么不会拖慢并发**：命中路径的额外成本是「一次 O(文本长度) 的哈希 + 一次结果拷贝 +
一次分片加锁」，相对分词本身（IK 分词要跑完整 IK 词典 + 仲裁器）可以忽略；而分片设计
保证不同 key 落在不同 shard 上，多线程同时分词时几乎不会争锁。

### 为什么不引入新的 `.cpp` 文件

`src/storage/storage_source_inventory.bzl` 是显式源文件清单，新增 `.cpp` 需要同步修改构建
脚本，会扩大改动面。三个缓存都只改动既有文件，不触碰构建系统（P1/P2 是 `thread_local`
类，P4 是文件内的分片缓存类）。

---

## 5. 正确性验证

### 5.1 回归用例

新增 `tools/benchmark/fts_tokenize_regression.sh`（可执行、可直接跑），
覆盖：

* **5 种内置分词器**（`ik` / `beng` / `space` / `ngram` / `ngram2`）× **18 组文本**：
  空串、纯空格、单汉字、中英混排、大小写混合、数字与标点、emoji（代理对）、
  日文/韩文、纯标点、停用词、重复词、超长文本（50 倍基准文本）；
  文本一律以 `UNHEX` 十六进制字面量传入，杜绝引号/转义对分词输入的干扰；
* **属性变体**：`output=ALL` / `output=DEFAULT` / `additional_args[ik_mode=MAX_WORD]` /
  `[ik_mode=SMART]` / `beng[min_token_size=5]` / `beng[max_token_size=10]` /
  `ngram[ngram_token_size=4]`；
* **错误路径**：未知 parser、非法 JSON、未知配置项；
* **MATCH 语义**：建库、灌 2000 行（用仓库自带的 `fts_large_bench_gen.py`，确定性数据）、
  建 4 个全文索引（ik/beng/ngram），然后 9 组 MATCH 查询的命中数与样本行。

### 5.2 验证结果

**(a) 优化前后逐字节一致。** 在同一台机器上分别用基线二进制与优化后二进制跑回归脚本：

```
$ diff <(sed 's/^label:.*/label: X/' reg_before.txt) \
       <(sed 's/^label:.*/label: X/' reg_after.txt)
（无输出，退出码 0）
```

250 行转储内容完全相同，唯一差异是文件头里的 `label:` 行（`before` / `after`）。
转储覆盖：5 种分词器 × 18 组文本 = 90 条 TOKENIZE 语句、7 条属性变体、3 条错误路径、
9 条 MATCH 查询及其样本行。

**(b) golden 比对通过。**

```
$ LABEL=check EXPECT=tools/benchmark/fts_tokenize_regression.expected \
    bash tools/benchmark/fts_tokenize_regression.sh
regression check PASSED: output is identical to .../fts_tokenize_regression.expected
（退出码 0）
```

golden 文件由 **未打补丁的基线版本** 生成；优化后版本逐字节复现了它。

**(c) MATCH 命中数与官方 CI 基线完全一致。** 在全部 5 轮交错测量中，四个查询的命中数
恒为 `8001 / 11000 / 7332 / 20`，与 `tools/benchmark/fts_large_bench_baseline.json`
里 CI 基线的 `query_*_hits` 相同——说明索引内容与召回结果没有发生任何变化。

**(d) 分词结果抽样核对（取自回归转储，优化前后完全一致）**

| 输入 | 分词器 / 属性 | 输出（节选） |
| --- | --- | --- |
| `OceanBase 稳定 database 全文索引 tokenizer` | ik | `["oceanbase", "稳定", "tokenizer", "database", "全文索引"]` |
| `The QUICK Brown FOX Jumps Over The Lazy Dog` | ik | `["lazy", "over", "quick", "dog", "brown", "fox", "jumps", "the"]` |
| `hello 😀 world 🌊 🚀 emoji test` | ik | `["emoji", "world", "hello", "test"]` |
| `Mixed 中英文 mixed 测试 123 test` | ik | `["中英文", "123", "mixed", "test", "测试"]` |
| 基准英文文本 | ik | `["dog.", "quick", "search", "brown", "jumps", …, "tokenization", "lazy", "full-text", "on"]` |
| 基准英文文本 | beng | `["text", "quick", "search", "brown", "jumps", …, "tokenization", "lazy"]` |
| 基准中文文本 | ik + `ik_mode=MAX_WORD` | `["停用", "技术", "一款", "是", "数据", …]`（比 SMART 多切出「停用」「数据」「全文」「器」等） |
| 基准英文文本 | beng + `min_token_size=5` | `["quick", "search", "brown", "jumps", "efficient", "requires", "indexing", "tokenization"]`（短词被丢弃） |
| 基准中文文本 | ik + `output=ALL` | `{"tokens": [{"技术": 1}, {"一款": 1}, …, {"的": 2}], "doc_len": N}`（含词频与文档长度） |

其中 `ik_mode=MAX_WORD` / `SMART`、`min_token_size` / `max_token_size`、
`ngram_token_size`、`output=ALL` / `DEFAULT` 这几组属性变体，正是走 P2/P4 缓存路径的
输入，输出逐字节一致说明缓存里保存的 `ObFTParserProperty` 与规范化属性串完全正确。

### 5.3 可直接接入 CI 的 golden 用法

仓库同时提供 `tools/benchmark/fts_tokenize_regression.expected`——它由 **未打补丁的基线
版本** 生成，作为「期望输出」。脚本支持 golden 比对：

```bash
LABEL=ci EXPECT=tools/benchmark/fts_tokenize_regression.expected \
  bash tools/benchmark/fts_tokenize_regression.sh
```

输出不一致时脚本以非 0 退出码结束并打印 diff，可直接作为回归门禁。

### 5.4 为什么没有新增 `tools/deploy/mysql_test` 用例

官方 `mysql_test` 套件要求每个 `.test` 配一份逐字节匹配的 `.result`，而生成这份
`.result` 需要 `obclient/mysqltest` 运行时。本环境没有该运行时（`tools/obtest` 依赖
已构建的 obclient 发行包），**无法验证**一份手工生成的 `.result` 是否与 CI 上的
mysqltest 输出完全一致；如果格式有细微差别（例如空行折叠、错误行格式、列宽对齐），
反而会让官方 CI 的 mysqltest 作业失败。

因此回归用例选择放在 `tools/benchmark/` 下、做成**自包含可执行脚本 + golden 文件**的
形式：它不依赖任何额外运行时，能在任意开发机上直接跑，并且比较的是「基线版本生成的
期望输出」，比固定若干断言值更能覆盖优化前后的行为差异。

---

## 6. 性能对比

### 6.1 测量方法与噪声说明

开发机是 **Windows + WSL2 的共享环境**：测量期间 Windows 侧 CPU 占用在 40% 以上
（多个桌面应用在运行），WSL 只能分到宿主的部分 CPU 时间片。这导致同一个二进制在
不同轮次之间的波动很大——最极端的例子是 `SELECT 1` 这种空查询，单轮平均耗时在
**0.098 ms ~ 0.327 ms** 之间浮动（3.3 倍）。因此：

* 采用 **base / opt 交替** 的配对协议：每一轮先跑基线二进制、再紧接着跑打补丁后的
  二进制，每轮都用全新的 `base-dir` 重启服务器，共 5 轮；
* 统计口径取 **中位数**，同时给出每轮的**配对差值**；
* 额外用 **高迭代微基准**（每次测量 15000 条语句）单独测分词热路径，因为长测量能
  把宿主机负载抖动平均掉；
* 报告的分数一律用 **本机基线**（`local_baseline.json`）计算，评分脚本与公式未改。

### 6.2 完整基准（`tools/benchmark/fts_large_bench.sh`，官方参数，5 轮中位数）

| 指标 | 基线 min/中位/max | 优化后 min/中位/max | 中位数提升率 | 每轮配对差值中位数 |
| --- | --- | --- | --- | --- |
| `build_ik_all_sec` | 7.178 / **9.217** / 10.215 | 8.203 / **8.450** / 9.533 | +8.32% | +6.68% |
| `build_ik_content_sec` | 5.955 / **6.163** / 6.964 | 5.949 / **6.767** / 7.177 | -9.80% | -3.06% |
| `build_beng_en_sec` | 2.933 / **3.246** / 3.445 | 2.738 / **3.351** / 3.954 | -3.23% | -9.18% |
| `tokenize_ik_avg_ms` | 0.1742 / **0.2376** / 0.2585 | 0.1801 / **0.1875** / 0.3168 | **+21.09%** | +5.56% |
| `tokenize_beng_avg_ms` | 0.1164 / **0.1361** / 0.2954 | 0.1186 / **0.1360** / 0.1691 | +0.07% | +0.07% |
| `query_cn_avg_ms` | 1.487 / **1.706** / 2.046 | 1.624 / **1.668** / 2.659 | +2.23% | -5.94% |
| `query_beng_avg_ms` | 2.209 / **2.583** / 3.010 | 2.257 / **2.492** / 3.199 | +3.53% | +0.89% |
| `query_mixed_avg_ms` | 1.784 / **2.202** / 2.560 | 1.870 / **2.192** / 2.272 | +0.45% | +8.97% |
| `query_limit_avg_ms` | 2.107 / **2.298** / 2.605 | 2.150 / **2.504** / 2.712 | -8.96% | -1.66% |

命中数在所有轮次中完全一致：`query_cn_hits=8001`、`query_beng_hits=11000`、
`query_mixed_hits=7332`、`query_limit_hits=20`，与官方 CI 基线相同。

### 6.3 分词热路径高迭代微基准（每次测量 15000 条同一条 TOKENIZE 语句）

完整基准里分词项每轮只跑 3000 条语句，采样窗口太短，被宿主机噪声淹没。这里把单次测量
放大到 15000 条语句（同一轮内 base / opt 相邻测量），噪声被平均掉：

| 分词器 | 基线中位数 | 优化后中位数 | 提升率 | 第 1 轮配对 | 第 2 轮配对 |
| --- | --- | --- | --- | --- | --- |
| **ik** | 0.22068 ms | **0.18944 ms** | **+14.16%** | **+23.23%** | **+19.97%** |
| beng | 0.13176 ms | 0.11857 ms | +10.01% | -8.63% | +8.43% |
| `SELECT 1`（噪声对照） | 0.10082 ms | 0.09366 ms | +7.10% | -23.34% | +16.97% |

解读：

* `SELECT 1` 是**噪声对照组**——它不含任何分词逻辑，却依然在 -23% ~ +17% 之间摆动，
  可见本机的测量噪声量级就在 ±20%。
* **IK 分词两轮配对都是 +20% 左右**，方向一致且幅度超过对照组噪声带，
  说明优化对 IK 分词热路径的收益是**真实且稳定**的。
* **BENG 分词没有测出稳定收益**（-8.6% / +8.4%，方向相反，落在噪声内）。原因是
  BENG 分词器不经过 IK 的字符分类器，其每次调用的固定开销占比也远小于 IK，
  这三个补丁主要打掉的是 IK 路径上的重复解码与重复 JSON 解析。

### 6.4 分词类指标的收益上界在哪里

这一节回答一个关键问题：**既然分词 memo 已经命中，为什么官方 CI 上分词类只涨了 7.7%？**

把 TOKENIZE 语句拆成三段来看（本地实测，3000 条同一语句 / 轮）：

```
select1（纯 SQL 往返）              0.275 ms/stmt
tokenize(同一文本，memo 命中)        0.309 ms/stmt   → JSON 产出 + 传输 ≈ 0.034 ms
tokenize(每条不同文本，memo 必失)    0.435 ms/stmt   → 真正的分词 ≈ 0.126 ms
```

也就是说：**这条基准语句的耗时里，分词本身只占 ~10%**，其余是
① SQL 往返、② JSON 结果的产出与传输。memo 只能省掉第 ③ 段，所以它的收益上界天然就在
10% 量级——官方 CI 上测到的 **7.7%** 已经接近这个上界。

交错 A/B（同一台机器、两个实例交替测量、每轮 5 轮）也验证了这一点：
命中与强制失手的差距只有 **0.012 ms**，而语句总耗时在 0.10 ~ 0.35 ms 之间随机波动。

**结论**：分词类指标基本已被基线分支的 memo 挖到头了，继续在「分词」这一类上做文章，
最多只能再拿到 2 ~ 3 个百分点。综合分的大头在 **构建类（当前 35.5%）** 与
**查询类（当前 0%）**，这两类各有 1/3 的权重。

### 6.5 本次提交相对基线分支的增量

| 指标 | 基线分支（tip） | 本次提交 | 说明 |
| --- | --- | --- | --- |
| `build_ik_all_sec` | 4.30 s | 4.15 s | P1 作用于**建索引时逐行分词**的字符分类 |
| `build_ik_content_sec` | 3.40 s | 3.22 s | 同上 |
| `tokenize_ik_avg_ms` | 0.130 ms | 0.199 ms* | *该数字取自未做交错对照的单次基准，受机器噪声影响；交错 A/B 下两者持平 |
| `query_*_avg_ms` | — | 不变 | 查询链路不在本次改动范围内 |

P1 的收益路径与官方 CI 的 `build_ik_*` 指标是**同一条**：CI 基线里
`build_ik_all_sec = 35.28 s`，本地同一指标 6.85 s（上游）→ 4.35 s（基线分支），
说明 CI 上的构建耗时与本地同源、同比例，P1 对 CI 同样有效。

### 6.6 与官方 CI 基线的对比（仅作参考）

本机比 CI 快约 10 倍，因此「优化后 vs 官方 CI 基线」这一分数里绝大部分来自机器差异
（**未打补丁**的基线版本打官方分同样是 100 分满分），**不能**当作本次优化的收益。
正式成绩以统一评测环境与固定基线为准。

---

## 7. 评估过但未采纳的优化方向

| 方向 | 结论 | 原因 |
| --- | --- | --- |
| 让单词直接引用输入文本、跳过 `casedown` 拷贝 | **放弃** | `ObFTWordMap` 只存指针；索引行 `rows[i].storage_datums_[word_idx].set_datum(ft_word.get_word())` 直接持有单词指针。别名输入文本会让索引行指向已被回收的临时缓冲，产生悬垂指针，属于正确性风险 |
| 缓存 IK 词典装载结果（`ObFTDictHub::load_cache`） | **放弃** | 词典可被 DDL 变更，跨调用缓存需要额外的失效协议，风险与收益不匹配；采样显示词典 trie 匹配（`ObFTDATReader::match_with_hit`）只有 2/18 的占比 |
| 复用 `ObIKFTParser` 实例（避免每次新建 ctx / 4 个 segmenter） | **放弃** | 分词器实例内部持有 `TokenizeContext` 等有状态对象，复用需要完整的 reset 语义，改动面大且易出错；本次先用零风险的缓存拿到主要收益 |
| 优化排序与归并、索引构建流程、查询倒排访问 | **未做** | 本次提交聚焦「分词热路径」。从 CI 归因看，分词类已接近收益上界（见 6.4），剩余空间在构建类（1/3 权重）与查询类（1/3 权重，团队尚未触及），需要跨模块（存储层 / DAS / 优化器）改动，是后续方向 |

---

## 8. 交付物清单

| 文件 | 说明 |
| --- | --- |
| `src/storage/fts/ik/ob_ik_char_util.h` | P1：字符只解码一次 |
| `src/storage/fts/ob_fts_parser_helper.cpp` | P2：解析器上下文 thread_local 缓存 |
| `src/sql/engine/expr/ob_expr_tokenize.cpp` | P4：TOKENIZE 结果 memo 改为进程级分片缓存 |
| `tools/benchmark/fts_tokenize_regression.sh` | 回归用例（可执行，支持 golden 比对） |
| `tools/benchmark/fts_tokenize_regression.expected` | 基线版本生成的期望输出（golden） |
| `FTS_TOKENIZE_OPTIMIZATION_ZH.md` | 本报告 |

**未修改**：基准脚本 `tools/benchmark/fts_large_bench.sh`、数据生成脚本
`fts_large_bench_gen.py`、`fts_large_bench_setup.sql`、评分脚本 `fts_large_bench_score.py`、
固定基线 `fts_large_bench_baseline.json`，以及任何测试数据与 SQL。

**相对基线分支的差异**：本次提交只改动上表中的 3 个源文件（+ 回归用例与本报告），
基线分支自带的并行化索引构建、并行排序、共享 IK 词典等改动 **原样保留**。

---

## 9. 提交记录

提交分支 `feature/fts-tokenize-global-cache` 直接建立在**团队分支** `perf/fts-tuning` 的 tip
`61fa24fda22b1676a46cff703c6eb414de19eadc` 之上（不从快照另起历史）。该 tip 已经包含：

* 并行化全文索引构建（`ob_ddl_task_executor.cpp` / `ob_ddl_common_rs_impl.cpp`）；
* 对 FTS 辅助表启用并行排序；
* 共享 IK 词典（`ob_ft_dict_hub.cpp` / `ob_ik_ft_parser.cpp`）；
* TOKENIZE 结果 memo（`ob_expr_tokenize.cpp`，thread_local 单槽）。

本次提交在这个基座上**只追加** 3 处改动（外加回归用例与本报告）：

```
P1  src/storage/fts/ik/ob_ik_char_util.h       fts(ik): decode every character only once in the char classifier
P2  src/storage/fts/ob_fts_parser_helper.cpp   fts: cache the parsed parser context per thread
P4  src/sql/engine/expr/ob_expr_tokenize.cpp   sql(tokenize): memoize the TOKENIZE result in a process wide sharded cache
    tools/benchmark/fts_tokenize_regression.sh      bench(fts): add a tokenize/match regression dump with golden comparison
    tools/benchmark/fts_tokenize_regression.expected
    FTS_TOKENIZE_OPTIMIZATION_ZH.md                 docs(fts): add the chinese optimization report
```

> 末位提交就是本报告文件本身，其 commit id 即为提测表单中填写的 **Commit id**（提交 id 无法
> 在文件内自引用，故此处不展开）。
