/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2020-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "mariadbmon.hh"
#include <inttypes.h>
#include <maxscale/modutil.h>
#include <maxscale/mysql_utils.h>

static int add_slave_to_master(long *slaves_list, int list_size, long node_id);
static bool check_replicate_ignore_table(MXS_MONITORED_SERVER* database);
static bool check_replicate_do_table(MXS_MONITORED_SERVER* database);
static bool check_replicate_wild_do_table(MXS_MONITORED_SERVER* database);
static bool check_replicate_wild_ignore_table(MXS_MONITORED_SERVER* database);

static const char HB_TABLE_NAME[] = "maxscale_schema.replication_heartbeat";
static bool report_version_err = true;

/**
 * Build the replication tree for a MySQL 5.1 cluster
 *
 * This function queries each server with SHOW SLAVE HOSTS to determine which servers have slaves replicating
 * from them.
 *
 * @return Lowest server ID master in the monitor
 */
MXS_MONITORED_SERVER* MariaDBMonitor::build_mysql51_replication_tree()
{
    /** Column positions for SHOW SLAVE HOSTS */
    const size_t SLAVE_HOSTS_SERVER_ID = 0;
    const size_t SLAVE_HOSTS_HOSTNAME = 1;
    const size_t SLAVE_HOSTS_PORT = 2;

    MXS_MONITORED_SERVER* database = m_monitor_base->monitored_servers;
    MXS_MONITORED_SERVER *ptr, *rval = NULL;
    int i;

    while (database)
    {
        bool ismaster = false;
        MYSQL_RES* result;
        MYSQL_ROW row;
        int nslaves = 0;
        if (database->con)
        {
            if (mxs_mysql_query(database->con, "SHOW SLAVE HOSTS") == 0
                && (result = mysql_store_result(database->con)) != NULL)
            {
                if (mysql_field_count(database->con) < 4)
                {
                    mysql_free_result(result);
                    MXS_ERROR("\"SHOW SLAVE HOSTS\" "
                              "returned less than the expected amount of columns. "
                              "Expected 4 columns.");
                    return NULL;
                }

                if (mysql_num_rows(result) > 0)
                {
                    ismaster = true;
                    while (nslaves < MAX_NUM_SLAVES && (row = mysql_fetch_row(result)))
                    {
                        /* get Slave_IO_Running and Slave_SQL_Running values*/
                        database->server->slaves[nslaves] = atol(row[SLAVE_HOSTS_SERVER_ID]);
                        nslaves++;
                        MXS_DEBUG("Found slave at %s:%s", row[SLAVE_HOSTS_HOSTNAME], row[SLAVE_HOSTS_PORT]);
                    }
                    database->server->slaves[nslaves] = 0;
                }

                mysql_free_result(result);
            }
            else
            {
                mon_report_query_error(database);
            }

            /* Set the Slave Role */
            if (ismaster)
            {
                m_master = get_server_info(database);

                MXS_DEBUG("Master server found at [%s]:%d with %d slaves",
                          database->server->name,
                          database->server->port,
                          nslaves);

                monitor_set_pending_status(database, SERVER_MASTER);
                database->server->depth = 0; // Add Depth 0 for Master

                if (rval == NULL || rval->server->node_id > database->server->node_id)
                {
                    rval = database;
                }
            }
        }
        database = database->next;
    }

    database = m_monitor_base->monitored_servers;

    /** Set master server IDs */
    while (database)
    {
        ptr = m_monitor_base->monitored_servers;

        while (ptr)
        {
            for (i = 0; ptr->server->slaves[i]; i++)
            {
                if (ptr->server->slaves[i] == database->server->node_id)
                {
                    database->server->master_id = ptr->server->node_id;
                    database->server->depth = 1; // Add Depth 1 for Slave
                    break;
                }
            }
            ptr = ptr->next;
        }
        if (SERVER_IS_SLAVE(database->server) &&
            (database->server->master_id <= 0 ||
             database->server->master_id != m_master->m_server_base->server->node_id))
        {

            monitor_set_pending_status(database, SERVER_SLAVE);
            monitor_set_pending_status(database, SERVER_SLAVE_OF_EXTERNAL_MASTER);
        }
        database = database->next;
    }
    return rval;
}

/**
 * This function computes the replication tree from a set of monitored servers and returns the root server
 * with SERVER_MASTER bit. The tree is computed even for servers in 'maintenance' mode.
 *
 * @return The server at root level with SERVER_MASTER bit
 */
