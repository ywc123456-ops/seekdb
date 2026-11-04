SET NAMES utf8mb4;
SELECT TOKENIZE('hello hello WORLD 中文', 'beng', '[{"output":"all"}]');
SELECT TOKENIZE('OceanBase数据库全文索引', 'ik', '[{"output":"all"}]');
SELECT TOKENIZE('OceanBase数据库全文索引', 'ik', '[{"additional_args":[{"ik_mode":"max_word"}]}]');
SELECT TOKENIZE('hello WORLD small an elephant', 'beng', '[{"additional_args":[{"min_token_size":4}]}]');
SELECT TOKENIZE('hello 中文', 'beng', '[]');
SELECT TOKENIZE('hello WORLD 中文', 'space');
SELECT TOKENIZE('hello WORLD 中文', 'space', '[]');
SELECT TOKENIZE('hello WORLD 中文', 'ngram');
SELECT TOKENIZE('hello WORLD 中文', 'ngram', '[]');
SELECT TOKENIZE('hello WORLD 中文', 'ngram2');
SELECT TOKENIZE('hello WORLD 中文', 'ngram2', '[]');
SELECT TOKENIZE(CAST(TOKENIZE('Alpha Beta Gamma', 'beng') AS CHAR), 'beng');
SELECT TOKENIZE(REPEAT('Alpha WORLD 中文 😀 ', 20), 'ik'),
 TOKENIZE(REPEAT('Beta WORLD 中文 ', 500), 'beng', '[{"output":"all"}]');
CREATE DATABASE IF NOT EXISTS fts_score_regression;
USE fts_score_regression;
DROP TABLE IF EXISTS score_docs;
CREATE TABLE score_docs(id INT PRIMARY KEY, body LONGTEXT, body_en LONGTEXT);
INSERT INTO score_docs VALUES
 (1, '数据库 database 全文索引', 'database performance audit'),
 (2, '全文索引 全文索引 database', 'database database performance'),
 (3, '不相关的内容', 'unrelated content'),
 (4, '数据库 OceanBase', 'audit audit database'),
 (5, NULL, NULL),
 (6, '', ''),
 (7, '数据库 performance', 'the and of');
CREATE FULLTEXT INDEX score_ik ON score_docs(body) WITH PARSER ik;
CREATE FULLTEXT INDEX score_beng ON score_docs(body_en) WITH PARSER beng;
SELECT COUNT(*) FROM score_docs WHERE MATCH(body) AGAINST('数据库');
SELECT COUNT(*) FROM score_docs WHERE MATCH(body) AGAINST('数据库') > 0;
SELECT COUNT(*) FROM score_docs WHERE 0 < MATCH(body) AGAINST('数据库');
SELECT COUNT(*) FROM score_docs WHERE MATCH(body) AGAINST('数据库') > 0.01;
SELECT COUNT(*) FROM score_docs WHERE MATCH(body) AGAINST('数据库') > 100;
SELECT COUNT(*) FROM score_docs WHERE MATCH(body_en) AGAINST('database performance');
SELECT COUNT(*) FROM score_docs WHERE MATCH(body_en) AGAINST('the and of');
SELECT COUNT(*) FROM score_docs WHERE MATCH(body_en) AGAINST('doesnotexist');
SELECT id, CAST(MATCH(body) AGAINST('数据库') AS DECIMAL(20,12))
 FROM score_docs WHERE MATCH(body) AGAINST('数据库') ORDER BY id;
SELECT id, CAST(MATCH(body_en) AGAINST('database audit') AS DECIMAL(20,12)) AS score
 FROM score_docs WHERE MATCH(body_en) AGAINST('database audit') ORDER BY score DESC, id LIMIT 3;
SELECT id, (MATCH(body_en) AGAINST('database') > 0) AS matched
 FROM score_docs ORDER BY id;
