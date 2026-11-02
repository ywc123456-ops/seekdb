#!/usr/bin/env bash
# 全文索引分词/查询回归对比脚本
#
# 该脚本对一组固定的 TOKENIZE 与 MATCH 语句执行并把结果转储到文件。
# 在优化前后的两个版本上分别运行该脚本，然后 diff 两个转储文件，即可证明
# 优化没有改变分词结果、词典处理逻辑与 MATCH 检索语义。
#
# 用法:
#   LABEL=before OUTPUT=/tmp/fts_regression_before.txt \
#     bash tools/benchmark/fts_tokenize_regression.sh
#   LABEL=after OUTPUT=/tmp/fts_regression_after.txt \
#     bash tools/benchmark/fts_tokenize_regression.sh
#   diff /tmp/fts_regression_before.txt /tmp/fts_regression_after.txt
#
# 环境变量:
#   MYSQL      mysql 客户端命令
#   LABEL      写入报告头的标签
#   OUTPUT     转储输出文件（同时总是打印到 stdout）
#   ROWS       MATCH 检查使用的文档数（默认 2000）
#   EXPECT     golden 文件。设置后脚本会把本次转储与 golden 文件逐字节比较，
#              不一致时以非 0 退出码结束，可直接当作 CI 回归用例使用。
#              golden 文件由未打补丁的基线版本生成，见
#              tools/benchmark/fts_tokenize_regression.expected

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MYSQL="${MYSQL:-mysql -h127.0.0.1 -P2881 -uroot -N -s --default-character-set=utf8mb4}"
LABEL="${LABEL:-unknown}"
OUTPUT="${OUTPUT:-}"
ROWS="${ROWS:-2000}"
DB=fts_large_bench
TMP_OUT="$(mktemp "${TMPDIR:-/tmp}/fts_regression.XXXXXX")"

cleanup() { rm -f "${TMP_OUT}"; }
trap cleanup EXIT

if ! $MYSQL -e "SELECT 1;" >/dev/null 2>&1; then
  echo "ERROR: cannot connect to seekdb on 127.0.0.1:2881" >&2
  exit 1
fi

# 覆盖分词器各类分支的文本：中日韩、拉丁字母、数字与标点、emoji（代理对）、
# 停用词、重复词、空串以及超长输入。
IK_TEXT="OceanBase是一款非常稳定的数据库，全文索引的分词器热路径优化包括解析器实例复用、停用词全局单例、内存池化数据结构等技术，在大量文档索引和查询场景下可以显著降低 CPU 开销。"
BENG_TEXT="The quick brown fox jumps over the lazy dog. Full-text search indexing requires efficient tokenization on the hot path."

declare -a TEXTS=(
  ""
  " "
  "   "
  "中"
  "${IK_TEXT}"
  "${BENG_TEXT}"
  "OceanBase 稳定 database 全文索引 tokenizer"
  "The QUICK Brown FOX Jumps Over The Lazy Dog"
  "12345 3.14159 1,000,000 2026-10-31 100% #tag @user"
  "hello 😀 world 🌊 🚀 emoji test"
  "こんにちは 世界 안녕하세요 日本語 한국어"
  "!!! @@@ ### *** --- ... ???"
  "the and of to a in is it what when where who will with"
  "database database database index index search"
  "aBc DeF gHi jKl"
  "全文索引 分词器 tokenizer benchmark"
  "OceanBase数据库OceanBase数据库OceanBase数据库"
  "Mixed 中英文 mixed 测试 123 test"
)

# 超长输入，验证分词器在大文本上的表现
LONG_TEXT=""
for _ in $(seq 1 50); do
  LONG_TEXT="${LONG_TEXT}${IK_TEXT}"
done
TEXTS+=("${LONG_TEXT}")

PARSERS=(ik beng space ngram ngram2)

hex_of() { printf '%s' "$1" | od -An -tx1 -v | tr -d ' \n'; }

