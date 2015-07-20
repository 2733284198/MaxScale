/**
 * @file bug519.cpp
 * - fill t1 wuth data
 * - execute SELECT * INTO OUTFILE '/tmp/t1.csv' FROM t1; against all routers
 * - DROP TABLE t1
 * - LOAD DATA LOCAL INFILE 't1.csv' INTO TABLE t1; using RWSplit
 * - check if t1 contains right data
 * - DROP t1 again and repeat LOAD DATA LOCAL INFILE 't1.csv' INTO TABLE t1; using ReadConn master
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
    int N=4;
    char str[1024];

    Test->read_env();
    Test->print_env();

    Test->connect_maxscale();
    Test->repl->connect();

    printf("Create t1\n"); fflush(stdout);
    create_t1(Test->conn_rwsplit);
    printf("Insert data into t1\n"); fflush(stdout);
    insert_into_t1(Test->conn_rwsplit, N);
    printf("Sleeping to let replication happen\n");fflush(stdout);
    sleep(30);


    sprintf(str, "ssh -i %s -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no %s@%s '%s rm /tmp/t*.csv; %s chmod 777 -R /tmp'", Test->repl->sshkey[0], Test->repl->access_user, Test->repl->IP[0], Test->repl->access_sudo, Test->repl->access_sudo);
    printf("%s\n", str);
    system(str);

    printf("Copying data from t1 to file...\n");fflush(stdout);
    printf("using RWSplit:\n");fflush(stdout);
    global_result += execute_query(Test->conn_rwsplit, (char *) "SELECT * INTO OUTFILE '/tmp/t1.csv' FROM t1;");
    printf("using ReadsConn master:\n");fflush(stdout);
    global_result += execute_query(Test->conn_master, (char *) "SELECT * INTO OUTFILE '/tmp/t2.csv' FROM t1;");
    printf("using ReadsConn slave:\n");fflush(stdout);
    global_result += execute_query(Test->conn_slave, (char *) "SELECT * INTO OUTFILE '/tmp/t3.csv' FROM t1;");

    printf("Copying t1.cvs from Maxscale machine:\n");fflush(stdout);
    sprintf(str, "scp -i %s -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no %s@%s:/tmp/t1.csv ./", Test->repl->sshkey[0], Test->repl->access_user, Test->repl->IP[0]);
    //sprintf(str, "scp -i %s -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no %s@%s:/tmp/t1.csv ./", Test->maxscale_sshkey, Test->access_user, Test->maxscale_IP);
    printf("%s\n", str);
    system(str);

    MYSQL *srv[2];

    srv[0] = Test->conn_rwsplit;
    srv[1] = Test->conn_master;
    for (int i=0; i<2; i++) {
        printf("Dropping t1 \n");fflush(stdout);
        global_result += execute_query(Test->conn_rwsplit, (char *) "DROP TABLE t1;");
        printf("Sleeping to let replication happen\n");fflush(stdout);
        sleep(100);
        printf("Create t1\n"); fflush(stdout);
        create_t1(Test->conn_rwsplit);
        printf("Loading data to t1 from file\n");fflush(stdout);
        global_result += execute_query(srv[i], (char *) "LOAD DATA LOCAL INFILE 't1.csv' INTO TABLE t1;");

        printf("Sleeping to let replication happen\n");fflush(stdout);
        sleep(100);
        printf("SELECT: rwsplitter\n");fflush(stdout);
        global_result += select_from_t1(Test->conn_rwsplit, N);
        printf("SELECT: master\n");fflush(stdout);
        global_result += select_from_t1(Test->conn_master, N);
        printf("SELECT: slave\n");fflush(stdout);
        global_result += select_from_t1(Test->conn_slave, N);
        printf("Sleeping to let replication happen\n");fflush(stdout);
        /*sleep(100);
        for (int i=0; i<Test->repl->N; i++) {
            printf("SELECT: directly from node %d\n", i);fflush(stdout);
            global_result += select_from_t1(Test->repl->nodes[i], N);
        }*/
    }



    Test->repl->close_connections();
    global_result += check_maxscale_alive();

    Test->copy_all_logs(); return(global_result);
}

