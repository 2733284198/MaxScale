/**
 * @file bug718.cpp bug718 regression case
 *
 */


#include <my_config.h>
#include <iostream>
#include <unistd.h>
#include "testconnections.h"
#include "sql_t1.h"
#include "maxadmin_operations.h"

using namespace std;

TestConnections * Test;
void *thread1( void *ptr );
void *thread2( void *ptr );

int db1_num = 0;
int main(int argc, char *argv[])
{
    Test = new TestConnections(argc, argv);
    int global_result = 0;
    int i;

    Test->print_env();

    execute_maxadmin_command(Test->maxscale_IP, (char *) "admin", Test->maxadmin_password, (char *) "set server server1 master");
    execute_maxadmin_command(Test->maxscale_IP, (char *) "admin", Test->maxadmin_password, (char *) "set server server2 slave");
    execute_maxadmin_command(Test->maxscale_IP, (char *) "admin", Test->maxadmin_password, (char *) "set server server3 slave");
    execute_maxadmin_command(Test->maxscale_IP, (char *) "admin", Test->maxadmin_password, (char *) "set server server4 slave");

    execute_maxadmin_command(Test->maxscale_IP, (char *) "admin", Test->maxadmin_password, (char *) "set server g_server1 master");
    execute_maxadmin_command(Test->maxscale_IP, (char *) "admin", Test->maxadmin_password, (char *) "set server g_server2 slave");
    execute_maxadmin_command(Test->maxscale_IP, (char *) "admin", Test->maxadmin_password, (char *) "set server g_server3 slave");
    execute_maxadmin_command(Test->maxscale_IP, (char *) "admin", Test->maxadmin_password, (char *) "set server g_server4 slave");

    printf("Connecting to all MaxScale services\n"); fflush(stdout);
    global_result += Test->connect_maxscale();

    //MYSQL * galera_rwsplit = open_conn(4016, Test->Maxscale_IP, Test->Maxscale_User, Test->Maxscale_Password);

    printf("executing show status 1000 times\n"); fflush(stdout);

    int ThreadsNum = 25;
    pthread_t thread_v1[ThreadsNum];

    int iret1[ThreadsNum];

    for (i = 0; i < ThreadsNum; i ++) { iret1[i] = pthread_create( &thread_v1[i], NULL, thread1, NULL); }

    create_t1(Test->conn_rwsplit);
    for (i = 0; i < 10000; i++) {
        insert_into_t1(Test->conn_rwsplit, 4);
        printf("i=%d\n", i);
    }

    for (i = 0; i < ThreadsNum; i ++) { pthread_join( thread_v1[i], NULL); }

    Test->close_maxscale_connections();
   Test->check_maxscale_alive();

    Test->copy_all_logs(); return(global_result);
}

void *thread1( void *ptr )
{
    MYSQL * conn = open_conn(Test->rwsplit_port , Test->maxscale_IP, Test->maxscale_user, Test->maxscale_password, Test->ssl);
    MYSQL * g_conn = open_conn(4016 , Test->maxscale_IP, Test->maxscale_user, Test->maxscale_password, Test->ssl);
    char sql[1034];
    sprintf(sql, "CREATE DATABASE IF NOT EXISTS test%d; USE test%d", db1_num, db1_num);
    execute_query(conn, sql);
    create_t1(conn);
    create_t1(g_conn);
    for (int i = 0; i < 10000; i++) {
        insert_into_t1(conn, 4);
        insert_into_t1(g_conn, 4);
        if ((i / 100) * 100 == i) {
            printf("Iteration %d\n", i); fflush(stdout);
        }
    }
    return NULL;
}