{
  echo "========================================"
  echo "FTS Tokenize / Match Regression Dump"
  echo "========================================"
  echo "label:    ${LABEL}"
  echo "database: ${DB}"
  echo "rows:     ${ROWS}"
  echo "----------------------------------------"

  # ------------------------------------------------------------- tokenize
  for parser in "${PARSERS[@]}"; do
    idx=0
    for text in "${TEXTS[@]}"; do
      idx=$((idx + 1))
      echo "--- tokenize parser=${parser} case=${idx} ---"
      # 文本以十六进制字面量传入，避免任何引号/转义问题影响分词器输入。
      hex=$(hex_of "${text}")
      if [[ -z "${hex}" ]]; then
        $MYSQL -e "SELECT tokenize('', '${parser}');" 2>&1
      else
        $MYSQL -e "SELECT tokenize(CONVERT(UNHEX('${hex}') USING utf8mb4), '${parser}');" 2>&1
      fi
    done
  done

  echo "--- tokenize NULL input ik ---"
  $MYSQL -e "SELECT tokenize(NULL, 'ik');" 2>&1

  # ------------------------------------------------ tokenize parser props
  IK_HEX=$(hex_of "${IK_TEXT}")
  BENG_HEX=$(hex_of "${BENG_TEXT}")
  echo "--- tokenize ik output=ALL ---"
  $MYSQL -e "SELECT tokenize(CONVERT(UNHEX('${IK_HEX}') USING utf8mb4), 'ik', '[{\"output\":\"ALL\"}]');" 2>&1
  echo "--- tokenize ik output=DEFAULT ---"
  $MYSQL -e "SELECT tokenize(CONVERT(UNHEX('${IK_HEX}') USING utf8mb4), 'ik', '[{\"output\":\"DEFAULT\"}]');" 2>&1
  echo "--- tokenize ik additional_args ik_mode=MAX_WORD ---"
  $MYSQL -e "SELECT tokenize(CONVERT(UNHEX('${IK_HEX}') USING utf8mb4), 'ik', '[{\"additional_args\":[{\"ik_mode\":\"MAX_WORD\"}]}]');" 2>&1
  echo "--- tokenize ik additional_args ik_mode=SMART ---"
  $MYSQL -e "SELECT tokenize(CONVERT(UNHEX('${IK_HEX}') USING utf8mb4), 'ik', '[{\"additional_args\":[{\"ik_mode\":\"SMART\"}]}]');" 2>&1
  echo "--- tokenize beng additional_args min_token_size=5 ---"
  $MYSQL -e "SELECT tokenize(CONVERT(UNHEX('${BENG_HEX}') USING utf8mb4), 'beng', '[{\"additional_args\":[{\"min_token_size\":5}]}]');" 2>&1
  echo "--- tokenize beng additional_args max_token_size=10 ---"
  $MYSQL -e "SELECT tokenize(CONVERT(UNHEX('${BENG_HEX}') USING utf8mb4), 'beng', '[{\"additional_args\":[{\"max_token_size\":10}]}]');" 2>&1
  echo "--- tokenize ngram additional_args ngram_token_size=4 ---"
  $MYSQL -e "SELECT tokenize(CONVERT(UNHEX('${IK_HEX}') USING utf8mb4), 'ngram', '[{\"additional_args\":[{\"ngram_token_size\":4}]}]');" 2>&1

  # ------------------------------------------------------------ error path
  echo "--- tokenize unknown parser ---"
  $MYSQL -e "SELECT tokenize('abc', 'not_a_parser');" 2>&1
  echo "--- tokenize invalid properties json ---"
  $MYSQL -e "SELECT tokenize('abc', 'ik', 'not json');" 2>&1
  echo "--- tokenize unsupported config ---"
  $MYSQL -e "SELECT tokenize('abc', 'ik', '[{\"unknown_config\":1}]');" 2>&1

  # ---------------------------------------------------------------- match
  echo "--- match setup ---"
  $MYSQL < "${SCRIPT_DIR}/fts_large_bench_setup.sql" >/dev/null 2>&1
  python3 "${SCRIPT_DIR}/fts_large_bench_gen.py" --rows "${ROWS}" --batch 500 | $MYSQL >/dev/null 2>&1
  $MYSQL -e "USE ${DB}; SELECT COUNT(*) FROM docs;" 2>&1
  $MYSQL -e "USE ${DB}; ALTER TABLE docs ADD FULLTEXT INDEX fti_all_ik(title, content) WITH PARSER ik;" 2>&1
  $MYSQL -e "USE ${DB}; ALTER TABLE docs ADD FULLTEXT INDEX fti_content_ik(content) WITH PARSER ik;" 2>&1
  $MYSQL -e "USE ${DB}; ALTER TABLE docs ADD FULLTEXT INDEX fti_en_beng(title_en, content_en) WITH PARSER beng;" 2>&1
  $MYSQL -e "USE ${DB}; ALTER TABLE docs ADD FULLTEXT INDEX fti_en_ngram(title_en) WITH PARSER ngram;" 2>&1

  echo "--- match cn ---"
  $MYSQL -e "USE ${DB}; SELECT COUNT(*) FROM docs WHERE MATCH(title, content) AGAINST('全文索引 分词器');" 2>&1
  echo "--- match beng ---"
  $MYSQL -e "USE ${DB}; SELECT COUNT(*) FROM docs WHERE MATCH(title_en, content_en) AGAINST('database performance audit');" 2>&1
  echo "--- match mixed ---"
  $MYSQL -e "USE ${DB}; SELECT COUNT(*) FROM docs WHERE MATCH(title, content) AGAINST('OceanBase 稳定 database');" 2>&1
  echo "--- match limit ---"
  $MYSQL -e "USE ${DB}; SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20) t;" 2>&1
  echo "--- match single word ---"
  $MYSQL -e "USE ${DB}; SELECT COUNT(*) FROM docs WHERE MATCH(content) AGAINST('database');" 2>&1
  echo "--- match upper case ---"
  $MYSQL -e "USE ${DB}; SELECT COUNT(*) FROM docs WHERE MATCH(content) AGAINST('DATABASE');" 2>&1
  echo "--- match ngram ---"
  $MYSQL -e "USE ${DB}; SELECT COUNT(*) FROM docs WHERE MATCH(title_en) AGAINST('Tokenization');" 2>&1
  echo "--- match no hit ---"
  $MYSQL -e "USE ${DB}; SELECT COUNT(*) FROM docs WHERE MATCH(content) AGAINST('zzzznotexistword');" 2>&1
  echo "--- match empty against ---"
  $MYSQL -e "USE ${DB}; SELECT COUNT(*) FROM docs WHERE MATCH(content) AGAINST('');" 2>&1
  echo "--- match stopwords only ---"
  $MYSQL -e "USE ${DB}; SELECT COUNT(*) FROM docs WHERE MATCH(content_en) AGAINST('the and of');" 2>&1
  echo "--- match sample rows ---"
  $MYSQL -e "USE ${DB}; SELECT id FROM docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') ORDER BY id LIMIT 5;" 2>&1

  echo "----------------------------------------"
  echo "end of regression dump"
  echo "========================================"
} > "${TMP_OUT}" 2>&1

cat "${TMP_OUT}"
if [[ -n "${OUTPUT}" ]]; then
  cp "${TMP_OUT}" "${OUTPUT}"
  echo "regression dump written to ${OUTPUT}" >&2
fi

# ---------------------------------------------------------------- golden check
if [[ -n "${EXPECT:-}" ]]; then
  # the label line is informational only, normalise it before comparing
  normalise() { sed -e 's/^label:.*/label: NORMALISED/' "$1"; }
  if [[ ! -f "${EXPECT}" ]]; then
    echo "ERROR: expected file '${EXPECT}' does not exist" >&2
    exit 2
  fi
  if diff -u <(normalise "${EXPECT}") <(normalise "${TMP_OUT}") > "${TMP_OUT}.diff"; then
    echo "regression check PASSED: output is identical to ${EXPECT}" >&2
    exit 0
  else
    echo "ERROR: regression check FAILED, diff against ${EXPECT}:" >&2
    cat "${TMP_OUT}.diff" >&2
    exit 1
  fi
fi
