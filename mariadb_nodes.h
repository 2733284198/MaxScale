#ifndef MARIADB_NODES_H
#define MARIADB_NODES_H

/**
 * @file mariadb_nodes.h - backend nodes routines
 *
 * @verbatim
 * Revision History
 *
 * Date		Who		Description
 * 17/11/14	Timofey Turenko	Initial implementation
 *
 * @endverbatim
 */


#include "mariadb_func.h"

/**
 * @brief A class to handle backend nodes
 * Contains references up to 256 nodes, info about IP, port, ssh key, use name and password for each node
 * Node parameters should be defined in the enviromental variables in the follwing way:
 * prefix_N - N number of nodes in the setup
 * prefix_NNN - IP adress of the node (NNN 3 digits node index)
 * prefix_port_NNN - MariaDB port number of the node
 * prefix_User - User name to access backend setup (should have full access to 'test' DB with GRANT OPTION)
 * prefix_Password - Password to access backend setup
 */
class Mariadb_nodes
{
public:
    /**
     * @brief Constructor
     * @param pref  name of backend setup (like 'repl' or 'galera')
     */
    Mariadb_nodes(char * pref);
    /**
    * @brief  MYSQL structs for every backend node
    */
    MYSQL *nodes[256];
    /**
     * @brief  IP address strings for every backend node
     */
    char IP[256][16];
    /**
     * @brief  private IP address strings for every backend node (for AWS)
     */
    char IP_private[256][16];
    /**
     * @brief  MariaDB port for every backend node
     */
    int port[256];
    /**
     * @brief  Path to ssh key for every backend node
     */
    char sshkey[256][4096];
    /**
     * @brief Number of backend nodes
     */
    int N;
    /**
     * @brief   User name to access backend nodes
     */
    char user_name[256];
    /**
     * @brief   Password to access backend nodes
     */
    char password[256];
    int master;
    /**
     * @brief     name of backend setup (like 'repl' or 'galera')
     */
    char prefix[16];
    /**
     * @brief     command to kill node virtual machine
     */
    char kill_vm_command[256][4096];
    /**
     * @brief     command to start node virtual machine
     */
    char start_vm_command[256][4096];
    /**
    * @brief  Opens connctions to all backend nodes (to 'test' DB)
    * @return 0 in case of success
    */

    /**
     * @brief start_db_command Command to start DB server
     */
    char start_db_command[256][4096];

    /**
     * @brief stop_db_command Command to start DB server
     */
    char stop_db_command[256][4096];

    /**
     * @brief ssl if true ssl  will be used
     */
    int ssl;

    char access_user[256];

    /**
     * @brief access_sudo empty if sudo is not needed or "sudo " if sudo is needed.
     */
    char access_sudo[64];

    /**
     * @brief no_set_pos if set to true setup_binlog function do not set log position
     */
    bool no_set_pos;

    /**
     * @brief connect open connecxtions to all Maxscale services
     * @return 0  in case of success
     */
    int connect();
    /**
     * @brief  close connections which were previously opened by Connect()
     * @return
     */
    int close_connections();
    /**
     * @brief reads IP, Ports, sshkeys for every node from enviromental variables as well as number of nodes (N) and  User/Password
     * @return 0
     */
    int read_env();
    /**
     * @brief  prints all nodes information
     * @return 0
     */
    int print_env();

    int find_master();
    /**
     * @brief change_master set a new master node for Master/Slave setup
     * @param NewMaster index of new Master node
     * @param OldMaster index of current Master node
     * @return  0 in case of success
     */
    int change_master(int NewMaster, int OldMaster);

    /**
     * @brief stop_nodes stops mysqld on all nodes
     * @return  0 in case of success
     */
    int stop_nodes();

    /**
     * @brief stop_slaves isues 'stop slave;' to all nodes
     * @return  0 in case of success
     */
    int stop_slaves();

    /**
     * @brief kill_all_vm kills all VMs using kill_vm_command
     * @return  0 in case of success
     */
    int kill_all_vm();

    /**
     * @brief start_all_vm starts all VMs using start_vm_command
     * @return  0 in case of success
     */
    int start_all_vm();

    /**
     * @brief wait_all_vm waits until all nodes are available
     * @return  0 in case of success
     */
    int wait_all_vm();

    /**
     * @brief restart_all_vm kills and start again all VMs
     * @return  0 in case of success
     */
    int restart_all_vm();

    /**
     * @brief configures nodes and starts Master/Slave replication
     * @return  0 in case of success
     */
    int start_replication();

    /**
     * @brief configures nodes and starts Galera cluster
     * @return  0 in case of success
     */
    int start_galera();

    /**
     * @brif BlockNode setup firewall on a backend node to block MariaDB port
     * @param node Index of node to block
     * @return 0 in case of success
     */
    int block_node(int node);

    /**
     * @brief UnblockNode setup firewall on a backend node to unblock MariaDB port
     * @param node Index of node to unblock
     * @return 0 in case of success
     */
    int unblock_node(int node);

    /**
     * @brief Check if all nodes are avaliable (via ssh)
     * @return 0 if everything is ok
     */
    int check_nodes();

    /**
     * @brief Check if all slaves have "Slave_IO_Running" set to "Yes" and master has N-1 slaves
     * @param master Index of master node
     * @return 0 if everything is ok
     */
    int check_replication(int master);

    /**
     * @brief Check if all nodes report wsrep_cluster_size equal to N
     * @return 0 if everything is ok
     */
    int check_galera();

    /**
     * @brief executes 'CHANGE MASTER TO ..' and 'START SLAVE'
     * @param MYSQL conn struct of slave node
     * @param master_host IP address of master node
     * @param master_port port of master node
     * @param log_file name of log file
     * @param log_pos initial position
     * @return 0 if everything is ok
     */
    int set_slave(MYSQL * conn, char master_host[], int master_port, char log_file[], char log_pos[]);

    /**
     * @brief Creates 'repl' user on all nodes
     * @return 0 if everything is ok
     */
    int set_repl_user();
};

#endif // MARIADB_NODES_H
