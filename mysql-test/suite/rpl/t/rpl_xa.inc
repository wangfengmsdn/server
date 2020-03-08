#
# This "body" file checks general properties of XA transaction replication
# as of MDEV-7974.
# Parameters:
# --let rpl_xa_check= SELECT ...
#
connection master;
create table t1 (a int, b int) engine=InnoDB;
insert into t1 values(0, 0);
xa start 't';
insert into t1 values(1, 2);
xa end 't';
xa prepare 't';
xa commit 't';

sync_slave_with_master;
let $diff_tables= master:t1, slave:t1;
source include/diff_tables.inc;

connection master;

xa start 't';
insert into t1 values(3, 4);
xa end 't';
xa prepare 't';
xa rollback 't';

sync_slave_with_master;
let $diff_tables= master:t1, slave:t1;
source include/diff_tables.inc;

connection master;
--disable_warnings
SET pseudo_slave_mode=1;
--enable_warnings
create table t2 (a int) engine=InnoDB;
xa start 't';
insert into t1 values (5, 6);
xa end 't';
xa prepare 't';
xa start 's';
insert into t2 values (0);
xa end 's';
xa prepare 's';
--source include/save_master_gtid.inc

connection slave;
source include/sync_with_master_gtid.inc;
if ($rpl_xa_check)
{
  --eval $rpl_xa_check
  if ($rpl_xa_verbose)
  {
    --eval SELECT $rpl_xa_check_lhs
    --eval SELECT $rpl_xa_check_rhs
  }
}
xa recover;

connection master;
xa commit 't';
xa commit 's';
--disable_warnings
SET pseudo_slave_mode=0;
--enable_warnings
sync_slave_with_master;
let $diff_tables= master:t1, slave:t1;
source include/diff_tables.inc;
let $diff_tables= master:t2, slave:t2;
source include/diff_tables.inc;

#
# Read-only XA remains prepared after disconnect and must rollback at XA-complete
# after recoonect. To the read-only also belongs non-transactional engine XA.
#
--connection master
flush logs;
create table tm (a int) engine=myisam;

#--let $p_trx=1
#--connect (master_s1, 127.0.0.1,root,,test,$MASTER_MYPORT,)
#xa start 's1'; select 1; xa end 's1'; xa prepare 's1';
#--disconnect master_s

set @query1="select 1";
set @query2="select count(*) into @s2 from t1";
set @query3="insert into tm set a=1";

--let $db=test
--let $ro_cases=3
--let $p_trx=$ro_cases
while ($p_trx)
{
--connection master
  --let $xid=ro_$p_trx
  --let $query=`SELECT @query$p_trx`
  --source rpl_create_xa_prepared.inc
  --let $complete=`select if(floor(rand()*10)%2,'COMMIT','ROLLBACK')`
  --error 0
  --disable_query_log
  --disable_result_log
  --eval xa $complete '$xid'
  --enable_result_log
  --enable_query_log

  --disconnect master_$xid
  --source include/wait_until_disconnected.inc

  --dec $p_trx
}

--let $p_trx=$ro_cases
while ($p_trx)
{
--connection master
  --let $xid=ro_$p_trx
  --let $query=`SELECT @query$p_trx`
  --source rpl_create_xa_prepared.inc

  --disconnect master_$xid
  --source include/wait_until_disconnected.inc

  --dec $p_trx
}

--echo *** $ro_cases prepared xa:s must be in the list:
--connection master
xa recover;

--let $p_trx=$ro_cases
while ($p_trx)
{
  --let $complete=`select if(floor(rand()*10)%2,'COMMIT','ROLLBACK')`
  --disable_query_log
  --disable_result_log
  --error ER_XA_RBROLLBACK
  --eval xa $complete 'ro_$p_trx'
  --enable_result_log
  --enable_query_log

  --dec $p_trx
}
--echo *** Zero prepared xa:s must be in the list:
xa recover;

#
# No XA logging cases
#
# A. Binlog filter


--let $db=test_ign
--eval create database $db
set @@sql_log_bin = 0;
--eval create table $db.t (a int) engine=InnoDB;
set @@sql_log_bin = 1;

--let $xid=rw_no_binlog
--let $query=insert into $db_ign.t set a=1
--source rpl_create_xa_prepared.inc
--disconnect master_$xid
--source include/wait_until_disconnected.inc

--echo *** $xid must be in the list:
--connection master
xa recover;

--let $complete=`select if(floor(rand()*10)%2,'COMMIT','ROLLBACK')`
--error 0
--disable_query_log
--disable_result_log
--eval xa $complete '$xid'
--enable_result_log
--enable_query_log

--echo *** Zero must be in the list:
--connection master
xa recover;

--let $file= query_get_value(SHOW MASTER STATUS, File, 1)
--source include/show_gtid_list.inc

--source include/save_master_gtid.inc

connection slave;
source include/sync_with_master_gtid.inc;


# B. Session binlog disable does not log even empty XA-prepare but XA-complete will be
# logged despite of that.

--let $db=test
--let $xid=skip_binlog
--let $query=insert into t2 values(1)
--connect (master_$xid, 127.0.0.1,root,,$db,$MASTER_MYPORT,)
set @@session.sql_log_bin = OFF;
--eval xa start '$xid'
  --eval $query
--eval xa end '$xid'
--eval xa prepare '$xid';

--disconnect master_$xid

--echo *** $xid must be in the list:
--connection master
xa recover;

--connection master
call mtr.add_suppression("Slave: XAER_NOTA: Unknown XID");
xa rollback 'skip_binlog';

--echo *** Zero must be in the list:
--connection master
xa recover;

#
# Expected error on slave and manual correction
#
--connection slave
let $slave_sql_errno= 1397; # ER_XAER_NOTA
source include/wait_for_slave_sql_error.inc;
set @@global.sql_slave_skip_counter= 1;
--source include/start_slave.inc

connection master;
--eval drop database test_ign
drop table t1, t2, tm;