MXS_MONITORED_SERVER* MariaDBMonitor::get_replication_tree()
{
    const int num_servers = m_servers.size();
    MXS_MONITORED_SERVER *ptr;
    MXS_MONITORED_SERVER *backend;
    SERVER *current;
    int depth = 0;
    long node_id;
    int root_level;

    ptr = m_monitor_base->monitored_servers;
    root_level = num_servers;

    while (ptr)
    {
        /* The server could be in SERVER_IN_MAINT
         * that means SERVER_IS_RUNNING returns 0
         * Let's check only for SERVER_IS_DOWN: server is not running
         */
        if (SERVER_IS_DOWN(ptr->server))
        {
            ptr = ptr->next;
            continue;
        }
        depth = 0;
        current = ptr->server;

        node_id = current->master_id;

        /** Either this node doesn't replicate from a master or the master
         * where it replicates from is not configured to this monitor. */
        if (node_id < 1 ||
            getServerByNodeId(node_id) == NULL)
        {
            MXS_MONITORED_SERVER *find_slave;
            find_slave = getSlaveOfNodeId(current->node_id, ACCEPT_DOWN);

            if (find_slave == NULL)
            {
                current->depth = -1;
                ptr = ptr->next;

                continue;
            }
            else
            {
                current->depth = 0;
            }
        }
        else
        {
            depth++;
        }

        while (depth <= num_servers)
        {
            /* set the root master at lowest depth level */
            if (current->depth > -1 && current->depth < root_level)
            {
                root_level = current->depth;
                m_master = get_server_info(ptr);
            }
            backend = getServerByNodeId(node_id);

            if (backend)
            {
                node_id = backend->server->master_id;
            }
            else
            {
                node_id = -1;
            }

            if (node_id > 0)
            {
                current->depth = depth + 1;
                depth++;

            }
            else
            {
                MXS_MONITORED_SERVER *master_cand;
                current->depth = depth;

                master_cand = getServerByNodeId(current->master_id);
                if (master_cand && master_cand->server && master_cand->server->node_id > 0)
                {
                    add_slave_to_master(master_cand->server->slaves, sizeof(master_cand->server->slaves),
                                        current->node_id);
                    master_cand->server->depth = current->depth - 1;

                    if (m_master && master_cand->server->depth < m_master->m_server_base->server->depth)
                    {
                        /** A master with a lower depth was found, remove
                            the master status from the previous master. */
                        monitor_clear_pending_status(m_master->m_server_base, SERVER_MASTER);
                        m_master = get_server_info(master_cand);
                    }

                    MariaDBServer* info = get_server_info(master_cand);

                    if (SERVER_IS_RUNNING(master_cand->server))
                    {
                        /** Only set the Master status if read_only is disabled */
                        monitor_set_pending_status(master_cand,
                                                   info->m_read_only ? SERVER_SLAVE : SERVER_MASTER);
                    }
                }
                else
                {
                    if (current->master_id > 0)
                    {
                        monitor_set_pending_status(ptr, SERVER_SLAVE);
                        monitor_set_pending_status(ptr, SERVER_SLAVE_OF_EXTERNAL_MASTER);
                    }
                }
                break;
            }

        }

        ptr = ptr->next;
    }

    /*
     * Return the root master
     */

    if (m_master != NULL)
    {
        /* If the root master is in MAINT, return NULL */
        if (SERVER_IN_MAINT(m_master->m_server_base->server))
        {
            return NULL;
        }
        else
        {
            return m_master->m_server_base;
        }
    }
    else
    {
        return NULL;
    }
}

/*******
 * This function add a slave id into the slaves server field
 * of its master server
 *
 * @param slaves_list   The slave list array of the master server
 * @param list_size     The size of the slave list
 * @param node_id       The node_id of the slave to be inserted
 * @return      1 for inserted value and 0 otherwise
 */
static int add_slave_to_master(long *slaves_list, int list_size, long node_id)
{
    for (int i = 0; i < list_size; i++)
    {
        if (slaves_list[i] == 0)
        {
            slaves_list[i] = node_id;
            return 1;
        }
    }
    return 0;
}

/**
 * Fetch a node by node_id
 *
 * @param node_id The server_id to fetch
 *
 * @return The server with the required server_id
 */
MXS_MONITORED_SERVER* MariaDBMonitor::getServerByNodeId(long node_id)
{
    SERVER *current;
    MXS_MONITORED_SERVER *ptr = m_monitor_base->monitored_servers;
    while (ptr)
    {
        current = ptr->server;
        if (current->node_id == node_id)
        {
            return ptr;
        }
        ptr = ptr->next;
    }
    return NULL;
}

/**
 * Fetch a slave node from a node_id
 *
 * @param node_id            The server_id to fetch
 * @param slave_down_setting Whether to accept or reject slaves which are down
 * @return                   The slave server of this node_id
 */
