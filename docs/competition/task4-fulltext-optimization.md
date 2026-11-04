# 赛题四全文索引优化及验证

本次以官方 `2026-competition` 的 `5bce9e06d1281347ad61344e4b01fab9158c58a2` 为比赛源码基线，恢复 build.sh、CMakeLists.txt 和官方全文索引基准。保留报名仓库原有 GitHub 工作流和 README 报名标记。旧提交基于 master，缺少比赛构建入口；平台没有提供可读取的失败明细，因此不能断言旧提测的唯一失败原因。

## 内核修改

- IK 字符分类原来逐项重新解码字符，中文通常解码三次、标点四次；改成解码一次，按原有顺序判断。保持全角字母、数字优先级和错误处理。
- IK 外层已读取字符地址、长度和类别，四个处理器原来又分别读取；现在直接传入已有信息。处理顺序、游标推进和分词规则保持一致。
- 词元特殊字符过滤在遇到第一个非标点且非控制字节后提前结束；保持原字符表、逐字节语义及空词元行为。

未修改官方数据、SQL、计时方法、基线或评分规则。源码差异与上述修改对应，没有共享缓存、输入裁剪或硬编码测试结果。

## 实际验证

2026-10-29，本地 WSL Ubuntu 26、官方 Clang 17 依赖、Release O2 Unity 构建。全量源文件编译成功，使用官方依赖 libaio.so.1 的绝对路径完成链接。此链接处理仅发生在本地生成的构建文件中，以适应 Ubuntu 26；没有修改提交中的官方构建源码。

已启动实际 seekdb 实例并连接成功；实例源码版本为 `938e19f9b8da227e92786167d5c17b4ffb41251f`。本文档及报告的后续提交不修改生产代码。

原生 Google Test 链接实际构建的 liboceanbase.so，以下三项全部通过：

- EveryUnicodeScalarMatchesReference：所有 Unicode 标量的类别与参考实现一致。
- MalformedAndTruncatedInputMatchesReference：非法、截断编码、空指针及不支持字符集。
- FullwidthClassificationPrecedence：全角字母与数字的分类优先级。

另已通过 6,294,657 次隔离分类对比和 262,145 次合成字符表过滤对比。这些组件检查作为补充，不能替代实际 SQL 测试。

完整执行官方 fts_large_bench.sh：ROWS=20000、BATCH=500、ROUNDS=3000、QUERY_ROUNDS=200、SAMPLES=3、WARMUP=30、SKIP_LOAD=0。包含重新导入数据、三个全文索引构建、IK/BENG 分词及四类 MATCH 查询，脚本退出码为零。

四类查询命中数分别为 8001、11000、7332、20，均与固定基线一致。

## 耗时和评分的适用范围

| 指标 | CI 固定基线 | 本地候选 |
| --- | ---: | ---: |
| build_ik_all_sec | 35.2836 | 8.371 |
| build_ik_content_sec | 28.3764 | 6.372 |
| build_beng_en_sec | 14.7578 | 3.057 |
| tokenize_ik_avg_ms | 0.76478 | 0.2398 |
| tokenize_beng_avg_ms | 0.42262 | 0.1605 |
| query_cn_avg_ms | 16.66282 | 2.0364 |
| query_beng_avg_ms | 24.30424 | 2.9001 |
| query_mixed_avg_ms | 17.55928 | 2.2718 |
| query_limit_avg_ms | 16.23338 | 2.6675 |

官方评分脚本接受完整报告，给出本地参考分 100/100，综合下降比例 76.55%。这里比较的是本机与 CI 固定基线，硬件和负载不同，不能将全部差异归因于本次内核修改，也不能保证正式提测得满分。正式成绩必须由统一评测环境计算。

原始报告中的 git_dirty=1 来自构建快照保留官方 GitHub 工作流，而远端候选保留报名仓库工作流，以及 Rust 构建生成的 .gitignore；已核对生产源码、Rust 源码、官方基准和构建入口无额外差异。

原始报告与评分输出见同目录 task4-benchmark-report.txt 和 task4-benchmark-score.txt。尚未执行两台不同版本实例间的 compare_servers.py；不能声称该项已经通过。

## 存储迁移

Ubuntu 虚拟磁盘已迁移至 E:\ckwjx\Oceanbase-seekdb-work\Ubuntu，原 C 盘虚拟磁盘已移除，释放约 24 GB。编译、依赖和测试实例保留在该 E 盘虚拟磁盘中，WSL 交换文件亦位于 E 盘。
