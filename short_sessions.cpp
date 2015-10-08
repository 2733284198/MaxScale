/**
 * @file short_sessions.cpp Executes a lof of short queries, use own short session for every query (some relations to bug#424)
 *
 * - using RSplit create table
 * - close connection
 * - do 10000 times: open connections to RWSplit, execute short INSERT, close connection
 * - do 10000 times: open connection to RWSplit, execute short SELECT, close connection
 * - repeat last previous step also to ReadConn master and ReadConn slave
 * - check if Maxscale alive
 */

#include <my_config.h>
#include <iostream>
#include "testconnections.h"
#include "sql_t1.h"

using namespace std;

int main(int argc, char *argv[])
{

    TestConnections * Test = new TestConnections(argc, argv);
    int global_result = 0;

    Test->read_env();
    Test->print_env();
    Test->repl->connect();
    fflush(stdout);

    MYSQL * conn;
    char sql[100];

    conn = Test->open_rwsplit_connection();
    execute_query(conn, (char *) "DROP DATABASE IF EXISTS test;");
    execute_query(conn, (char *) "CREATE DATABASE test; USE test;");
    execute_query(conn, (char *) "USE test_non_existing_DB; USE test;");
    create_t1(conn);
    mysql_close(conn);
    printf("Table t1 is created\n");
    fflush(stdout);

    for (int i = 0; i < 10000; i++) {
        conn = Test->open_rwsplit_connection();
        sprintf(sql, "INSERT INTO t1 (x1, fl) VALUES(%d, 1);", i);
        printf("%s\n", sql);
        execute_query(conn, sql);
        fflush(stdout);
        mysql_close(conn);
    }
    fflush(stdout);

    printf("Connecting to MaxScale\n");
    fflush(stdout);
    global_result += Test->connect_maxscale();
    printf("Checking t1 table using RWSplit router\n");
    fflush(stdout);
    global_result += execute_select_query_and_check(Test->conn_rwsplit, (char *) "SELECT * FROM t1;", 10000);
    printf("Checking t1 table using ReadConn router in master mode\n");
    fflush(stdout);
    global_result += execute_select_query_and_check(Test->conn_master, (char *) "SELECT * FROM t1;", 10000);
    printf("Checking t1 table using ReadConn router in slave mode\n");
    fflush(stdout);
    global_result += execute_select_query_and_check(Test->conn_slave, (char *) "SELECT * FROM t1;", 10000);
    fflush(stdout);
    Test->close_maxscale_connections();

    global_result +=Test->check_maxscale_alive();

    Test->copy_all_logs(); return(Test->global_result);
}