MXS_MONITORED_SERVER* MariaDBMonitor::getSlaveOfNodeId(long node_id, slave_down_setting_t slave_down_setting)
{
    MXS_MONITORED_SERVER *ptr = m_monitor_base->monitored_servers;
    SERVER *current;
    while (ptr)
    {
        current = ptr->server;
        if (current->master_id == node_id && (slave_down_setting == ACCEPT_DOWN || !SERVER_IS_DOWN(current)))
        {
            return ptr;
        }
        ptr = ptr->next;
    }
    return NULL;
}

/**
 * @brief A node in a graph
 */
struct graph_node
{
    int index;
    int lowest_index;
    int cycle;
    bool active;
    struct graph_node *parent;
    MariaDBServer *info;
    MXS_MONITORED_SERVER *db;
};

/**
 * @brief Visit a node in the graph
 *
 * This function is the main function used to determine whether the node is a
 * part of a cycle. It is an implementation of the Tarjan's strongly connected
 * component algorithm. All one node cycles are ignored since normal
 * master-slave monitoring handles that.
 *
 * Tarjan's strongly connected component algorithm:
 *
 *     https://en.wikipedia.org/wiki/Tarjan%27s_strongly_connected_components_algorithm
 */
static void visit_node(struct graph_node *node, struct graph_node **stack,
                       int *stacksize, int *index, int *cycle)
{
    /** Assign an index to this node */
    node->lowest_index = node->index = *index;
    node->active = true;
    *index += 1;

    stack[*stacksize] = node;
    *stacksize += 1;

    if (node->parent == NULL)
    {
        /** This node does not connect to another node, it can't be a part of a cycle */
        node->lowest_index = -1;
    }
    else if (node->parent->index == 0)
    {
        /** Node has not been visited */
        visit_node(node->parent, stack, stacksize, index, cycle);

        if (node->parent->lowest_index < node->lowest_index)
        {
            /** The parent connects to a node with a lower index, this node
                could be a part of a cycle. */
            node->lowest_index = node->parent->lowest_index;
        }
    }
    else if (node->parent->active)
    {
        /** This node could be a root node of the cycle */
        if (node->parent->index < node->lowest_index)
        {
            /** Root node found */
            node->lowest_index = node->parent->index;
        }
    }
    else
    {
        /** Node connects to an already connected cycle, it can't be a part of it */
        node->lowest_index = -1;
    }

    if (node->active && node->parent && node->lowest_index > 0)
    {
        if (node->lowest_index == node->index &&
            node->lowest_index == node->parent->lowest_index)
        {
            /**
             * Found a multi-node cycle from the graph. The cycle is formed from the
             * nodes with a lowest_index value equal to the lowest_index value of the
             * current node. Rest of the nodes on the stack are not part of a cycle
             * and can be discarded.
             */

            *cycle += 1;

            while (*stacksize > 0)
            {
                struct graph_node *top = stack[(*stacksize) - 1];
                top->active = false;

                if (top->lowest_index == node->lowest_index)
                {
                    top->cycle = *cycle;
                }
                *stacksize -= 1;
            }
        }
    }
    else
    {
        /** Pop invalid nodes off the stack */
        node->active = false;
        if (*stacksize > 0)
        {
            *stacksize -= 1;
        }
    }
}

/**
 * @brief Find the strongly connected components in the replication tree graph
 *
 * Each replication cluster is a directed graph made out of replication
 * trees. If this graph has strongly connected components (more generally
 * cycles), it is considered a multi-master cluster due to the fact that there
 * are multiple nodes where the data can originate.
 *
 * Detecting the cycles in the graph allows this monitor to better understand
 * the relationships between the nodes. All nodes that are a part of a cycle can
 * be labeled as master nodes. This information will later be used to choose the
 * right master where the writes should go.
 *
 * This function also populates the MYSQL_SERVER_INFO structures group
 * member. Nodes in a group get a positive group ID where the nodes not in a
 * group get a group ID of 0.
 */