SELECT COUNT(*) FROM score_docs WHERE MATCH(body_en) AGAINST('+database -unrelated' IN BOOLEAN MODE);
SELECT COUNT(*) FROM (SELECT id FROM score_docs WHERE MATCH(body) AGAINST('全文索引') LIMIT 2) t;
PREPARE score_query FROM 'SELECT COUNT(*) FROM score_docs WHERE MATCH(body_en) AGAINST(''database'') > ?';
SET @threshold = 0;
EXECUTE score_query USING @threshold;
SET @threshold = 0.01;
EXECUTE score_query USING @threshold;
SET @threshold = 100;
EXECUTE score_query USING @threshold;
SET @threshold = 0;
EXECUTE score_query USING @threshold;
DEALLOCATE PREPARE score_query;
SET group_concat_max_len = 16384;
SET @many_terms = (WITH RECURSIVE terms(n) AS
 (SELECT 1000 UNION ALL SELECT n + 1 FROM terms WHERE n < 1300)
 SELECT CONCAT('database ', GROUP_CONCAT(CONCAT('missing', n) SEPARATOR ' ')) FROM terms);
PREPARE many_query FROM 'SELECT COUNT(*) FROM score_docs WHERE MATCH(body_en) AGAINST(?)';
EXECUTE many_query USING @many_terms;
DEALLOCATE PREPARE many_query;
DROP TABLE score_docs;
DROP TABLE IF EXISTS batch_docs;
CREATE TABLE batch_docs(id INT PRIMARY KEY, body TEXT);
INSERT INTO batch_docs WITH RECURSIVE seq(n) AS
 (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 769)
 SELECT n, CONCAT(CASE WHEN MOD(n,3)=0 THEN 'alpha beta' WHEN MOD(n,3)=1 THEN 'alpha' ELSE 'beta' END,
   CASE WHEN MOD(n,5)=0 THEN ' gamma' ELSE '' END,
   CASE WHEN MOD(n,7)=0 THEN ' delta' ELSE '' END) FROM seq;
CREATE FULLTEXT INDEX batch_beng ON batch_docs(body) WITH PARSER beng;
SELECT COUNT(*), SUM(id), MIN(id), MAX(id) FROM batch_docs WHERE MATCH(body) AGAINST('alpha beta');
SELECT GROUP_CONCAT(id ORDER BY id) FROM batch_docs WHERE MATCH(body) AGAINST('alpha beta');
SELECT COUNT(*), SUM(id) FROM batch_docs WHERE MATCH(body) AGAINST('alpha');
SELECT COUNT(*), SUM(id) FROM batch_docs WHERE MATCH(body) AGAINST('beta');
SELECT GROUP_CONCAT(id ORDER BY id) FROM batch_docs WHERE MATCH(body) AGAINST('alpha beta gamma');
SELECT GROUP_CONCAT(id ORDER BY id) FROM batch_docs WHERE MATCH(body) AGAINST('alpha beta gamma delta');
SELECT COUNT(*), SUM(id) FROM batch_docs WHERE MATCH(body) AGAINST('alpha missingqqq');
SELECT id FROM batch_docs ORDER BY body COLLATE utf8mb4_general_ci DESC, id;
SELECT id, CAST(MATCH(body) AGAINST('alpha beta gamma delta') AS DECIMAL(20,12)) AS relevance
 FROM batch_docs WHERE MATCH(body) AGAINST('alpha beta gamma delta') ORDER BY relevance DESC, id LIMIT 20;
SELECT GROUP_CONCAT(CAST(relevance AS DECIMAL(20,12)) ORDER BY relevance)
 FROM (SELECT id, MATCH(body) AGAINST('alpha beta gamma') AS relevance FROM batch_docs
 WHERE MATCH(body) AGAINST('alpha beta gamma') ORDER BY relevance DESC LIMIT 20) ranked;
SELECT GROUP_CONCAT(CAST(relevance AS DECIMAL(20,12)) ORDER BY relevance)
 FROM (SELECT id, MATCH(body) AGAINST('gamma delta') AS relevance FROM batch_docs
 WHERE MATCH(body) AGAINST('gamma delta') ORDER BY relevance DESC LIMIT 17 OFFSET 13) ranked;
