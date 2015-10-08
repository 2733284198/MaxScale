/**
 * @file bug656.cpp Checks Maxscale behaviour in case if Master node is blocked
 *
 * - Connecto RWSplit
 * - block Mariadb server on Master node by Firewall
 * - try simple query *show servers" via Maxadmin
 */

#include <my_config.h>
#include "testconnections.h"
#include "maxadmin_operations.h"

int main(int argc, char *argv[])
{
    TestConnections * Test = new TestConnections(argc, argv);
    Test->set_timeout(30);

    Test->tprintf("Connecting to RWSplit %s\n", Test->maxscale_IP);
    Test->connect_rwsplit();

    printf("Setup firewall to block mysql on master\n"); fflush(stdout);
    Test->repl->block_node(0);

    //printf("Trying query to RWSplit, expecting failure, but not a crash\n"); fflush(stdout);
    //execute_query(Test->conn_rwsplit, (char *) "show processlist;");
    execute_maxadmin_command_print(Test->maxscale_IP, (char *) "admin", Test->maxadmin_password, (char *) "show servers");

    Test->tprintf("Setup firewall back to allow mysql and wait\n"); fflush(stdout);
    Test->repl->unblock_node(0);
    sleep(10);

    Test->check_maxscale_alive();
    Test->close_rwsplit();
    Test->copy_all_logs(); return(Test->global_result);
}


