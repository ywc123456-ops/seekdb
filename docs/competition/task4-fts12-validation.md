# fts12：小规模语料的批量 TopK 评分

上一版 fts11 的用户官方反馈为 27.562/50，综合提升率 0.27561755538102667。本版尚无官方成绩，不承诺已达到满分。

## 优化依据及实现

参考 Apache Lucene 的 [MaxScoreBulkScorer 批量评分设计](https://github.com/apache/lucene/blob/f815e679bf6c3dbb1b27c587ed65010d7e1d8a39/lucene/core/src/java/org/apache/lucene/search/MaxScoreBulkScorer.java)，结合 seekdb 的实际 ID 格式和迭代器接口适配。没有把 Java 实现冒称为可以直接替换本项目的 C++ 库。

对一至三个词项、向量化、自然语言模式、无词项权重且不处于函数式按行查询模式、估计语料量不超过 64Ki 篇文档的 TopK 查询，以连续 posting 批次进行有序合并和评分。其余查询保留原 WAND 路径。仍使用原 BM25 批量表达式、相关度收集器、TopK 堆及结果投影，过滤处理与原 BMW 路径一致。相同 ID 的贡献按原三路 simple merger 的词项序号顺序累加，保留浮点计算次序。TopK 只保留需要的文档，不缓存查询结果。

二进制 ID 比较对常见的 16 字节宽度调用编译期定长 memcmp；其他长度、NULL、扩展值及不同类型保留原处理。TOKENIZE 参数使用现有受 TempAllocGuard 保护的表达式临时内存，减少独立 arena 创建；默认 IK 配置直接设置与原 JSON 解析相同的字段，显式非默认配置继续走原验证。

## 验证

- 原生 Linux Release 完整构建，13 项原生测试通过。覆盖默认 IK 配置在新建和复用对象上的等价性，以及既有随机字符排序和分词回归。
- 全部生产源文件与最终编译快照逐字节核对（统一换行符）；临时诊断代码未纳入提交。
- 完整 SQL 对照覆盖分词、自定义参数、Unicode、长输入、嵌套表达式、输出模式、精确分数和排名、OFFSET、LIMIT 0、空结果、得分过滤、预编译参数切换、一至四个词项、跨批次合并、排序和窗口函数。
- 同分且未指定第二排序键的结果允许不同 ID 次序；比较完整分数多重集和匹配有效性。另建 321 篇长度不同的文档，确认 321 个不同分数，并对返回 ID、分数和 OFFSET 结果做精确比较，避免用同分测试掩盖排名错误。
- 65792 篇文档的排名、布尔查询回退与原版一致；三个原有错误场景（长 IK 输入、无效 IK 配置、未知解析器）返回码及报错内容一致。
- 8 个并发客户端，每版 4800 次 TOKENIZE，完整结果一致。

## 同机新实例对照

每版使用独立全新实例，先完整性能测试，再功能测试，避免功能测试提前预热字典。参数保持 ROWS=20000、BATCH=500、ROUNDS=3000、QUERY_ROUNDS=200、SAMPLES=3、WARMUP=30、SKIP_LOAD=0。四类命中数均为 8001、11000、7332、20。未修改评测入口、SQL、数据、基线或评分脚本。

| 指标 | fts11 | fts12 |
| --- | ---: | ---: |
| build_ik_all_sec | 5.769 | 5.867 |
| build_ik_content_sec | 3.938 | 4.035 |
| build_beng_en_sec | 2.132 | 2.236 |
| tokenize_ik_avg_ms | 0.1359 | 0.1372 |
| tokenize_beng_avg_ms | 0.1044 | 0.1050 |
| query_cn_avg_ms | 0.7432 | 0.7337 |
| query_beng_avg_ms | 1.0289 | 0.9601 |
| query_mixed_avg_ms | 0.8217 | 0.7711 |
| query_limit_avg_ms | 1.7808 | 1.1562 |

本次新实例 LIMIT 耗时下降约 35.1%；其他三类查询也有下降，构建及分词没有显示稳定收益。本地 SQL 往返基准约 0.08ms，与用户官方环境约 0.027ms 不同，不能直接换算官方分数。三类等权，仍需进一步改善构建和分词才能冲击综合耗时减半。

原始报告、完整输出、大语料/错误对照、并发检查分别保存在 E 盘 `fts12-final-clean-results`、`fts12-boundary-results`、`fts12-concurrent-results`。本轮只交付仓库 branch 和 commit，由用户自行提测。
