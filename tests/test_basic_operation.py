import pgautofailover_utils as pgautofailover
from nose.tools import raises, eq_

import time

cluster = None
monitor = None
node1 = None
node2 = None
node3 = None

def setup_module():
    global cluster
    cluster = pgautofailover.Cluster()

def teardown_module():
    cluster.destroy()

def test_000_create_monitor():
    global monitor
    monitor = cluster.create_monitor("/tmp/basic/monitor")
    monitor.run()

def test_001_init_primary():
    global node1
    node1 = cluster.create_datanode("/tmp/basic/node1")
    node1.create()

    # the name of the node should be "%s_%d" % ("node", node1.nodeid)
    eq_(node1.get_nodename(), "node_%d" % node1.get_nodeid())

    # we can change the name on the monitor with pg_autoctl set node metadata
    node1.set_metadata(name="node a")
    eq_(node1.get_nodename(), "node a")

    node1.run()
    assert node1.wait_until_state(target_state="single")

    # we can also change the name directly in the configuration file
    node1.config_set("pg_autoctl.name", "a")

    # wait until the reload signal has been processed before checking
    time.sleep(2)
    eq_(node1.get_nodename(), "a")

def test_002_stop_postgres():
    node1.stop_postgres()
    assert node1.wait_until_pg_is_running()

def test_003_create_t1():
    node1.run_sql_query("CREATE TABLE t1(a int)")
    node1.run_sql_query("INSERT INTO t1 VALUES (1), (2)")

def test_004_init_secondary():
    global node2
    node2 = cluster.create_datanode("/tmp/basic/node2")

    # register the node on the monitor with a first name for tests
    node2.create(name="node_b")
    eq_(node2.get_nodename(), "node_b")

    # now run the node and change its name again
    node2.run(name="b")
    eq_(node2.get_nodename(), "b")

    assert node2.wait_until_state(target_state="secondary")
    assert node1.wait_until_state(target_state="primary")

    assert node1.has_needed_replication_slots()
    assert node2.has_needed_replication_slots()

def test_005_read_from_secondary():
    results = node2.run_sql_query("SELECT * FROM t1")
    assert results == [(1,), (2,)]

@raises(Exception)
def test_006_writes_to_node2_fail():
    node2.run_sql_query("INSERT INTO t1 VALUES (3)")

@raises(Exception)
def test_007a_maintenance_primary():
    assert node1.wait_until_state(target_state="primary")
    node1.enable_maintenance()  # without --allow-failover, that fails

def test_007b_maintenance_primary_allow_failover():
    print()
    print("Enabling maintenance on node1, allowing failover")
    assert node1.wait_until_state(target_state="primary")
    node1.enable_maintenance(allowFailover=True)
    assert node1.wait_until_state(target_state="maintenance")
    assert node2.wait_until_state(target_state="wait_primary")

    print("Disabling maintenance on node1")
    node1.disable_maintenance()
    assert node1.wait_until_pg_is_running()
    assert node1.wait_until_state(target_state="secondary")
    assert node2.wait_until_state(target_state="primary")

def test_008_maintenance_secondary():
    print()
    print("Enabling maintenance on node2")
    assert node2.wait_until_state(target_state="primary")
    node1.enable_maintenance()
    assert node1.wait_until_state(target_state="maintenance")
    node1.stop_postgres()
    node2.run_sql_query("INSERT INTO t1 VALUES (3)")

    print("Disabling maintenance on node2")
    node1.disable_maintenance()
    assert node1.wait_until_pg_is_running()
    assert node1.wait_until_state(target_state="secondary")
    assert node2.wait_until_state(target_state="primary")

# the rest of the tests expect node1 to be primary, make it so
def test_009_failback():
    print()
    monitor.failover()
    assert node2.wait_until_state(target_state="secondary")
    assert node1.wait_until_state(target_state="primary")

def test_010_fail_primary():
    print()
    print("Injecting failure of node1")
    node1.fail()
    assert node2.wait_until_state(target_state="wait_primary")

def test_011_writes_to_node2_succeed():
    node2.run_sql_query("INSERT INTO t1 VALUES (4)")
    results = node2.run_sql_query("SELECT * FROM t1 ORDER BY a")
    assert results == [(1,), (2,), (3,), (4,)]

def test_012_start_node1_again():
    node1.run()
    assert node2.wait_until_state(target_state="primary")
    assert node1.wait_until_state(target_state="secondary")

def test_013_read_from_new_secondary():
    results = node1.run_sql_query("SELECT * FROM t1 ORDER BY a")
    assert results == [(1,), (2,), (3,), (4,)]

@raises(Exception)
def test_014_writes_to_node1_fail():
    node1.run_sql_query("INSERT INTO t1 VALUES (3)")

def test_015_fail_secondary():
    node1.fail()
    assert node2.wait_until_state(target_state="wait_primary")

def test_016_drop_secondary():
    node1.run()
    assert node1.wait_until_state(target_state="secondary")
    node1.drop()
    assert not node1.pg_is_running()
    assert node2.wait_until_state(target_state="single")

    # replication slot list should be empty now
    assert node2.has_needed_replication_slots()

def test_017_add_new_secondary():
    global node3
    node3 = cluster.create_datanode("/tmp/basic/node3")
    node3.create()

@raises(Exception)
def test_018_cant_failover_yet():
    monitor.failover()

def test_019_run_secondary():
    node3.run()
    assert node3.wait_until_state(target_state="secondary")
    assert node2.wait_until_state(target_state="primary")

    assert node2.has_needed_replication_slots()
    assert node3.has_needed_replication_slots()

# In previous versions of pg_auto_failover we removed the replication slot
# on the secondary after failover. Now, we instead maintain the replication
# slot's replay_lsn thanks for the monitor tracking of the nodes' LSN
# positions.
#
# So rather than checking that we want to zero replication slots after
# replication, we check that we still have a replication slot for the other
# node.
#
def test_020_multiple_manual_failover_verify_replication_slots():
    print()

    print("Calling pgautofailover.failover() on the monitor")
    monitor.failover()
    assert node2.wait_until_state(target_state="secondary")
    assert node3.wait_until_state(target_state="primary")

    assert node2.has_needed_replication_slots()
    assert node3.has_needed_replication_slots()

    print("Calling pgautofailover.failover() on the monitor")
    monitor.failover()
    assert node2.wait_until_state(target_state="primary")
    assert node3.wait_until_state(target_state="secondary")

    assert node2.has_needed_replication_slots()
    assert node3.has_needed_replication_slots()

def test_021_drop_primary():
    node2.drop()
    assert not node2.pg_is_running()
    assert node3.wait_until_state(target_state="single")

def test_022_stop_postgres_monitor():
    original_state = node3.get_state()
    monitor.stop_postgres()
    monitor.wait_until_pg_is_running()
    assert node3.wait_until_state(target_state=original_state)