void MariaDBMonitor::find_graph_cycles()
{
    const int nservers = m_servers.size();
    struct graph_node graph[nservers];
    struct graph_node *stack[nservers];
    int nodes = 0;

    for (MXS_MONITORED_SERVER *db = m_monitor_base->monitored_servers; db; db = db->next)
    {
        graph[nodes].info = get_server_info(db);
        graph[nodes].db = db;
        graph[nodes].index = graph[nodes].lowest_index = 0;
        graph[nodes].cycle = 0;
        graph[nodes].active = false;
        graph[nodes].parent = NULL;
        nodes++;
    }

    /** Build the graph */
    for (int i = 0; i < nservers; i++)
    {
        if (graph[i].info->m_slave_status.master_server_id > 0)
        {
            /** Found a connected node */
            for (int k = 0; k < nservers; k++)
            {
                if (graph[k].info->m_server_id == graph[i].info->m_slave_status.master_server_id)
                {
                    graph[i].parent = &graph[k];
                    break;
                }
            }
        }
    }

    int index = 1;
    int cycle = 0;
    int stacksize = 0;

    for (int i = 0; i < nservers; i++)
    {
        if (graph[i].index == 0)
        {
            /** Index is 0, this node has not yet been visited */
            visit_node(&graph[i], stack, &stacksize, &index, &cycle);
        }
    }

    for (int i = 0; i < nservers; i++)
    {
        graph[i].info->m_group = graph[i].cycle;

        if (graph[i].cycle > 0)
        {
            /** We have at least one cycle in the graph */
            if (graph[i].info->m_read_only)
            {
                monitor_set_pending_status(graph[i].db, SERVER_SLAVE | SERVER_STALE_SLAVE);
                monitor_clear_pending_status(graph[i].db, SERVER_MASTER);
            }
            else
            {
                monitor_set_pending_status(graph[i].db, SERVER_MASTER);
                monitor_clear_pending_status(graph[i].db, SERVER_SLAVE | SERVER_STALE_SLAVE);
            }
        }
        else if (m_detect_stale_master && cycle == 0 &&
                 graph[i].db->server->status & SERVER_MASTER &&
                 (graph[i].db->pending_status & SERVER_MASTER) == 0)
        {
            /**
             * Stale master detection is handled here for multi-master mode.
             *
             * If we know that no cycles were found from the graph and that a
             * server once had the master status, replication has broken
             * down. These masters are assigned the stale master status allowing
             * them to be used as masters even if they lose their slaves. A
             * slave in this case can be either a normal slave or another
             * master.
             */
            if (graph[i].info->m_read_only)
            {
                /** The master is in read-only mode, set it into Slave state */
                monitor_set_pending_status(graph[i].db, SERVER_SLAVE | SERVER_STALE_SLAVE);
                monitor_clear_pending_status(graph[i].db, SERVER_MASTER | SERVER_STALE_STATUS);
            }
            else
            {
                monitor_set_pending_status(graph[i].db, SERVER_MASTER | SERVER_STALE_STATUS);
                monitor_clear_pending_status(graph[i].db, SERVER_SLAVE | SERVER_STALE_SLAVE);
            }
        }
    }
}

/**
 * Monitor an individual server. TODO: this will likely end up as method of MariaDBServer class.
 *
 * @param database  The database to probe
 */
void MariaDBMonitor::monitor_database(MariaDBServer* serv_info)
{
    MXS_MONITORED_SERVER* database = serv_info->m_server_base;
    /* Don't probe servers in maintenance mode */
    if (SERVER_IN_MAINT(database->server))
    {
        return;
    }

    /** Store previous status */
    database->mon_prev_status = database->server->status;

    mxs_connect_result_t rval = mon_ping_or_connect_to_db(m_monitor_base, database);
    if (rval == MONITOR_CONN_OK)
    {
        server_clear_status_nolock(database->server, SERVER_AUTH_ERROR);
        monitor_clear_pending_status(database, SERVER_AUTH_ERROR);
    }
    else
    {
        /**
         * The current server is not running. Clear all but the stale master bit
         * as it is used to detect masters that went down but came up.
         */
        unsigned int all_bits = ~SERVER_STALE_STATUS;
        server_clear_status_nolock(database->server, all_bits);
        monitor_clear_pending_status(database, all_bits);

        if (mysql_errno(database->con) == ER_ACCESS_DENIED_ERROR)
        {
            server_set_status_nolock(database->server, SERVER_AUTH_ERROR);
            monitor_set_pending_status(database, SERVER_AUTH_ERROR);
        }

        /* Log connect failure only once */
        if (mon_status_changed(database) && mon_print_fail_status(database))
        {
            mon_log_connect_error(database, rval);
        }

        return;
    }

    /* Store current status in both server and monitor server pending struct */
    server_set_status_nolock(database->server, SERVER_RUNNING);
    monitor_set_pending_status(database, SERVER_RUNNING);

    /* Check whether current server is MaxScale Binlog Server */
    MYSQL_RES *result;
    if (mxs_mysql_query(database->con, "SELECT @@maxscale_version") == 0 &&
        (result = mysql_store_result(database->con)) != NULL)
    {
        serv_info->m_binlog_relay = true;
        mysql_free_result(result);
    }
    else
    {
        serv_info->m_binlog_relay = false;
    }

    /* Get server version string, also get/set numeric representation. */
    mxs_mysql_set_server_version(database->con, database->server);
    /* Set monitor version enum. */
    uint64_t version_num = server_get_version(database->server);
    if (version_num >= 100000)
    {
        serv_info->m_version = MYSQL_SERVER_VERSION_100;
    }
    else if (version_num >= 5 * 10000 + 5 * 100)
    {
        serv_info->m_version = MYSQL_SERVER_VERSION_55;
    }
    else
    {
        serv_info->m_version = MYSQL_SERVER_VERSION_51;
    }
    /* Query a few settings. */
    serv_info->read_server_variables();
    /* If gtid domain exists and server is 10.0, update gtid:s */
    if (m_master_gtid_domain >= 0 && serv_info->m_version == MYSQL_SERVER_VERSION_100)
    {
        serv_info->update_gtids();
    }
    /* Check for MariaDB 10.x.x and get status for multi-master replication */
    if (serv_info->m_version == MYSQL_SERVER_VERSION_100 || serv_info->m_version == MYSQL_SERVER_VERSION_55)
    {
        monitor_mysql_db(serv_info);
    }
    else
    {
        if (m_mysql51_replication)
        {
            monitor_mysql_db(serv_info);
        }
        else if (report_version_err)
        {
            report_version_err = false;
            MXS_ERROR("MySQL version is lower than 5.5 and 'mysql51_replication' option is "
                      "not enabled, replication tree cannot be resolved. To enable MySQL 5.1 replication "
                      "detection, add 'mysql51_replication=true' to the monitor section.");
        }
    }
}