SELECT COUNT(*) FROM (SELECT id FROM batch_docs WHERE MATCH(body) AGAINST('missingqqq') LIMIT 20) ranked;
SELECT COUNT(*) FROM (SELECT id FROM batch_docs WHERE MATCH(body) AGAINST('alpha beta gamma') LIMIT 0) ranked;
PREPARE ranked_query FROM 'SELECT GROUP_CONCAT(CAST(relevance AS DECIMAL(20,12)) ORDER BY relevance) FROM (SELECT id, MATCH(body) AGAINST(?) AS relevance FROM batch_docs WHERE MATCH(body) AGAINST(?) ORDER BY relevance DESC LIMIT 20) ranked';
SET @query = 'alpha';
EXECUTE ranked_query USING @query, @query;
SET @query = 'alpha beta';
EXECUTE ranked_query USING @query, @query;
SET @query = 'alpha beta gamma';
EXECUTE ranked_query USING @query, @query;
SET @query = 'alpha beta gamma delta';
EXECUTE ranked_query USING @query, @query;
SET @query = 'missingqqq';
EXECUTE ranked_query USING @query, @query;
SET @query = 'alpha beta gamma';
EXECUTE ranked_query USING @query, @query;
DEALLOCATE PREPARE ranked_query;
SELECT COUNT(*), SUM(checked.body LIKE '%gamma%' OR checked.body LIKE '%delta%')
 FROM (SELECT id FROM batch_docs WHERE MATCH(body) AGAINST('gamma delta') LIMIT 20) ranked
 JOIN batch_docs checked ON checked.id = ranked.id;
SELECT COUNT(*), SUM(checked.body LIKE '%gamma%' AND checked.body NOT LIKE '%delta%')
 FROM (SELECT id FROM batch_docs WHERE MATCH(body) AGAINST('+gamma -delta' IN BOOLEAN MODE) LIMIT 20) ranked
 JOIN batch_docs checked ON checked.id = ranked.id;
DROP TABLE batch_docs;
DROP TABLE IF EXISTS rank_docs;
CREATE TABLE rank_docs(id INT PRIMARY KEY, body TEXT);
INSERT INTO rank_docs WITH RECURSIVE seq(n) AS
 (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 321)
 SELECT n, CONCAT('alpha beta gamma ', REPEAT('padding ', n)) FROM seq;
CREATE FULLTEXT INDEX unique_rank_beng ON rank_docs(body) WITH PARSER beng;
SELECT COUNT(*), COUNT(DISTINCT MATCH(body) AGAINST('alpha beta gamma')) FROM rank_docs;
PREPARE unique_ranked_query FROM 'SELECT GROUP_CONCAT(CONCAT(id, ":", CAST(relevance AS DECIMAL(20,12))) ORDER BY id) FROM (SELECT id, MATCH(body) AGAINST(?) AS relevance FROM rank_docs WHERE MATCH(body) AGAINST(?) ORDER BY relevance DESC LIMIT 20 OFFSET 13) ranked';
SET @query = 'alpha';
EXECUTE unique_ranked_query USING @query, @query;
SET @query = 'alpha beta';
EXECUTE unique_ranked_query USING @query, @query;
SET @query = 'alpha beta gamma';
EXECUTE unique_ranked_query USING @query, @query;
SET @query = 'alpha beta gamma missingqqq';
EXECUTE unique_ranked_query USING @query, @query;
SET @query = 'alpha beta gamma';
EXECUTE unique_ranked_query USING @query, @query;
DEALLOCATE PREPARE unique_ranked_query;
SELECT COUNT(*) FROM (SELECT id FROM rank_docs
 WHERE MATCH(body) AGAINST('alpha beta gamma') > 100 ORDER BY MATCH(body) AGAINST('alpha beta gamma') DESC LIMIT 20) ranked;
DROP TABLE rank_docs;
DROP TABLE IF EXISTS sort_docs;
CREATE TABLE sort_docs(id INT PRIMARY KEY, value VARCHAR(64) COLLATE utf8mb4_general_ci);
INSERT INTO sort_docs WITH RECURSIVE seq(n) AS
 (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 257)
 SELECT n, CASE MOD(n,7) WHEN 0 THEN NULL WHEN 1 THEN 'a' WHEN 2 THEN 'A '
 WHEN 3 THEN '中文' WHEN 4 THEN 'é' WHEN 5 THEN 'e' ELSE '😀' END FROM seq;
SELECT id, value FROM sort_docs ORDER BY value, id DESC;
SELECT id, value FROM sort_docs ORDER BY value COLLATE utf8mb4_bin DESC, id;
SELECT id, ROW_NUMBER() OVER (PARTITION BY MOD(id,3) ORDER BY value DESC, id) AS ordinal
 FROM sort_docs ORDER BY id;
DROP TABLE sort_docs;
