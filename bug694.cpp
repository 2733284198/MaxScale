/**
 * @file bug694.cpp  - regression test for bug694 ("RWSplit: SELECT @a:=@a+1 as a, test.b FROM test breaks client session")
 *
 * - set use_sql_variables_in=all in MaxScale.cnf
 * - connect to readwritesplit router and execute:
 * @verbatim
CREATE TABLE test (b integer);
SELECT @a:=@a+1 as a, test.b FROM test;
USE test
@endverbatim
 * - check if MaxScale alive
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

    printf("Trying SELECT @a:=@a+1 as a, test.b FROM test\n"); fflush(stdout);
    global_result += execute_query(Test->conn_rwsplit, "DROP TABLE IF EXISTS test; CREATE TABLE test (b integer);");
    for (int i=0; i<10000;i++) {execute_query(Test->conn_rwsplit, "insert into test value(2);");}
    if (execute_query(Test->conn_rwsplit, "SELECT @a:=@a+1 as a, test.b FROM test;") == 0) {
        printf("Query succeded, but expected to fail. Test FAILED!\n"); fflush(stdout);
        global_result++;
    }
    printf("Trying USE test\n"); fflush(stdout);
    global_result += execute_query(Test->conn_rwsplit, "USE test");

    global_result += execute_query(Test->conn_rwsplit, "DROP TABLE IF EXISTS test;");

    printf("Checking if MaxScale alive\n"); fflush(stdout);
    Test->close_maxscale_connections();

    printf("Checking logs\n"); fflush(stdout);
    global_result    +=Test->check_log_err((char *) "Warning : The query can't be routed to all backend servers because it includes SELECT and SQL variable modifications which is not supported", TRUE);
    global_result    +=Test->check_log_err((char *) "SELECT with session data modification is not supported if configuration parameter use_sql_variables_in=all", TRUE);

    global_result +=Test->check_maxscale_alive();

    Test->copy_all_logs(); return(global_result);
}