/**
 * Monitor a database with given server info.
 *
 * @param serv_info Server info for database
 */
void MariaDBMonitor::monitor_mysql_db(MariaDBServer* serv_info)
{
    MXS_MONITORED_SERVER* database = serv_info->m_server_base;
    /** Clear old states */
    monitor_clear_pending_status(database, SERVER_SLAVE | SERVER_MASTER | SERVER_RELAY_MASTER |
                                 SERVER_SLAVE_OF_EXTERNAL_MASTER);

    if (serv_info->do_show_slave_status())
    {
        /* If all configured slaves are running set this node as slave */
        if (serv_info->m_slave_configured && serv_info->m_n_slaves_running > 0 &&
            serv_info->m_n_slaves_running == serv_info->m_n_slaves_configured)
        {
            monitor_set_pending_status(database, SERVER_SLAVE);
        }

        /** Store master_id of current node. For MySQL 5.1 it will be set at a later point. */
        database->server->master_id = serv_info->m_slave_status.master_server_id;
    }
}

/**
 * Check if the maxscale_schema.replication_heartbeat table is replicated on all
 * servers and log a warning if problems were found.
 *
 * @param monitor Monitor structure
 */
void MariaDBMonitor::check_maxscale_schema_replication()
{
    MXS_MONITORED_SERVER* database = m_monitor_base->monitored_servers;
    bool err = false;

    while (database)
    {
        mxs_connect_result_t rval = mon_ping_or_connect_to_db(m_monitor_base, database);
        if (rval == MONITOR_CONN_OK)
        {
            if (!check_replicate_ignore_table(database) ||
                !check_replicate_do_table(database) ||
                !check_replicate_wild_do_table(database) ||
                !check_replicate_wild_ignore_table(database))
            {
                err = true;
            }
        }
        else
        {
            mon_log_connect_error(database, rval);
        }
        database = database->next;
    }

    if (err)
    {
        MXS_WARNING("Problems were encountered when checking if '%s' is replicated. Make sure that "
                    "the table is replicated to all slaves.", HB_TABLE_NAME);
    }
}

/**
 * Check if replicate_ignore_table is defined and if maxscale_schema.replication_hearbeat
 * table is in the list.
 * @param database Server to check
 * @return False if the table is not replicated or an error occurred when querying
 * the server
 */
static bool check_replicate_ignore_table(MXS_MONITORED_SERVER* database)
{
    MYSQL_RES *result;
    bool rval = true;

    if (mxs_mysql_query(database->con,
                        "show variables like 'replicate_ignore_table'") == 0 &&
        (result = mysql_store_result(database->con)) &&
        mysql_num_fields(result) > 1)
    {
        MYSQL_ROW row;

        while ((row = mysql_fetch_row(result)))
        {
            if (strlen(row[1]) > 0 &&
                strcasestr(row[1], HB_TABLE_NAME))
            {
                MXS_WARNING("'replicate_ignore_table' is "
                            "defined on server '%s' and '%s' was found in it. ",
                            database->server->unique_name, HB_TABLE_NAME);
                rval = false;
            }
        }

        mysql_free_result(result);
    }
    else
    {
        MXS_ERROR("Failed to query server %s for "
                  "'replicate_ignore_table': %s",
                  database->server->unique_name,
                  mysql_error(database->con));
        rval = false;
    }
    return rval;
}

