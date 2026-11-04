SET NAMES utf8mb4;
DROP DATABASE IF EXISTS fts17_parallel;
CREATE DATABASE fts17_parallel;
USE fts17_parallel;
CREATE TABLE first_docs(id INT PRIMARY KEY, body TEXT);
CREATE TABLE second_docs LIKE first_docs;
CREATE TABLE ordinary(id INT PRIMARY KEY, tag INT, payload VARCHAR(64));
INSERT INTO first_docs WITH RECURSIVE seq(n) AS
 (SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n<513)
 SELECT n, CONCAT(CASE MOD(n,3) WHEN 0 THEN 'alpha beta' WHEN 1 THEN 'alpha' ELSE 'beta' END,
   ' ', REPEAT('padding ', MOD(n,7))) FROM seq;
INSERT INTO second_docs SELECT id,body FROM first_docs;
INSERT INTO ordinary SELECT id,MOD(id,11),CONCAT('payload',id) FROM first_docs;
