SET NAMES utf8mb4;
USE fts17_parallel;
SELECT COUNT(*),SUM(id) FROM first_docs WHERE MATCH(body) AGAINST('alpha beta');
SELECT COUNT(*),SUM(id) FROM second_docs WHERE MATCH(body) AGAINST('+alpha -beta' IN BOOLEAN MODE);
SELECT COUNT(*),SUM(id) FROM ordinary FORCE INDEX(ordinary_tag) WHERE tag BETWEEN 2 AND 5;
SELECT GROUP_CONCAT(CONCAT(id,':',CAST(relevance AS DECIMAL(20,12))) ORDER BY id)
 FROM (SELECT id,MATCH(body) AGAINST('alpha beta') relevance FROM first_docs
 WHERE MATCH(body) AGAINST('alpha beta') ORDER BY relevance DESC,id LIMIT 20 OFFSET 7) ranked;
INSERT INTO first_docs VALUES(9001,'alpha beta alpha'),(9002,'beta beta');
UPDATE first_docs SET body='alpha alpha padding' WHERE MOD(id,11)=0;
DELETE FROM first_docs WHERE MOD(id,17)=0;
UPDATE ordinary SET tag=tag+2 WHERE MOD(id,13)=0;
DELETE FROM ordinary WHERE MOD(id,23)=0;
SELECT COUNT(*),SUM(id) FROM first_docs WHERE MATCH(body) AGAINST('alpha beta');
SELECT COUNT(*),SUM(id) FROM first_docs WHERE body LIKE '%alpha%' OR body LIKE '%beta%';
SELECT COUNT(*),SUM(id) FROM first_docs WHERE MATCH(body) AGAINST('+alpha -beta' IN BOOLEAN MODE);
SELECT COUNT(*),SUM(id) FROM first_docs WHERE body LIKE '%alpha%' AND body NOT LIKE '%beta%';
SELECT COUNT(*),SUM(id) FROM ordinary FORCE INDEX(ordinary_tag) WHERE tag BETWEEN 2 AND 5;
SELECT COUNT(*),SUM(id) FROM ordinary IGNORE INDEX(ordinary_tag) WHERE tag BETWEEN 2 AND 5;