/**
 * Check if replicate_do_table is defined and if maxscale_schema.replication_hearbeat
 * table is not in the list.
 * @param database Server to check
 * @return False if the table is not replicated or an error occurred when querying
 * the server
 */
static bool check_replicate_do_table(MXS_MONITORED_SERVER* database)
{
    MYSQL_RES *result;
    bool rval = true;

    if (mxs_mysql_query(database->con,
                        "show variables like 'replicate_do_table'") == 0 &&
        (result = mysql_store_result(database->con)) &&
        mysql_num_fields(result) > 1)
    {
        MYSQL_ROW row;

        while ((row = mysql_fetch_row(result)))
        {
            if (strlen(row[1]) > 0 &&
                strcasestr(row[1], HB_TABLE_NAME) == NULL)
            {
                MXS_WARNING("'replicate_do_table' is "
                            "defined on server '%s' and '%s' was not found in it. ",
                            database->server->unique_name, HB_TABLE_NAME);
                rval = false;
            }
        }
        mysql_free_result(result);
    }
    else
    {
        MXS_ERROR("Failed to query server %s for "
                  "'replicate_do_table': %s",
                  database->server->unique_name,
                  mysql_error(database->con));
        rval = false;
    }
    return rval;
}

/**
 * Check if replicate_wild_do_table is defined and if it doesn't match
 * maxscale_schema.replication_heartbeat.
 * @param database Database server
 * @return False if the table is not replicated or an error occurred when trying to
 * query the server.
 */
static bool check_replicate_wild_do_table(MXS_MONITORED_SERVER* database)
{
    MYSQL_RES *result;
    bool rval = true;

    if (mxs_mysql_query(database->con,
                        "show variables like 'replicate_wild_do_table'") == 0 &&
        (result = mysql_store_result(database->con)) &&
        mysql_num_fields(result) > 1)
    {
        MYSQL_ROW row;

        while ((row = mysql_fetch_row(result)))
        {
            if (strlen(row[1]) > 0)
            {
                mxs_pcre2_result_t rc = modutil_mysql_wildcard_match(row[1], HB_TABLE_NAME);
                if (rc == MXS_PCRE2_NOMATCH)
                {
                    MXS_WARNING("'replicate_wild_do_table' is "
                                "defined on server '%s' and '%s' does not match it. ",
                                database->server->unique_name,
                                HB_TABLE_NAME);
                    rval = false;
                }
            }
        }
        mysql_free_result(result);
    }
    else
    {
        MXS_ERROR("Failed to query server %s for "
                  "'replicate_wild_do_table': %s",
                  database->server->unique_name,
                  mysql_error(database->con));
        rval = false;
    }
    return rval;
}

/**
 * Check if replicate_wild_ignore_table is defined and if it matches
 * maxscale_schema.replication_heartbeat.
 * @param database Database server
 * @return False if the table is not replicated or an error occurred when trying to
 * query the server.
 */
static bool check_replicate_wild_ignore_table(MXS_MONITORED_SERVER* database)
{
    MYSQL_RES *result;
    bool rval = true;

    if (mxs_mysql_query(database->con,
                        "show variables like 'replicate_wild_ignore_table'") == 0 &&
        (result = mysql_store_result(database->con)) &&
        mysql_num_fields(result) > 1)
    {
        MYSQL_ROW row;

        while ((row = mysql_fetch_row(result)))
        {
            if (strlen(row[1]) > 0)
            {
                mxs_pcre2_result_t rc = modutil_mysql_wildcard_match(row[1], HB_TABLE_NAME);
                if (rc == MXS_PCRE2_MATCH)
                {
                    MXS_WARNING("'replicate_wild_ignore_table' is "
                                "defined on server '%s' and '%s' matches it. ",
                                database->server->unique_name,
                                HB_TABLE_NAME);
                    rval = false;
                }
            }
        }
        mysql_free_result(result);
    }
    else
    {
        MXS_ERROR("Failed to query server %s for "
                  "'replicate_wild_do_table': %s",
                  database->server->unique_name,
                  mysql_error(database->con));
        rval = false;
    }
    return rval;
}

/**
 * @brief Check whether standalone master conditions have been met
 *
 * This function checks whether all the conditions to use a standalone master are met. For this to happen,
 * only one server must be available and other servers must have passed the configured tolerance level of
 * failures.
 *
 * @return True if standalone master should be used
 */
