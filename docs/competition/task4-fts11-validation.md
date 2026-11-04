# fts11：上游排序库接入与验证

已知官方成绩是 fts10 的 21.077/50，本轮尚无官方成绩，不声称达到 50 分。

## 实际采用的上游实现

接入 Orson Peters 的 [pdqsort](https://github.com/orlp/pdqsort/tree/b1ef26a55cdb60d236a5cb199c4234c704f46726)，固定提交 `b1ef26a55cdb60d236a5cb199c4234c704f46726`。算法源文件完整保留，仅在开头加入注明的 `#pragma once`。原始 zlib 许可声明保留，完整许可另存于 `third-party/pdqsort-LICENSE.txt`。不是重新编写或冒称原创的排序算法。

接入范围限于 SQL 已物化行的普通内存排序及分组内排序回退路径。沿用原比较器、NULL 处理、升降序、复合键和字符排序规则。编码排序、TopN 堆和外部归并保留原实现。

另外，UTF8MB4_GENERAL_CI 下等长且字节完全相同的字符串直接用 libc `memcmp` 识别相等，避免重复键反复 Unicode 解码；其余字符串走原排序规则。没有改变分词或查询规则。

## 定位和排除的实验

对 fts10 建索引过程原生采样，主要热点包括字符排序比较、比较函数数组访问、排序比较器和编码处理。Google Abseil SwissTable 在 IK 仲裁及字典字符表上的试验没有稳定胜过原路径，已撤回。用较松的 BM25 理论上界省略块统计预扫描虽然结果正确，但 LIMIT 查询实测由 1.7598ms 变为 2.3614ms，已撤回。也阅读了 Apache Lucene 的 MaxScoreBulkScorer 和窗口位图合并实现；没有将 Java 代码误当作可直接替换本项目不同文档 ID 布局的 C++ 实现。

## 正确性及复现

原生 Release 完整编译；12 项单元测试全部通过，包含多种输入分布、字符排序规则、NULL、多列键的行指针排序与 `std::sort` 对照，以及 16384 组任意字节字符串在两种末尾空格模式下与原始字符排序处理器的精确比较。8 个并发客户端、每版 4800 次 TOKENIZE 的完整输出一致。

完整 SQL 差分核对 IK/BENG/其他内置解析器、自定义参数、Unicode、空值、全文分数和排名、多批次一至四路并集、大量查询词，以及完整 ORDER BY、NULL、不同排序规则和分区 ROW_NUMBER。输出逐字节一致，性能脚本四类命中数保持 8001/11000/7332/20。

由于旧实例多次建删索引会产生抖动，最终对照为每版建立一个全新专用实例，先运行原版完整性能脚本，之后运行功能验证，避免功能验证预热字典。两个实例及全部编译文件、日志均保存在 E 盘对应的 Linux 文件系统。ROWS=20000、BATCH=500、ROUNDS=3000、QUERY_ROUNDS=200、SAMPLES=3、WARMUP=30、SKIP_LOAD=0。官方脚本、评分、基线、输入数据均未修改。

| 指标 | 同机 fts10 新实例 | fts11 新实例 |
| --- | ---: | ---: |
| build_ik_all_sec | 5.971 | 5.571 |
| build_ik_content_sec | 4.039 | 3.937 |
| build_beng_en_sec | 2.333 | 2.235 |
| tokenize_ik_avg_ms | 0.1408 | 0.1376 |
| tokenize_beng_avg_ms | 0.1091 | 0.1029 |
| query_cn_avg_ms | 0.6616 | 0.7243 |
| query_beng_avg_ms | 0.9757 | 0.9694 |
| query_mixed_avg_ms | 0.7677 | 0.7784 |
| query_limit_avg_ms | 1.7854 | 1.6744 |

收益有改善也有回退，无法从这一对本地测量证明稳定提分或满分。原始结果位于 `E:\ckwjx\Oceanbase-seekdb-work\fts11-final-clean-results`，其他实验和原生采样在同级 `fts11-*` 目录。需在官方环境复测，fts10 作为已得分版本保留。
