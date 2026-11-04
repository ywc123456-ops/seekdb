SET NAMES utf8mb4;
CREATE DATABASE IF NOT EXISTS fts_score_regression;
USE fts_score_regression;
DROP TABLE IF EXISTS topk_large_docs;
CREATE TABLE topk_large_docs(id INT PRIMARY KEY, body TEXT);
INSERT INTO topk_large_docs
 WITH RECURSIVE a(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM a WHERE n<256),
 b(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM b WHERE n<255)
 SELECT a.n*256+b.n+1, 'alpha beta gamma' FROM a CROSS JOIN b;
CREATE FULLTEXT INDEX large_beng ON topk_large_docs(body) WITH PARSER beng;
SELECT COUNT(*) FROM topk_large_docs;
SELECT COUNT(*), MIN(relevance), MAX(relevance) FROM
 (SELECT id, CAST(MATCH(body) AGAINST('alpha beta gamma') AS DECIMAL(20,12)) AS relevance
 FROM topk_large_docs WHERE MATCH(body) AGAINST('alpha beta gamma')
 ORDER BY relevance DESC LIMIT 20) ranked;
SELECT COUNT(*), MIN(relevance), MAX(relevance) FROM
 (SELECT id, CAST(MATCH(body) AGAINST('+alpha +beta' IN BOOLEAN MODE) AS DECIMAL(20,12)) AS relevance
 FROM topk_large_docs WHERE MATCH(body) AGAINST('+alpha +beta' IN BOOLEAN MODE)
 ORDER BY relevance DESC LIMIT 17 OFFSET 13) ranked;
DROP TABLE topk_large_docs;