bool MariaDBMonitor::standalone_master_required()
{
    int candidates = 0;
    for (auto iter = m_servers.begin(); iter != m_servers.end(); iter++)
    {
        MariaDBServer* server = *iter;
        if (server->is_running())
        {
            candidates++;
            if (server->m_read_only || server->m_slave_configured || candidates > 1)
            {
                return false;
            }
        }
        else if (server->m_server_base->mon_err_count < m_failcount)
        {
            return false;
        }
    }

    return candidates == 1;
}

/**
 * @brief Use standalone master
 *
 * This function assigns the last remaining server the master status and sets all other servers into
 * maintenance mode. By setting the servers into maintenance mode, we prevent any possible conflicts when
 * the failed servers come back up.
 *
 * @return True if standalone master was set
 */
bool MariaDBMonitor::set_standalone_master()
{
    bool rval = false;
    for (auto iter = m_servers.begin(); iter != m_servers.end(); iter++)
    {
        MariaDBServer* server = *iter;
        auto mon_server = server->m_server_base;
        if (server->is_running())
        {
            if (!server->is_master() && m_warn_set_standalone_master)
            {
                MXS_WARNING("Setting standalone master, server '%s' is now the master.%s",
                            server->name(), m_allow_cluster_recovery ? "" :
                            " All other servers are set into maintenance mode.");
                m_warn_set_standalone_master = false;
            }

            server_clear_set_status(mon_server->server, SERVER_SLAVE, SERVER_MASTER | SERVER_STALE_STATUS);
            monitor_set_pending_status(mon_server, SERVER_MASTER | SERVER_STALE_STATUS);
            monitor_clear_pending_status(mon_server, SERVER_SLAVE);
            m_master = server;
            rval = true;
        }
        else if (!m_allow_cluster_recovery)
        {
            server_set_status_nolock(mon_server->server, SERVER_MAINT);
            monitor_set_pending_status(mon_server, SERVER_MAINT);
        }
    }

    return rval;
}

/**
 * Monitor a server. Should be moved to the server class later on.
 *
 * @param server The server
 */
void MariaDBMonitor::monitor_one_server(MariaDBServer& server)
{
    MXS_MONITORED_SERVER* ptr = server.m_server_base;

    ptr->mon_prev_status = ptr->server->status;
    /* copy server status into monitor pending_status */
    ptr->pending_status = ptr->server->status;

    /* monitor current node */
    monitor_database(get_server_info(ptr));

    /* reset the slave list of current node */
    memset(&ptr->server->slaves, 0, sizeof(ptr->server->slaves));

    if (mon_status_changed(ptr))
    {
        if (SRV_MASTER_STATUS(ptr->mon_prev_status))
        {
            /** Master failed, can't recover */
            MXS_NOTICE("Server [%s]:%d lost the master status.",
                       ptr->server->name,
                       ptr->server->port);
        }
    }

    if (mon_status_changed(ptr))
    {
#if defined(SS_DEBUG)
        MXS_INFO("Backend server [%s]:%d state : %s",
                 ptr->server->name,
                 ptr->server->port,
                 STRSRVSTATUS(ptr->server));
#else
        MXS_DEBUG("Backend server [%s]:%d state : %s",
                  ptr->server->name,
                  ptr->server->port,
                  STRSRVSTATUS(ptr->server));
#endif
    }

    if (SERVER_IS_DOWN(ptr->server))
    {
        /** Increase this server'e error count */
        ptr->mon_err_count += 1;
    }
    else
    {
        /** Reset this server's error count */
        ptr->mon_err_count = 0;
    }
}

/**
 * Compute replication tree, find root master.
 *
 * @return Found master server or NULL
 */
MariaDBServer* MariaDBMonitor::find_root_master()
{
    MXS_MONITORED_SERVER* found_root_master = NULL;
    const int num_servers = m_servers.size();
    /* if only one server is configured, that's is Master */
    if (num_servers == 1)
    {
        auto mon_server = m_servers[0]->m_server_base;
        if (SERVER_IS_RUNNING(mon_server->server))
        {
            mon_server->server->depth = 0;
            /* status cleanup */
            monitor_clear_pending_status(mon_server, SERVER_SLAVE);
            /* master status set */
            monitor_set_pending_status(mon_server, SERVER_MASTER);

            mon_server->server->depth = 0;
            m_master = m_servers[0];
            found_root_master = mon_server;
        }
    }
    else
    {
        /* Compute the replication tree */
        if (m_mysql51_replication)
        {
            found_root_master = build_mysql51_replication_tree();
        }
        else
        {
            found_root_master = get_replication_tree();
        }
    }

    if (m_detect_multimaster && num_servers > 0)
    {
        /** Find all the master server cycles in the cluster graph. If
         multiple masters are found, the servers with the read_only
         variable set to ON will be assigned the slave status. */
        find_graph_cycles();
    }

    return found_root_master ? get_server_info(found_root_master) : NULL;
}

