--source include/have_innodb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc

# Alter table should fail for strict sql mode

CREATE TABLE t1(c1 INT NOT NULL, c2 INT, PRIMARY KEY(c1))ENGINE=INNODB;
INSERT INTO t1 VALUES(1, 1);
--enable_info
SET DEBUG_SYNC= 'row_merge_after_scan
SIGNAL opened WAIT_FOR flushed';
send ALTER TABLE t1 CHANGE c2 c2 INT NOT NULL DEFAULT 2;
connect (con1,localhost,root);
SET DEBUG_SYNC= 'now WAIT_FOR opened';
INSERT INTO t1 VALUES(2, NULL);
UPDATE t1 SET c1 = 3 WHERE c2 = 1;
SET DEBUG_SYNC= 'now SIGNAL flushed';
connection default;
--error $error_code
reap;
--disable_info
SELECT * FROM t1;
DROP TABLE t1;

# Alter table should successfully apply the log for the alter operation

CREATE TABLE t1(c1 INT NOT NULL, c2 INT, PRIMARY KEY(c1))ENGINE=INNODB;
INSERT INTO t1 VALUES(1, 1);
--enable_info
SET DEBUG_SYNC= 'row_merge_after_scan
SIGNAL opened WAIT_FOR flushed';
send ALTER TABLE t1 CHANGE c2 c2 INT NOT NULL DEFAULT 2;
connection con1;
SET DEBUG_SYNC= 'now WAIT_FOR opened';
INSERT INTO t1 VALUES(2, 3);
UPDATE t1 SET c1 = 3 WHERE c2 = 1;
SET DEBUG_SYNC= 'now SIGNAL flushed';
connection default;
reap;
--disable_info
SELECT * FROM t1;
DROP TABLE t1;

# Alter ignore should not give error

CREATE TABLE t1(c1 INT NOT NULL, c2 INT, c3 INT, PRIMARY KEY(c1))ENGINE=INNODB;
INSERT INTO t1 VALUES(1, 2, 3);
--enable_info
SET DEBUG_SYNC= 'row_merge_after_scan
SIGNAL opened WAIT_FOR flushed';
send ALTER IGNORE TABLE t1 CHANGE c2 c2 INT NOT NULL DEFAULT 2;
connection con1;
SET DEBUG_SYNC= 'now WAIT_FOR opened';
UPDATE t1 SET c2= 2 WHERE c1 = 1;
INSERT INTO t1 VALUES (2, NULL, 4);
SET DEBUG_SYNC= 'now SIGNAL flushed';
connection default;
reap;
--disable_info
SELECT * FROM t1;
DROP TABLE t1;
disconnect con1;
SET DEBUG_SYNC='RESET';
