#pragma once

/*
 * Copyright (c) 2016 MariaDB Corporation Ab
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

#include <maxscale/cppdefs.hh>
#include <string>
#include <tr1/unordered_map>
#include <vector>

#include <maxscale/json_api.h>
#include <maxscale/monitor.h>
#include <maxscale/thread.h>

#include "utilities.hh"

/** Utility macro for printing both MXS_ERROR and json error */
#define PRINT_MXS_JSON_ERROR(err_out, format, ...)\
    do {\
       MXS_ERROR(format, ##__VA_ARGS__);\
       if (err_out)\
       {\
            *err_out = mxs_json_error_append(*err_out, format, ##__VA_ARGS__);\
       }\
    } while (false)

extern const int PORT_UNKNOWN;
extern const int64_t SERVER_ID_UNKNOWN;
class MariaDBMonitor;

typedef std::tr1::unordered_map<const MXS_MONITORED_SERVER*, MySqlServerInfo> ServerInfoMap;
typedef std::vector<MXS_MONITORED_SERVER*> ServerVector;
typedef std::vector<string> StringVector;

enum print_repl_warnings_t
{
    WARNINGS_ON,
    WARNINGS_OFF
};

// TODO: Most of following should be class methods
MySqlServerInfo* get_server_info(MariaDBMonitor* handle, const MXS_MONITORED_SERVER* db);
const MySqlServerInfo* get_server_info(const MariaDBMonitor* handle, const MXS_MONITORED_SERVER* db);

bool can_replicate_from(MariaDBMonitor* mon,
                               MXS_MONITORED_SERVER* slave, MySqlServerInfo* slave_info,
                               MXS_MONITORED_SERVER* master, MySqlServerInfo* master_info);
MySqlServerInfo* update_slave_info(MariaDBMonitor* mon, MXS_MONITORED_SERVER* server);
void print_redirect_errors(MXS_MONITORED_SERVER* first_server, const ServerVector& servers, json_t** err_out);
bool do_show_slave_status(MariaDBMonitor* mon, MySqlServerInfo* serv_info, MXS_MONITORED_SERVER* database);
string generate_master_gtid_wait_cmd(const Gtid& gtid, double timeout);
bool query_one_row(MXS_MONITORED_SERVER *database, const char* query, unsigned int expected_cols,
                   StringVector* output);
bool check_replication_settings(const MXS_MONITORED_SERVER* server, MySqlServerInfo* server_info,
                                print_repl_warnings_t print_warnings = WARNINGS_ON);

// MariaDB Monitor instance data
class MariaDBMonitor
{
private:
    MariaDBMonitor(const MariaDBMonitor&);
    MariaDBMonitor& operator = (const MariaDBMonitor&);
public:
    // TODO: Once done refactoring, see which of these can be moved to private.

    /**
     * Start the monitor instance and return the instance data, creating it if starting for the first time.
     * This function creates a thread to execute the monitoring.
     *
     * @param monitor General monitor data
     * @param params Configuration parameters
     * @return A pointer to MariaDBMonitor specific data.
     */
    static MariaDBMonitor* start_monitor(MXS_MONITOR *monitor, const MXS_CONFIG_PARAMETER* params);

    /**
     * Stop the monitor. Waits until monitor has stopped.
     */
    void stop_monitor();

    /**
     * Monitor a database with given server info.
     *
     * @param mon
     * @param database Database to monitor
     * @param serv_info Server info for database
     */
    void monitor_mysql_db(MXS_MONITORED_SERVER* database, MySqlServerInfo *serv_info);

    /**
     * Performs switchover for a simple topology (1 master, N slaves, no intermediate masters). If an
     * intermediate step fails, the cluster may be left without a master.
     *
     * @param err_out json object for error printing. Can be NULL.
     * @return True if successful. If false, the cluster can be in various situations depending on which step
     * failed. In practice, manual intervention is usually required on failure.
     */
    bool do_switchover(MXS_MONITORED_SERVER* current_master, MXS_MONITORED_SERVER* new_master,
                       json_t** err_out);

    /**
     * @brief Process possible failover event
     *
     * If a master failure has occurred and MaxScale is configured with failover functionality, this fuction
     * executes failover to select and promote a new master server. This function should be called immediately
     * after @c mon_process_state_changes.
     *
     * @param cluster_modified_out Set to true if modifying cluster
     * @return True on success, false on error
     */
    bool mon_process_failover(bool* cluster_modified_out);

    /**
     * Performs failover for a simple topology (1 master, N slaves, no intermediate masters).
     *
     * @param mon Server cluster monitor
     * @param err_out Json output
     * @return True if successful
     */
    bool do_failover(json_t** err_out);

    /**
     * Checks if a server is a possible rejoin candidate. A true result from this function is not yet sufficient
     * criteria and another call to can_replicate_from() should be made.
     *
     * @param server Server to check
     * @param master_info Master server info
     * @param output Error output. If NULL, no error is printed to log.
     * @return True, if server is a rejoin suspect.
     */
    bool server_is_rejoin_suspect(MXS_MONITORED_SERVER* server, MySqlServerInfo* master_info,
                                  json_t** output);

    /**
     * (Re)join given servers to the cluster. The servers in the array are assumed to be joinable.
     * Usually the list is created by get_joinable_servers().
     *
     * @param joinable_servers Which servers to rejoin
     * @return The number of servers successfully rejoined
     */
    uint32_t do_rejoin(const ServerVector& joinable_servers);

    /**
     * Check if the cluster is a valid rejoin target.
     *
     * @return True if master and gtid domain are known
     */
    bool cluster_can_be_joined();

    /**
     * Check that preconditions for a failover are met.
     *
    * @param mon Cluster monitor
    * @param error_out JSON error out
    * @return True if failover may proceed
    */
    bool failover_check(json_t** error_out);

    /**
     * Check if server is using gtid replication.
     *
     * @param mon_server Server to check
     * @param error_out Error output
     * @return True if using gtid-replication. False if not, or if server is not a slave or otherwise does
     * not have a gtid_IO_Pos.
     */
    bool uses_gtid(MXS_MONITORED_SERVER* mon_server, json_t** error_out);

    MXS_MONITOR* monitor;          /**< Generic monitor object */
    volatile int shutdown;         /**< Flag to shutdown the monitor thread.
                                    *   Accessed from multiple threads. */
    int status;                    /**< Monitor status. TODO: This should be in MXS_MONITOR */
    MXS_MONITORED_SERVER *master;  /**< Master server for MySQL Master/Slave replication */
    ServerInfoMap server_info;     /**< Contains server specific information */
    unsigned long id;              /**< Monitor ID */
    bool warn_set_standalone_master; /**< Log a warning when setting standalone master */

    // Values updated by monitor
    int64_t master_gtid_domain;    /**< Gtid domain currently used by the master */
    string external_master_host;   /**< External master host, for fail/switchover */
    int external_master_port;      /**< External master port */

    // Replication topology detection settings
    bool mysql51_replication;      /**< Use MySQL 5.1 replication */
    bool detectStaleMaster;        /**< Monitor flag for MySQL replication Stale Master detection */
    bool detectStaleSlave;         /**< Monitor flag for MySQL replication Stale Master detection */
    bool multimaster;              /**< Detect and handle multi-master topologies */
    bool ignore_external_masters;  /**< Ignore masters outside of the monitor configuration */
    bool detect_standalone_master; /**< If standalone master are detected */
    bool replicationHeartbeat;     /**< Monitor flag for MySQL replication heartbeat */

    // Failover, switchover and rejoin settings
    int failcount;                 /**< How many monitoring cycles master must be down before auto-failover
                                    *   begins */
    uint32_t failover_timeout;     /**< Timeout in seconds for the master failover */
    uint32_t switchover_timeout;   /**< Timeout in seconds for the master switchover */
    bool verify_master_failure;    /**< Whether master failure is verified via slaves */
    int master_failure_timeout;    /**< Master failure verification (via slaves) time in seconds */
    bool auto_failover;            /**< If automatic master failover is enabled */
    bool auto_rejoin;              /**< Attempt to start slave replication on standalone servers or servers
                                    *   replicating from the wrong master automatically. */
    ServerVector excluded_servers; /**< Servers banned for master promotion during auto-failover. */

    // Other settings
    string script;                 /**< Script to call when state changes occur on servers */
    uint64_t events;               /**< enabled events */
    bool allow_cluster_recovery;   /**< Allow failed servers to rejoin the cluster */

private:
    THREAD m_thread;               /**< Monitor thread */

    // Failover, switchover and rejoin settings
    string m_replication_user;     /**< Replication user for CHANGE MASTER TO-commands */
    string m_replication_password; /**< Replication password for CHANGE MASTER TO-commands */

    MariaDBMonitor(MXS_MONITOR* monitor_base);
    ~MariaDBMonitor();
    bool load_config_params(const MXS_CONFIG_PARAMETER* params);
    bool failover_wait_relay_log(MXS_MONITORED_SERVER* new_master, int seconds_remaining, json_t** err_out);
    bool switchover_demote_master(MXS_MONITORED_SERVER* current_master, MySqlServerInfo* info,
                                  json_t** err_out);
    bool switchover_wait_slaves_catchup(const ServerVector& slaves, const Gtid& gtid, int total_timeout,
                                        int read_timeout, json_t** err_out);
    bool switchover_wait_slave_catchup(MXS_MONITORED_SERVER* slave, const Gtid& gtid,
                                       int total_timeout, int read_timeout, json_t** err_out);
    bool wait_cluster_stabilization(MXS_MONITORED_SERVER* new_master, const ServerVector& slaves,
                                    int seconds_remaining);
    bool switchover_check_preferred_master(MXS_MONITORED_SERVER* preferred, json_t** err_out);
    bool promote_new_master(MXS_MONITORED_SERVER* new_master, json_t** err_out);
    MXS_MONITORED_SERVER* select_new_master(ServerVector* slaves_out, json_t** err_out);
    bool server_is_excluded(const MXS_MONITORED_SERVER* server);
    bool is_candidate_better(const MySqlServerInfo* current_best_info, const MySqlServerInfo* candidate_info);
    MySqlServerInfo* update_slave_info(MXS_MONITORED_SERVER* server);
    bool do_show_slave_status(MySqlServerInfo* serv_info, MXS_MONITORED_SERVER* database);
    bool update_replication_settings(MXS_MONITORED_SERVER *database, MySqlServerInfo* info);

public:
    // Following methods should be private, change it once refactoring is done.
    bool update_gtids(MXS_MONITORED_SERVER *database, MySqlServerInfo* info);
    string generate_change_master_cmd(const string& master_host, int master_port);
    int redirect_slaves(MXS_MONITORED_SERVER* new_master, const ServerVector& slaves,
                        ServerVector* redirected_slaves);
    bool set_replication_credentials(const MXS_CONFIG_PARAMETER* params);
    bool start_external_replication(MXS_MONITORED_SERVER* new_master, json_t** err_out);
    bool switchover_start_slave(MXS_MONITORED_SERVER* old_master, SERVER* new_master);
    bool redirect_one_slave(MXS_MONITORED_SERVER* slave, const char* change_cmd);
    bool get_joinable_servers(ServerVector* output);
    bool join_cluster(MXS_MONITORED_SERVER* server, const char* change_cmd);
    void set_master_heartbeat(MXS_MONITORED_SERVER *);
    void set_slave_heartbeat(MXS_MONITORED_SERVER *);
    void disable_setting(const char* setting);
};