/**
 * Test if server is a relay master and assign status if yes.
 *
 * @param candidate The server to assign
 */
void MariaDBMonitor::assign_relay_master(MariaDBServer& candidate)
{
    MXS_MONITORED_SERVER* ptr = candidate.m_server_base;
    if (ptr->server->node_id > 0 && ptr->server->master_id > 0 &&
        getSlaveOfNodeId(ptr->server->node_id, REJECT_DOWN) &&
        getServerByNodeId(ptr->server->master_id) &&
        (!m_detect_multimaster || candidate.m_group == 0))
    {
        /** This server is both a slave and a master i.e. a relay master */
        monitor_set_pending_status(ptr, SERVER_RELAY_MASTER);
        monitor_clear_pending_status(ptr, SERVER_MASTER);
    }
}

/**
 * Update serve states of a single server
 *
 * @param db_server Server to update
 * @param root_master_server The current best master
 */
void MariaDBMonitor::update_server_states(MariaDBServer& db_server, MariaDBServer* root_master_server)
{
    MXS_MONITORED_SERVER* ptr = db_server.m_server_base;
    MXS_MONITORED_SERVER* root_master = root_master_server ? root_master_server->m_server_base : NULL;
    if (!SERVER_IN_MAINT(ptr->server))
    {
        MariaDBServer *serv_info = get_server_info(ptr);

        /** If "detect_stale_master" option is On, let's use the previous master.
         *
         * Multi-master mode detects the stale masters in find_graph_cycles().
         *
         * TODO: If a stale master goes down and comes back up, it loses
         * the master status. An adequate solution would be to promote
         * the stale master as a real master if it is the last running server.
         */
        if (m_detect_stale_master && root_master && !m_detect_multimaster &&
            (strcmp(ptr->server->name, root_master->server->name) == 0 &&
             ptr->server->port == root_master->server->port) &&
            (ptr->server->status & SERVER_MASTER) &&
            !(ptr->pending_status & SERVER_MASTER) &&
            !serv_info->m_read_only)
        {
            /**
             * In this case server->status will not be updated from pending_status
             * Set the STALE bit for this server in server struct
             */
            server_set_status_nolock(ptr->server, SERVER_STALE_STATUS | SERVER_MASTER);
            monitor_set_pending_status(ptr, SERVER_STALE_STATUS | SERVER_MASTER);

            /** Log the message only if the master server didn't have
             * the stale master bit set */
            if ((ptr->mon_prev_status & SERVER_STALE_STATUS) == 0)
            {
                MXS_WARNING("All slave servers under the current master "
                            "server have been lost. Assigning Stale Master"
                            " status to the old master server '%s' (%s:%i).",
                            ptr->server->unique_name, ptr->server->name,
                            ptr->server->port);
            }
        }

        if (m_detect_stale_slave)
        {
            unsigned int bits = SERVER_SLAVE | SERVER_RUNNING;

            if ((ptr->mon_prev_status & bits) == bits &&
                root_master && SERVER_IS_MASTER(root_master->server))
            {
                /** Slave with a running master, assign stale slave candidacy */
                if ((ptr->pending_status & bits) == bits)
                {
                    monitor_set_pending_status(ptr, SERVER_STALE_SLAVE);
                }
                /** Server lost slave when a master is available, remove
                 * stale slave candidacy */
                else if ((ptr->pending_status & bits) == SERVER_RUNNING)
                {
                    monitor_clear_pending_status(ptr, SERVER_STALE_SLAVE);
                }
            }
            /** If this server was a stale slave candidate, assign
             * slave status to it */
            else if (ptr->mon_prev_status & SERVER_STALE_SLAVE &&
                     ptr->pending_status & SERVER_RUNNING &&
                     // Master is down
                     (!root_master || !SERVER_IS_MASTER(root_master->server) ||
                      // Master just came up
                      (SERVER_IS_MASTER(root_master->server) &&
                       (root_master->mon_prev_status & SERVER_MASTER) == 0)))
            {
                monitor_set_pending_status(ptr, SERVER_SLAVE);
            }
            else if (root_master == NULL && serv_info->m_slave_configured)
            {
                monitor_set_pending_status(ptr, SERVER_SLAVE);
            }
        }

        ptr->server->status = ptr->pending_status;
    }
}
