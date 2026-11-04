"""Compare full TOKENIZE output and indexed MATCH rows on two test instances.

Each client command must select a dedicated test database; this script creates
and drops only the table task4_fts_regression. Example:
  python compare_servers.py --baseline 'mysql -P2881 ... test' \
    --candidate 'mysql -P2882 ... test'
"""
import argparse
import shlex
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--baseline', required=True)
p.add_argument('--candidate', required=True)
a = p.parse_args()
texts = [
    '', ' ', '!!!', 'a', 'Hello WORLD', 'hello hello world',
    'OceanBase 数据库全文检索优化', '中文分词，数据库索引。',
    'ＡＢＣ１２３ abc123', '2026年10月29日 3.14 100公斤',
    'かな カナ 한글 😀', 'foo-bar a_b x@y.com',
    '中A1文，。！？', '数据库' * 1024, 'word ' * 2048,
]
queries = ['hello', 'world', '数据库', '全文检索', '数据库 OceanBase', 'abc123', 'missingtoken']
def literal(s):
    # Binary hex literals with an explicit charset preserve embedded bytes.
    return "CONVERT(X'" + s.encode('utf-8').hex() + "' USING utf8mb4)"
def execute(command, sql):
    result = subprocess.run(shlex.split(command) + [
        '--batch', '--raw', '--skip-column-names', '--default-character-set=utf8mb4'],
        input=sql.encode('utf-8'), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(result.stderr.decode('utf-8', errors='replace'))
    return result.stdout
checks = 0
for parser in ['ik', 'beng']:
    sql = ['DROP TABLE IF EXISTS task4_fts_regression;',
           'CREATE TABLE task4_fts_regression(id INT PRIMARY KEY, body LONGTEXT);']
    for i, text in enumerate(texts):
        sql.append(f'INSERT INTO task4_fts_regression VALUES({i},{literal(text)});')
        sql.append(f"SELECT TOKENIZE({literal(text)}, '{parser}');")
    # Build after loading, so the check exercises the index construction path.
    sql.append('CREATE FULLTEXT INDEX task4_ft ON task4_fts_regression(body) WITH PARSER ' + parser + ';')
    for query in queries:
        match = f'MATCH(body) AGAINST({literal(query)} IN NATURAL LANGUAGE MODE)'
        sql.append(f'SELECT id FROM task4_fts_regression WHERE {match} ORDER BY id;')
        sql.append(f'SELECT id FROM task4_fts_regression WHERE {match} ORDER BY id LIMIT 3;')
        sql.append(f'SELECT COUNT(*) FROM task4_fts_regression WHERE {match};')
    sql.append('DROP TABLE task4_fts_regression;')
    statement = '\n'.join(sql)
    before, after = execute(a.baseline, statement), execute(a.candidate, statement)
    if before != after:
        raise AssertionError(f'{parser}: complete TOKENIZE/MATCH output differs')
    checks += len(texts) + 3 * len(queries)
print(f'PASS: {checks} complete TOKENIZE/MATCH result comparisons across IK and BENG')
