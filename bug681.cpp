/**
 * @file bug681.cpp  - regression test for bug681 ("crash if max_slave_connections=10% and 4 or less backends are configured")
 *
 * - Configure RWSplit with max_slave_connections=10%
 * - check ReadConn master and ReadConn slave are alive and RWSplit is not started
 */

#include <my_config.h>
#include <iostream>
#include "testconnections.h"
#include "mariadb_func.h"

int main(int argc, char *argv[])
{
    TestConnections * Test = new TestConnections(argc, argv);
    int global_result = 0;

    Test->read_env();
    Test->print_env();

    Test->connect_maxscale();

    if (Test->conn_rwsplit != NULL) {
        global_result++;
        printf("FAILED: RWSplit services should fail, but it is started\n"); fflush(stdout);
    }

    printf("Trying query to ReadConn master\n"); fflush(stdout);
    global_result += execute_query(Test->conn_master, "show processlist;");
    printf("Trying query to ReadConn slave\n"); fflush(stdout);
    global_result += execute_query(Test->conn_slave, "show processlist;");

    Test->close_maxscale_connections();

    global_result    += check_log_err((char *) "Error : Unable to start RW Split Router service. There are too few backend servers configured in", TRUE);

    Test->copy_all_logs(); return(global_result);
}
