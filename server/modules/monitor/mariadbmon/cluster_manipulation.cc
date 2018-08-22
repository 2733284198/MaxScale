/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2022-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "mariadbmon.hh"

#include <inttypes.h>
#include <sstream>
#include <maxscale/clock.h>
#include <maxscale/mysql_utils.h>
#include <maxscale/utils.hh>

using std::string;
using maxscale::string_printf;

static const char RE_ENABLE_FMT[] = "To re-enable automatic %s, manually set '%s' to 'true' "
                                    "for monitor '%s' via MaxAdmin or the REST API, or restart MaxScale.";

static void print_redirect_errors(MariaDBServer* first_server, const ServerArray& servers,
                                  json_t** err_out);

/**
 * Run a manual switchover, promoting a new master server and demoting the existing master.
 *
 * @param promotion_server The server which should be promoted. If null, monitor will autoselect.
 * @param demotion_server The server which should be demoted. Can be null for autoselect, in which case
 * monitor will select the cluster master server. Otherwise must be a valid master server or a relay.
 * @param error_out Error output
 * @return True, if switchover was performed successfully
 */
bool MariaDBMonitor::manual_switchover(SERVER* promotion_server, SERVER* demotion_server, json_t** error_out)
{
    /* The server parameters may be null, in which case the monitor will autoselect.
     *
     * Manual commands (as well as automatic ones) are ran at the end of a normal monitor loop,
     * so server states can be assumed to be up-to-date.
     */
    bool switchover_done = false;
    MariaDBServer* promotion_target = NULL;
    MariaDBServer* demotion_target = NULL;

    auto ok_to_switch = switchover_prepare(promotion_server, demotion_server, Log::ON,
                                           &promotion_target, &demotion_target,
                                           error_out);
    if (ok_to_switch)
    {
        switchover_done = do_switchover(demotion_target, promotion_target, error_out);
        if (switchover_done)
        {
            MXS_NOTICE("Switchover '%s' -> '%s' performed.",
                       demotion_target->name(), promotion_target->name());
        }
        else
        {
            string msg = string_printf("Switchover %s -> %s failed",
                                       demotion_target->name(), promotion_target->name());
            bool failover_setting = config_get_bool(m_monitor->parameters, CN_AUTO_FAILOVER);
            if (failover_setting)
            {
                disable_setting(CN_AUTO_FAILOVER);
                msg += ", automatic failover has been disabled";
            }
            msg += ".";
            PRINT_MXS_JSON_ERROR(error_out, "%s", msg.c_str());
        }
    }
    else
    {
        PRINT_MXS_JSON_ERROR(error_out, "Switchover cancelled.");
    }
    return switchover_done;
}

bool MariaDBMonitor::manual_failover(json_t** output)
{
    bool failover_done = false;
    MariaDBServer* promotion_target = NULL;
    MariaDBServer* demotion_target = NULL;

    bool ok_to_failover = failover_prepare(Log::ON, &promotion_target, &demotion_target, output);
    if (ok_to_failover)
    {
        failover_done = do_failover(promotion_target, demotion_target, output);
        if (failover_done)
        {
            MXS_NOTICE("Failover '%s' -> '%s' performed.",
                       demotion_target->name(), promotion_target->name());
        }
        else
        {
            PRINT_MXS_JSON_ERROR(output, "Failover '%s' -> '%s' failed.",
                                 demotion_target->name(), promotion_target->name());
        }
    }
    else
    {
        PRINT_MXS_JSON_ERROR(output, "Failover cancelled.");
    }
    return failover_done;
}

bool MariaDBMonitor::manual_rejoin(SERVER* rejoin_server, json_t** output)
{
    bool rval = false;
    if (cluster_can_be_joined())
    {
        const char* rejoin_serv_name = rejoin_server->name;
        MXS_MONITORED_SERVER* mon_slave_cand = mon_get_monitored_server(m_monitor, rejoin_server);
        if (mon_slave_cand)
        {
            MariaDBServer* slave_cand = get_server_info(mon_slave_cand);

            if (server_is_rejoin_suspect(slave_cand, output))
            {
                if (m_master->update_gtids())
                {
                    string no_rejoin_reason;
                    if (slave_cand->can_replicate_from(m_master, &no_rejoin_reason))
                    {
                        ServerArray joinable_server;
                        joinable_server.push_back(slave_cand);
                        if (do_rejoin(joinable_server, output) == 1)
                        {
                            rval = true;
                            MXS_NOTICE("Rejoin performed.");
                        }
                        else
                        {
                            PRINT_MXS_JSON_ERROR(output, "Rejoin attempted but failed.");
                        }
                    }
                    else
                    {
                        PRINT_MXS_JSON_ERROR(output, "Server '%s' cannot replicate from cluster master '%s': "
                                             "%s.", rejoin_serv_name, m_master->name(),
                                             no_rejoin_reason.c_str());
                    }
                }
                else
                {
                    PRINT_MXS_JSON_ERROR(output, "Cluster master '%s' gtid info could not be updated.",
                                         m_master->name());
                }
            } // server_is_rejoin_suspect has added any error messages to the output, no need to print here
        }
        else
        {
            PRINT_MXS_JSON_ERROR(output, "The given server '%s' is not monitored by this monitor.",
                                 rejoin_serv_name);
        }
    }
    else
    {
        const char BAD_CLUSTER[] = "The server cluster of monitor '%s' is not in a state valid for joining. "
                                   "Either it has no master or its gtid domain is unknown.";
        PRINT_MXS_JSON_ERROR(output, BAD_CLUSTER, m_monitor->name);
    }

    return rval;
}

/**
 * Generate a CHANGE MASTER TO-query.
 *
 * @param master_host Master hostname/address
 * @param master_port Master port
 * @return Generated query
 */
string MariaDBMonitor::generate_change_master_cmd(const string& master_host, int master_port)
{
    std::stringstream change_cmd;
    change_cmd << "CHANGE MASTER TO MASTER_HOST = '" << master_host << "', ";
    change_cmd << "MASTER_PORT = " <<  master_port << ", ";
    change_cmd << "MASTER_USE_GTID = current_pos, ";
    change_cmd << "MASTER_USER = '" << m_replication_user << "', ";
    const char MASTER_PW[] = "MASTER_PASSWORD = '";
    const char END[] = "';";
#if defined(SS_DEBUG)
    std::stringstream change_cmd_nopw;
    change_cmd_nopw << change_cmd.str();
    change_cmd_nopw << MASTER_PW << "******" << END;;
    MXS_DEBUG("Change master command is '%s'.", change_cmd_nopw.str().c_str());
#endif
    change_cmd << MASTER_PW << m_replication_password << END;
    return change_cmd.str();
}

/**
 * Redirects slaves to replicate from another master server.
 *
 * @param new_master The replication master
 * @param slaves An array of slaves
 * @param redirected_slaves A vector where to insert successfully redirected slaves.
 * @return The number of slaves successfully redirected.
 */
int MariaDBMonitor::redirect_slaves(MariaDBServer* new_master, const ServerArray& slaves,
                                    ServerArray* redirected_slaves)
{
    mxb_assert(redirected_slaves != NULL);
    MXS_NOTICE("Redirecting slaves to new master.");
    string change_cmd = generate_change_master_cmd(new_master->m_server_base->server->address,
                                                   new_master->m_server_base->server->port);
    int successes = 0;
    for (MariaDBServer* slave : slaves)
    {
        if (slave->redirect_one_slave(change_cmd))
        {
            successes++;
            redirected_slaves->push_back(slave);
        }
    }
    return successes;
}

/**
 * Set the new master to replicate from the cluster external master.
 *
 * @param new_master The server being promoted
 * @param err_out Error output
 * @return True if new master accepted commands
 */
bool MariaDBMonitor::start_external_replication(MariaDBServer* new_master, json_t** err_out)
{
    bool rval = false;
    MYSQL* new_master_conn = new_master->m_server_base->con;
    string change_cmd = generate_change_master_cmd(m_external_master_host, m_external_master_port);
    if (mxs_mysql_query(new_master_conn, change_cmd.c_str()) == 0 &&
        mxs_mysql_query(new_master_conn, "START SLAVE;") == 0)
    {
        MXS_NOTICE("New master starting replication from external master %s:%d.",
                   m_external_master_host.c_str(), m_external_master_port);
        rval = true;
    }
    else
    {
        PRINT_MXS_JSON_ERROR(err_out, "Could not start replication from external master: '%s'.",
                             mysql_error(new_master_conn));
    }
    return rval;
}

/**
 * Starts a new slave connection on a server. Should be used on a demoted master server.
 *
 * @param old_master The server which will start replication
 * @param new_master Replication target
 * @return True if commands were accepted. This does not guarantee that replication proceeds
 * successfully.
 */
bool MariaDBMonitor::switchover_start_slave(MariaDBServer* old_master, MariaDBServer* new_master)
{
    bool rval = false;
    MYSQL* old_master_con = old_master->m_server_base->con;
    SERVER* new_master_server = new_master->m_server_base->server;

    string change_cmd = generate_change_master_cmd(new_master_server->address, new_master_server->port);
    if (mxs_mysql_query(old_master_con, change_cmd.c_str()) == 0 &&
        mxs_mysql_query(old_master_con, "START SLAVE;") == 0)
    {
        MXS_NOTICE("Old master '%s' starting replication from '%s'.",
                   old_master->name(), new_master->name());
        rval = true;
    }
    else
    {
        MXS_ERROR("Old master '%s' could not start replication: '%s'.",
                  old_master->name(), mysql_error(old_master_con));
    }
    return rval;
}

/**
 * (Re)join given servers to the cluster. The servers in the array are assumed to be joinable.
 * Usually the list is created by get_joinable_servers().
 *
 * @param joinable_servers Which servers to rejoin
 * @param output Error output. Can be null.
 * @return The number of servers successfully rejoined
 */
uint32_t MariaDBMonitor::do_rejoin(const ServerArray& joinable_servers, json_t** output)
{
    SERVER* master_server = m_master->m_server_base->server;
    const char* master_name = master_server->name;
    uint32_t servers_joined = 0;
    if (!joinable_servers.empty())
    {
        string change_cmd = generate_change_master_cmd(master_server->address, master_server->port);
        for (MariaDBServer* joinable : joinable_servers)
        {
            const char* name = joinable->name();
            bool op_success = false;

            if (joinable->m_slave_status.empty())
            {
                if (!m_demote_sql_file.empty() && !joinable->run_sql_from_file(m_demote_sql_file, output))
                {
                    PRINT_MXS_JSON_ERROR(output, "%s execution failed when attempting to rejoin server '%s'.",
                                         CN_DEMOTION_SQL_FILE, joinable->name());
                }
                else
                {
                    MXS_NOTICE("Directing standalone server '%s' to replicate from '%s'.", name, master_name);
                    op_success = joinable->join_cluster(change_cmd);
                }
            }
            else
            {
                MXS_NOTICE("Server '%s' is replicating from a server other than '%s', "
                           "redirecting it to '%s'.", name, master_name, master_name);
                op_success = joinable->redirect_one_slave(change_cmd);
            }

            if (op_success)
            {
                servers_joined++;
                m_cluster_modified = true;
            }
        }
    }
    return servers_joined;
}

/**
 * Check if the cluster is a valid rejoin target.
 *
 * @return True if master and gtid domain are known
 */
bool MariaDBMonitor::cluster_can_be_joined()
{
    return (m_master != NULL && m_master->is_master() && m_master_gtid_domain != GTID_DOMAIN_UNKNOWN);
}

/**
 * Scan the servers in the cluster and add (re)joinable servers to an array.
 *
 * @param mon Cluster monitor
 * @param output Array to save results to. Each element is a valid (re)joinable server according
 * to latest data.
 * @return False, if there were possible rejoinable servers but communications error to master server
 * prevented final checks.
 */
bool MariaDBMonitor::get_joinable_servers(ServerArray* output)
{
    mxb_assert(output);

    // Whether a join operation should be attempted or not depends on several criteria. Start with the ones
    // easiest to test. Go though all slaves and construct a preliminary list.
    ServerArray suspects;
    for (MariaDBServer* server : m_servers)
    {
        if (server_is_rejoin_suspect(server, NULL))
        {
            suspects.push_back(server);
        }
    }

    // Update Gtid of master for better info.
    bool comm_ok = true;
    if (!suspects.empty())
    {
        if (m_master->update_gtids())
        {
            for (size_t i = 0; i < suspects.size(); i++)
            {
                string rejoin_err_msg;
                if (suspects[i]->can_replicate_from(m_master, &rejoin_err_msg))
                {
                    output->push_back(suspects[i]);
                }
                else if (m_warn_cannot_rejoin)
                {
                    // Print a message explaining why an auto-rejoin is not done. Suppress printing.
                    MXS_WARNING("Automatic rejoin was not attempted on server '%s' even though it is a "
                                "valid candidate. Will keep retrying with this message suppressed for all "
                                "servers. Errors: \n%s",
                                suspects[i]->name(), rejoin_err_msg.c_str());
                    m_warn_cannot_rejoin = false;
                }
            }
        }
        else
        {
            comm_ok = false;
        }
    }
    else
    {
        m_warn_cannot_rejoin = true;
    }
    return comm_ok;
}

/**
 * Checks if a server is a possible rejoin candidate. A true result from this function is not yet sufficient
 * criteria and another call to can_replicate_from() should be made.
 *
 * @param rejoin_cand Server to check
 * @param output Error output. If NULL, no error is printed to log.
 * @return True, if server is a rejoin suspect.
 */
bool MariaDBMonitor::server_is_rejoin_suspect(MariaDBServer* rejoin_cand, json_t** output)
{
    bool is_suspect = false;
    if (rejoin_cand->is_usable() && !rejoin_cand->is_master())
    {
        // Has no slave connection, yet is not a master.
        if (rejoin_cand->m_slave_status.empty())
        {
            is_suspect = true;
        }
        // Or has existing slave connection ...
        else if (rejoin_cand->m_slave_status.size() == 1)
        {
            SlaveStatus* slave_status = &rejoin_cand->m_slave_status[0];
            // which is connected to master but it's the wrong one
            if (slave_status->slave_io_running == SlaveStatus::SLAVE_IO_YES  &&
                slave_status->master_server_id != m_master->m_server_id)
            {
                is_suspect = true;
            }
            // or is disconnected but master host or port is wrong.
            else if (slave_status->slave_io_running == SlaveStatus::SLAVE_IO_CONNECTING &&
                     slave_status->slave_sql_running &&
                     (slave_status->master_host != m_master->m_server_base->server->address ||
                      slave_status->master_port != m_master->m_server_base->server->port))
            {
                is_suspect = true;
            }
        }

        if (output != NULL && !is_suspect)
        {
            /* User has requested a manual rejoin but with a server which has multiple slave connections or
             * is already connected or trying to connect to the correct master. TODO: Slave IO stopped is
             * not yet handled perfectly. */
            if (rejoin_cand->m_slave_status.size() > 1)
            {
                const char MULTI_SLAVE[] = "Server '%s' has multiple slave connections, cannot rejoin.";
                PRINT_MXS_JSON_ERROR(output, MULTI_SLAVE, rejoin_cand->name());
            }
            else
            {
                const char CONNECTED[] = "Server '%s' is already connected or trying to connect to the "
                                         "correct master server.";
                PRINT_MXS_JSON_ERROR(output, CONNECTED, rejoin_cand->name());
            }
        }
    }
    else if (output != NULL)
    {
        PRINT_MXS_JSON_ERROR(output, "Server '%s' is master or not running.", rejoin_cand->name());
    }
    return is_suspect;
}

/**
 * Performs switchover for a simple topology (1 master, N slaves, no intermediate masters). If an
 * intermediate step fails, the cluster may be left without a master and manual intervention is
 * required to fix things.
 *
 * @param demotion_target Server to demote
 * @param promotion_target Server to promote
 * @param err_out json object for error printing. Can be NULL.
 * @return True if successful. If false, replication may be broken.
 */
bool MariaDBMonitor::do_switchover(MariaDBServer* demotion_target, MariaDBServer* promotion_target,
                                   json_t** err_out)
{
    mxb_assert(demotion_target && promotion_target);

    // Total time limit on how long this operation may take. Checked and modified after significant steps are
    // completed.
    int seconds_remaining = m_switchover_timeout;
    time_t start_time = time(NULL);

    // Step 1: Save all slaves except promotion target to an array.
    // Try to redirect even disconnected slaves.
    // TODO: 'switchover_wait_slaves_catchup' needs to be smarter and not bother with such slaves.
    ServerArray redirectable_slaves = get_redirectables(promotion_target, demotion_target);

    bool rval = false;
    // Step 2: Set read-only to on, flush logs, update master gtid:s
    if (switchover_demote_master(demotion_target, err_out))
    {
        m_cluster_modified = true;
        bool catchup_and_promote_success = false;
        time_t step2_time = time(NULL);
        seconds_remaining -= difftime(step2_time, start_time);

        // Step 3: Wait for the slaves (including promotion target) to catch up with master.
        ServerArray catchup_slaves = redirectable_slaves;
        catchup_slaves.push_back(promotion_target);
        if (switchover_wait_slaves_catchup(catchup_slaves, demotion_target->m_gtid_binlog_pos,
                                           seconds_remaining, err_out))
        {
            time_t step3_time = time(NULL);
            int seconds_step3 = difftime(step3_time, step2_time);
            MXS_DEBUG("Switchover: slave catchup took %d seconds.", seconds_step3);
            seconds_remaining -= seconds_step3;

            // Step 4: On new master STOP and RESET SLAVE, set read-only to off.
            if (promote_new_master(promotion_target, err_out))
            {
                catchup_and_promote_success = true;
                m_next_master = promotion_target;

                // Step 5: Redirect slaves and start replication on old master.
                ServerArray redirected_slaves;
                bool start_ok = switchover_start_slave(demotion_target, promotion_target);
                if (start_ok)
                {
                    redirected_slaves.push_back(demotion_target);
                }
                int redirects = redirect_slaves(promotion_target, redirectable_slaves, &redirected_slaves);

                bool success = redirectable_slaves.empty() ? start_ok : start_ok || redirects > 0;
                if (success)
                {
                    time_t step5_time = time(NULL);
                    seconds_remaining -= difftime(step5_time, step3_time);

                    // Step 6: Finally, add an event to the new master to advance gtid and wait for the slaves
                    // to receive it. If using external replication, skip this step. Come up with an
                    // alternative later.
                    if (m_external_master_port != PORT_UNKNOWN)
                    {
                        MXS_WARNING("Replicating from external master, skipping final check.");
                        rval = true;
                    }
                    else if (wait_cluster_stabilization(promotion_target, redirected_slaves,
                                                        seconds_remaining))
                    {
                        rval = true;
                        time_t step6_time = time(NULL);
                        int seconds_step6 = difftime(step6_time, step5_time);
                        seconds_remaining -= seconds_step6;
                        MXS_DEBUG("Switchover: slave replication confirmation took %d seconds with "
                                  "%d seconds to spare.", seconds_step6, seconds_remaining);
                    }
                }
                else
                {
                    print_redirect_errors(demotion_target, redirectable_slaves, err_out);
                }
            }
        }

        if (!catchup_and_promote_success)
        {
            // Step 3 or 4 failed, try to undo step 2.
            const char QUERY_UNDO[] = "SET GLOBAL read_only=0;";
            if (mxs_mysql_query(demotion_target->m_server_base->con, QUERY_UNDO) == 0)
            {
                PRINT_MXS_JSON_ERROR(err_out, "read_only disabled on server %s.", demotion_target->name());
            }
            else
            {
                PRINT_MXS_JSON_ERROR(err_out, "Could not disable read_only on server %s: '%s'.",
                                     demotion_target->name(),
                                     mysql_error(demotion_target->m_server_base->con));
            }

            // Try to reactivate external replication if any.
            if (m_external_master_port != PORT_UNKNOWN)
            {
                start_external_replication(promotion_target, err_out);
            }
        }
    }
    return rval;
}

/**
 * Performs failover for a simple topology (1 master, N slaves, no intermediate masters).
 *
 * @param demotion_target Server to demote
 * @param promotion_target Server to promote
 * @param err_out Error output
 * @return True if successful
 */
bool MariaDBMonitor::do_failover(MariaDBServer* promotion_target, MariaDBServer* demotion_target,
                                 json_t** error_out)
{
    // Total time limit on how long this operation may take. Checked and modified after significant steps are
    // completed.
    int seconds_remaining = m_failover_timeout;
    time_t start_time = time(NULL);
    // Step 1: Populate a vector with all slaves not the selected master.
    ServerArray redirectable_slaves = get_redirectables(promotion_target, demotion_target);

    time_t step1_time = time(NULL);
    seconds_remaining -= difftime(step1_time, start_time);

    bool rval = false;
    // Step 2: Wait until relay log consumed.
    if (promotion_target->failover_wait_relay_log(seconds_remaining, error_out))
    {
        time_t step2_time = time(NULL);
        int seconds_step2 = difftime(step2_time, step1_time);
        MXS_DEBUG("Failover: relay log processing took %d seconds.", seconds_step2);
        seconds_remaining -= seconds_step2;

        // Step 3: Stop and reset slave, set read-only to 0.
        if (promote_new_master(promotion_target, error_out))
        {
            m_next_master = promotion_target;
            m_cluster_modified = true;
            // Step 4: Redirect slaves.
            ServerArray redirected_slaves;
            int redirects = redirect_slaves(promotion_target, redirectable_slaves, &redirected_slaves);
            bool success = redirectable_slaves.empty() ? true : redirects > 0;
            if (success)
            {
                time_t step4_time = time(NULL);
                seconds_remaining -= difftime(step4_time, step2_time);

                // Step 5: Finally, add an event to the new master to advance gtid and wait for the slaves
                // to receive it. seconds_remaining can be 0 or less at this point. Even in such a case
                // wait_cluster_stabilization() may succeed if replication is fast enough. If using external
                // replication, skip this step. Come up with an alternative later.
                if (m_external_master_port != PORT_UNKNOWN)
                {
                    MXS_WARNING("Replicating from external master, skipping final check.");
                    rval = true;
                }
                else if (redirected_slaves.empty())
                {
                    // No slaves to check. Assume success.
                    rval = true;
                    MXS_DEBUG("Failover: no slaves to redirect, skipping stabilization check.");
                }
                else if (wait_cluster_stabilization(promotion_target, redirected_slaves, seconds_remaining))
                {
                    rval = true;
                    time_t step5_time = time(NULL);
                    int seconds_step5 = difftime(step5_time, step4_time);
                    seconds_remaining -= seconds_step5;
                    MXS_DEBUG("Failover: slave replication confirmation took %d seconds with "
                              "%d seconds to spare.", seconds_step5, seconds_remaining);
                }
            }
            else
            {
                print_redirect_errors(NULL, redirectable_slaves, error_out);
            }
        }
    }

    return rval;
}

/**
 * Demotes the current master server, preparing it for replicating from another server. This step can take a
 * while if long writes are running on the server.
 *
 * @param current_master Server to demote
 * @param info Current master info. Will be written to. TODO: Remove need for this.
 * @param err_out json object for error printing. Can be NULL.
 * @return True if successful.
 */
bool MariaDBMonitor::switchover_demote_master(MariaDBServer* current_master, json_t** err_out)
{
    MXS_NOTICE("Demoting server '%s'.", current_master->name());
    bool success = false;
    bool query_error = false;
    MYSQL* conn = current_master->m_server_base->con;
    const char* query = ""; // The next query to execute. Used also for error printing.
    // The presence of an external master changes several things.
    const bool external_master = server_is_slave_of_ext_master(current_master->m_server_base->server);

    if (external_master)
    {
        // First need to stop slave. read_only is probably on already, although not certain.
        query = "STOP SLAVE;";
        query_error = (mxs_mysql_query(conn, query) != 0);
        if (!query_error)
        {
            query = "RESET SLAVE ALL;";
            query_error = (mxs_mysql_query(conn, query) != 0);
        }
    }

    bool error_fetched = false;
    string error_desc;
    if (!query_error)
    {
        query = "SET GLOBAL read_only=1;";
        query_error = (mxs_mysql_query(conn, query) != 0);
        if (!query_error)
        {
            // If have external master, no writes are allowed so skip this step. It's not essential, just
            // adds one to gtid.
            if (!external_master)
            {
                query = "FLUSH TABLES;";
                query_error = (mxs_mysql_query(conn, query) != 0);
            }

            if (!query_error)
            {
                query = "FLUSH LOGS;";
                query_error = (mxs_mysql_query(conn, query) != 0);
                if (!query_error)
                {
                    query = "";
                    if (current_master->update_gtids())
                    {
                        success = true;
                    }
                }
            }

            if (!success)
            {
                // Somehow, a step after "SET read_only" failed. Try to set read_only back to 0. It may not
                // work since the connection is likely broken.
                error_desc = mysql_error(conn); // Read connection error before next step.
                error_fetched = true;
                mxs_mysql_query(conn, "SET GLOBAL read_only=0;");
            }
        }
    }

    if (query_error && !error_fetched)
    {
        error_desc = mysql_error(conn);
    }

    if (!success)
    {
        if (query_error)
        {
            if (error_desc.empty())
            {
                const char UNKNOWN_ERROR[] = "Demotion failed due to an unknown error when executing "
                                             "a query. Query: '%s'.";
                PRINT_MXS_JSON_ERROR(err_out, UNKNOWN_ERROR, query);
            }
            else
            {
                const char KNOWN_ERROR[] = "Demotion failed due to a query error: '%s'. Query: '%s'.";
                PRINT_MXS_JSON_ERROR(err_out, KNOWN_ERROR, error_desc.c_str(), query);
            }
        }
        else
        {
            const char * const GTID_ERROR = "Demotion failed due to an error in updating gtid:s. "
                                            "Check log for more details.";
            PRINT_MXS_JSON_ERROR(err_out, GTID_ERROR);
        }
    }
    else if (!m_demote_sql_file.empty() && !current_master->run_sql_from_file(m_demote_sql_file, err_out))
    {
        PRINT_MXS_JSON_ERROR(err_out, "%s execution failed when demoting server '%s'.",
                             CN_DEMOTION_SQL_FILE, current_master->name());
        success = false;
    }

    return success;
}

/**
 * Wait until slave replication catches up with the master gtid for all slaves in the vector.
 *
 * @param slave Slaves to wait on
 * @param gtid Which gtid must be reached
 * @param total_timeout Maximum wait time in seconds
 * @param err_out json object for error printing. Can be NULL.
 * @return True, if target gtid was reached within allotted time for all servers
 */
bool MariaDBMonitor::switchover_wait_slaves_catchup(const ServerArray& slaves, const GtidList& gtid,
                                                    int total_timeout, json_t** err_out)
{
    bool success = true;
    int seconds_remaining = total_timeout;

    for (auto iter = slaves.begin(); iter != slaves.end() && success; iter++)
    {
        if (seconds_remaining <= 0)
        {
            success = false;
        }
        else
        {
            time_t begin = time(NULL);
            MariaDBServer* slave_server = *iter;
            if (slave_server->wait_until_gtid(gtid, seconds_remaining, err_out))
            {
                seconds_remaining -= difftime(time(NULL), begin);
            }
            else
            {
                success = false;
            }
        }
    }
    return success;
}

/**
 * Send an event to new master and wait for slaves to get the event.
 *
 * @param new_master Where to send the event
 * @param slaves Servers to monitor
 * @param seconds_remaining How long can we wait
 * @return True, if at least one slave got the new event within the time limit
 */
bool MariaDBMonitor::wait_cluster_stabilization(MariaDBServer* new_master, const ServerArray& slaves,
                                                int seconds_remaining)
{
    mxb_assert(!slaves.empty());
    bool rval = false;
    time_t begin = time(NULL);

    if (mxs_mysql_query(new_master->m_server_base->con, "FLUSH TABLES;") == 0 &&
        new_master->update_gtids())
    {
        int query_fails = 0;
        int repl_fails = 0;
        int successes = 0;
        const GtidList& target = new_master->m_gtid_current_pos;
        ServerArray wait_list = slaves; // Check all the servers in the list
        bool first_round = true;
        bool time_is_up = false;

        while (!wait_list.empty() && !time_is_up)
        {
            if (!first_round)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            // Erasing elements from an array, so iterate from last to first
            int i = wait_list.size() - 1;
            while (i >= 0)
            {
                MariaDBServer* slave = wait_list[i];
                if (slave->update_gtids() && slave->do_show_slave_status() && !slave->m_slave_status.empty())
                {
                    if (!slave->m_slave_status[0].last_error.empty())
                    {
                        // IO or SQL error on slave, replication is a fail
                        MXS_WARNING("Slave '%s' cannot start replication: '%s'.", slave->name(),
                                    slave->m_slave_status[0].last_error.c_str());
                        wait_list.erase(wait_list.begin() + i);
                        repl_fails++;
                    }
                    else if (GtidList::events_ahead(target, slave->m_gtid_current_pos,
                                                    GtidList::MISSING_DOMAIN_IGNORE) == 0)
                    {
                        // This slave has reached the same gtid as master, remove from list
                        wait_list.erase(wait_list.begin() + i);
                        successes++;
                    }
                }
                else
                {
                    wait_list.erase(wait_list.begin() + i);
                    query_fails++;
                }
                i--;
            }

            first_round = false; // Sleep at start of next iteration
            if (difftime(time(NULL), begin) >= seconds_remaining)
            {
                time_is_up = true;
            }
        }

        auto fails = repl_fails + query_fails + wait_list.size();
        if (fails > 0)
        {
            const char MSG[] = "Replication from the new master could not be confirmed for %lu slaves. "
                               "%d encountered an I/O or SQL error, %d failed to reply and %lu did not "
                               "advance in Gtid until time ran out.";
            MXS_WARNING(MSG, fails, repl_fails, query_fails, wait_list.size());
        }
        rval = (successes > 0);
    }
    else
    {
        MXS_ERROR("Could not confirm replication after switchover/failover because query to "
                  "the new master failed.");
    }
    return rval;
}

/**
 * Prepares a server for the replication master role.
 *
 * @param new_master The new master server
 * @param err_out json object for error printing. Can be NULL.
 * @return True if successful
 */
bool MariaDBMonitor::promote_new_master(MariaDBServer* new_master, json_t** err_out)
{
    bool success = false;
    MYSQL* new_master_conn = new_master->m_server_base->con;
    MXS_NOTICE("Promoting server '%s' to master.", new_master->name());
    const char* query = "STOP SLAVE;";
    if (mxs_mysql_query(new_master_conn, query) == 0)
    {
        query = "RESET SLAVE ALL;";
        if (mxs_mysql_query(new_master_conn, query) == 0)
        {
            query = "SET GLOBAL read_only=0;";
            if (mxs_mysql_query(new_master_conn, query) == 0)
            {
                success = true;
            }
        }
    }

    if (!success)
    {
        PRINT_MXS_JSON_ERROR(err_out, "Promotion failed: '%s'. Query: '%s'.",
                             mysql_error(new_master_conn), query);
    }
    else
    {
        // Promotion commands ran successfully, run promotion sql script file before external replication.
        if (!m_promote_sql_file.empty() && !new_master->run_sql_from_file(m_promote_sql_file, err_out))
        {
            PRINT_MXS_JSON_ERROR(err_out, "%s execution failed when promoting server '%s'.",
                                 CN_PROMOTION_SQL_FILE, new_master->name());
            success = false;
        }
        // If the previous master was a slave to an external master, start the equivalent slave connection on
        // the new master. Success of replication is not checked.
        else if (m_external_master_port != PORT_UNKNOWN && !start_external_replication(new_master, err_out))
        {
            success = false;
        }
    }

    return success;
}

MariaDBServer* MariaDBMonitor::select_promotion_target(MariaDBServer* demotion_target,
                                                       ClusterOperation op, Log log_mode,
                                                       json_t** error_out)
{
    /* Select a new master candidate. Selects the one with the latest event in relay log.
     * If multiple slaves have same number of events, select the one with most processed events. */

    if (!demotion_target->m_node.children.empty())
    {
        if (log_mode == Log::ON)
        {
            MXS_NOTICE("Selecting a server to promote and replace '%s'. Candidates are: %s.",
                       demotion_target->name(),
                       monitored_servers_to_string(demotion_target->m_node.children).c_str());
        }
    }
    else
    {
        PRINT_ERROR_IF(log_mode, error_out, "'%s' does not have any slaves to promote.",
                       demotion_target->name());
        return NULL;
    }

    // Servers that cannot be selected because of exclusion, but seem otherwise ok.
    ServerArray valid_but_excluded;

    string all_reasons;
    DelimitedPrinter printer("\n");
    // The valid promotion candidates are the slaves replicating directly from the demotion target.
    ServerArray candidates;
    for (MariaDBServer* cand : demotion_target->m_node.children)
    {
        string reason;
        if (!cand->can_be_promoted(op, demotion_target, &reason))
        {
            string msg = string_printf("'%s' cannot be selected because %s", cand->name(), reason.c_str());
            printer.cat(all_reasons, msg);
        }
        else if (server_is_excluded(cand))
        {
            valid_but_excluded.push_back(cand);
            string msg = string_printf("'%s' cannot be selected because it is excluded.", cand->name());
            printer.cat(all_reasons, msg);
        }
        else
        {
            candidates.push_back(cand);
        }
    }

    MariaDBServer* current_best = NULL;
    if (candidates.empty())
    {
        PRINT_ERROR_IF(log_mode, error_out, "No suitable promotion candidate found:\n%s", all_reasons.c_str());
    }
    else
    {
        current_best = candidates.front();
        candidates.erase(candidates.begin());
        if (!all_reasons.empty() && log_mode == Log::ON)
        {
            MXS_WARNING("Some servers were disqualified for promotion:\n%s", all_reasons.c_str());
        }
    }

    // Check which candidate is best
    string current_best_reason;
    for (MariaDBServer* cand : candidates)
    {
        if (is_candidate_better(cand, current_best, m_master_gtid_domain, &current_best_reason))
        {
            // Select the server for promotion, for now.
            current_best = cand;
        }
    }

    // Check if any of the excluded servers would be better than the best candidate. Only print one item.
    if (log_mode == Log::ON)
    {
        for (MariaDBServer* excluded_info : valid_but_excluded)
        {
            const char* excluded_name = excluded_info->name();
            if (current_best == NULL)
            {
                const char EXCLUDED_ONLY_CAND[] = "Server '%s' is a viable choice for new master, "
                                                  "but cannot be selected as it's excluded.";
                MXS_WARNING(EXCLUDED_ONLY_CAND, excluded_name);
                break;
            }
            else if (is_candidate_better(excluded_info, current_best, m_master_gtid_domain))
            {
                // Print a warning if this server is actually a better candidate than the previous best.
                const char EXCLUDED_CAND[] = "Server '%s' is superior to current best candidate '%s', "
                                             "but cannot be selected as it's excluded. This may lead to "
                                             "loss of data if '%s' is ahead of other servers.";
                MXS_WARNING(EXCLUDED_CAND, excluded_name, current_best->name(), excluded_name);
                break;
            }
        }
    }

    if (current_best && log_mode == Log::ON)
    {
        // If there was a specific reason this server was selected, print it now. If the first candidate
        // was chosen (likely all servers were equally good), do not print.
        string msg = string_printf("Selected '%s'", current_best->name());
        msg += current_best_reason.empty() ? "." : (" because " + current_best_reason);
        MXS_NOTICE("%s", msg.c_str());
    }
    return current_best;
}

/**
 * Is the server in the excluded list
 *
 * @param server Server to test
 * @return True if server is in the excluded-list of the monitor.
 */
bool MariaDBMonitor::server_is_excluded(const MariaDBServer* server)
{
    for (MariaDBServer* excluded : m_excluded_servers)
    {
        if (excluded == server)
        {
            return true;
        }
    }
    return false;
}

/**
 * Is the candidate a better choice for master than the previous best?
 *
 * @param candidate_info Server info of new candidate
 * @param current_best_info Server info of current best choice
 * @param gtid_domain Which domain to compare
 * @param reason_out Why is the candidate better than current_best
 * @return True if candidate is better
 */
bool MariaDBMonitor::is_candidate_better(const MariaDBServer* candidate, const MariaDBServer* current_best,
                                         uint32_t gtid_domain, std::string* reason_out)
{
    string reason;
    bool is_better = false;
    uint64_t cand_io = candidate->m_slave_status[0].gtid_io_pos.get_gtid(gtid_domain).m_sequence;
    uint64_t curr_io = current_best->m_slave_status[0].gtid_io_pos.get_gtid(gtid_domain).m_sequence;
    // A slave with a later event in relay log is always preferred.
    if (cand_io > curr_io)
    {
        is_better = true;
        reason = "it has received more events.";
    }
    // If io sequences are identical ...
    else if (cand_io == curr_io)
    {
        uint64_t cand_processed = candidate->m_gtid_current_pos.get_gtid(gtid_domain).m_sequence;
        uint64_t curr_processed = current_best->m_gtid_current_pos.get_gtid(gtid_domain).m_sequence;
        // ... the slave with more events processed wins.
        if (cand_processed > curr_processed)
        {
            is_better = true;
            reason = "it has processed more events.";
        }
        // If gtid positions are identical ...
        else if (cand_processed == curr_processed)
        {
            bool cand_updates = candidate->m_rpl_settings.log_slave_updates;
            bool curr_updates = current_best->m_rpl_settings.log_slave_updates;
            // ... prefer a slave with log_slave_updates.
            if (cand_updates && !curr_updates)
            {
                is_better = true;
                reason = "it has 'log_slave_updates' on.";
            }
            // If both have log_slave_updates on ...
            else if (cand_updates && curr_updates)
            {
                bool cand_disk_ok = !server_is_disk_space_exhausted(candidate->m_server_base->server);
                bool curr_disk_ok = !server_is_disk_space_exhausted(current_best->m_server_base->server);
                // ... prefer a slave without disk space issues.
                if (cand_disk_ok && !curr_disk_ok)
                {
                    is_better = true;
                    reason = "it is not low on disk space.";
                }
            }
        }
    }

    if (reason_out && is_better)
    {
        *reason_out = reason;
    }
    return is_better;
}

/**
 * Check cluster and parameters for suitability to failover. Also writes found servers to output pointers.
 *
 * @param log_mode Logging mode
 * @param promotion_target_out Output for promotion target
 * @param demotion_target_out Output for demotion target
 * @param error_out Error output
 * @return True if cluster is suitable and failover may proceed
 */
bool MariaDBMonitor::failover_prepare(Log log_mode,
                                      MariaDBServer** promotion_target_out,
                                      MariaDBServer** demotion_target_out,
                                      json_t** error_out)
{
    // This function resembles 'switchover_prepare', but does not yet support manual selection.
    const auto op = ClusterOperation::FAILOVER;
    // Check that the cluster has a non-functional master server and that one of the slaves of
    // that master can be promoted. TODO: add support for demoting a relay server.
    MariaDBServer* demotion_target = NULL;
    // Autoselect current master as demotion target.
    string demotion_msg;
    if (m_master == NULL)
    {
        const char msg[] = "Can not select a demotion target for failover: cluster does not have a master.";
        PRINT_ERROR_IF(log_mode, error_out, msg);
    }
    else if (!m_master->can_be_demoted_failover(&demotion_msg))
    {
        const char msg[] = "Can not select '%s' as a demotion target for failover because %s";
        PRINT_ERROR_IF(log_mode, error_out, msg, m_master->name(), demotion_msg.c_str());
    }
    else
    {
        demotion_target = m_master;
    }

    MariaDBServer* promotion_target = NULL;
    if (demotion_target)
    {
        // Autoselect best server for promotion.
        MariaDBServer* promotion_candidate = select_promotion_target(demotion_target, op,
                                                                     log_mode, error_out);
        if (promotion_candidate)
        {
            promotion_target = promotion_candidate;
        }
        else
        {
            PRINT_ERROR_IF(log_mode, error_out, "Could not autoselect promotion target for failover.");
        }
    }

    bool gtid_ok = false;
    if (demotion_target)
    {
        gtid_ok = check_gtid_replication(log_mode, demotion_target, error_out);
    }

    if (promotion_target && demotion_target && gtid_ok)
    {
        *promotion_target_out = promotion_target;
        *demotion_target_out = demotion_target;
        return true;
    }
    return false;
}

/**
 * @brief Process possible failover event
 *
 * If a master failure has occurred and MaxScale is configured with failover functionality, this fuction
 * executes failover to select and promote a new master server. This function should be called immediately
 * after @c mon_process_state_changes. If an error occurs, this method disables automatic failover.
*/
void MariaDBMonitor::handle_auto_failover()
{
    string cluster_issues;
    // TODO: Only check this when topology has changed.
    if (!cluster_supports_failover(&cluster_issues))
    {
        const char PROBLEMS[] = "The backend cluster does not support failover due to the following "
                                "reason(s):\n"
                                "%s\n\n"
                                "Automatic failover has been disabled. It should only be enabled after the "
                                "above issues have been resolved.";
        string p1 = string_printf(PROBLEMS, cluster_issues.c_str());
        string p2 = string_printf(RE_ENABLE_FMT, "failover", CN_AUTO_FAILOVER, m_monitor->name);
        string total_msg = p1 + " " + p2;
        MXS_ERROR("%s", total_msg.c_str());
        m_auto_failover = false;
        disable_setting(CN_AUTO_FAILOVER);
        return;
    }

    if (m_master && m_master->is_master())
    {
        // Certainly no need for a failover.
        return;
    }

    // If master seems to be down, check if slaves are receiving events.
    if (m_verify_master_failure && m_master && m_master->is_down() && slave_receiving_events())
    {
        MXS_INFO("Master failure not yet confirmed by slaves, delaying failover.");
        return;
    }

    MariaDBServer* failed_master = NULL;
    for (auto iter = m_servers.begin(); iter != m_servers.end(); iter++)
    {
        MariaDBServer* server = *iter;
        MXS_MONITORED_SERVER* mon_server = server->m_server_base;
        if (mon_server->new_event && mon_server->server->last_event == MASTER_DOWN_EVENT)
        {
            if (mon_server->server->active_event)
            {
                // MaxScale was active when the event took place
                failed_master = server;
            }
            else
            {
                /* If a master_down event was triggered when this MaxScale was passive, we need to execute
                 * the failover script again if no new masters have appeared. */
                int64_t timeout = MXS_SEC_TO_CLOCK(m_failover_timeout);
                int64_t t = mxs_clock() - mon_server->server->triggered_at;

                if (t > timeout)
                {
                    MXS_WARNING("Failover of server '%s' did not take place within %u seconds, "
                                "failover needs to be re-triggered", server->name(), m_failover_timeout);
                    failed_master = server;
                }
            }
        }
    }

    if (failed_master)
    {
        if (m_failcount > 1 && failed_master->m_server_base->mon_err_count == 1)
        {
            MXS_WARNING("Master has failed. If master status does not change in %d monitor passes, failover "
                        "begins.", m_failcount - 1);
        }
        else if (failed_master->m_server_base->mon_err_count >= m_failcount)
        {
            // Failover is required, but first we should check if preconditions are met.
            MariaDBServer* promotion_target = NULL;
            MariaDBServer* demotion_target = NULL;
            Log log_mode = m_warn_failover_precond ? Log::ON : Log::OFF;
            if (failover_prepare(log_mode, &promotion_target, &demotion_target, NULL))
            {
                m_warn_failover_precond = true;
                MXS_NOTICE("Performing automatic failover to replace failed master '%s'.",
                           failed_master->name());
                failed_master->m_server_base->new_event = false;
                if (!do_failover(promotion_target, demotion_target, NULL))
                {
                    report_and_disable("failover", CN_AUTO_FAILOVER, &m_auto_failover);
                }
            }
            else
            {
                // Failover was not attempted because of errors, however these errors are not permanent.
                // Servers were not modified, so it's ok to try this again.
                if (m_warn_failover_precond)
                {
                    MXS_WARNING("Not performing automatic failover. Will keep retrying with this message "
                                "suppressed.");
                    m_warn_failover_precond = false;
                }
            }
        }
    }
    else
    {
        m_warn_failover_precond = true;
    }
}

/**
 * Is the topology such that failover is supported, even if not required just yet?
 *
 * @param reasons_out Why failover is not supported
 * @return True if cluster supports failover
 */
bool MariaDBMonitor::cluster_supports_failover(string* reasons_out)
{
    bool rval = true;
    string separator;
    // Currently, only simple topologies are supported. No Relay Masters or multiple slave connections.
    // Gtid-replication is required, and a server version which supports it.
    for (MariaDBServer* server : m_servers)
    {
        // Need to accept unknown versions here. Otherwise servers which are down when the monitor starts
        // would deactivate failover.
        if (server->m_version != MariaDBServer::version::UNKNOWN &&
            server->m_version != MariaDBServer::version::MARIADB_100)
        {
            *reasons_out += separator + string_printf("The version of server '%s' is not supported. Failover "
                                                      "requires MariaDB 10.X.", server->name());
            separator = "\n";
            rval = false;
        }

        if (server->is_slave() && !server->m_slave_status.empty())
        {
            if (server->m_slave_status.size() > 1)
            {
                *reasons_out += separator +
                    string_printf("Server '%s' is replicating or attempting to replicate from "
                                  "multiple masters.", server->name());
                separator = "\n";
                rval = false;
            }
            if (!server->uses_gtid())
            {
                *reasons_out += separator +
                    string_printf("Server '%s' is not using gtid-replication.", server->name());
                separator = "\n";
                rval = false;
            }
        }

        if (server->is_relay_master())
        {
            *reasons_out += separator +
                    string_printf("Server '%s' is a relay master. Only topologies with one replication "
                                  "layer are supported.", server->name());
            separator = "\n";
            rval = false;
        }
    }
    return rval;
}

/**
 * Check if a slave is receiving events from master.
 *
 * @return True, if a slave has an event more recent than master_failure_timeout.
 */
bool MariaDBMonitor::slave_receiving_events()
{
    mxb_assert(m_master);
    bool received_event = false;
    int64_t master_id = m_master->m_server_base->server->node_id;

    for (MariaDBServer* server : m_servers)
    {
        if (!server->m_slave_status.empty() &&
            server->m_slave_status[0].slave_io_running == SlaveStatus::SLAVE_IO_YES &&
            server->m_slave_status[0].master_server_id == master_id &&
            difftime(time(NULL), server->m_latest_event) < m_master_failure_timeout)
        {
            /**
             * The slave is still connected to the correct master and has received events. This means that
             * while MaxScale can't connect to the master, it's probably still alive.
             */
            received_event = true;
            break;
        }
    }
    return received_event;
}

/**
 * Print a redirect error to logs. If err_out exists, generate a combined error message by querying all
 * the server parameters for connection errors and append these errors to err_out.
 *
 * @param demotion_target If not NULL, this is the first server to query.
 * @param redirectable_slaves Other servers to query for errors.
 * @param err_out If not null, the error output object.
 */
static void print_redirect_errors(MariaDBServer* first_server, const ServerArray& servers,
                                  json_t** err_out)
{
    // Individual server errors have already been printed to the log.
    // For JSON, gather the errors again.
    const char* const MSG = "Could not redirect any slaves to the new master.";
    MXS_ERROR(MSG);
    if (err_out)
    {
        ServerArray failed_slaves;
        if (first_server)
        {
            failed_slaves.push_back(first_server);
        }
        for (auto iter = servers.begin(); iter != servers.end(); iter++)
        {
            failed_slaves.push_back(*iter);
        }

        string combined_error = get_connection_errors(failed_slaves);
        *err_out = mxs_json_error_append(*err_out, "%s Errors: %s.", MSG, combined_error.c_str());
    }
}

/**
 * Check cluster and parameters for suitability to switchover. Also writes found servers to output pointers.
 *
 * @param promotion_server The server which should be promoted. If null, monitor will autoselect.
 * @param demotion_server The server which should be demoted. Can be null for autoselect.
 * @param log_mode Logging mode
 * @param promotion_target_out Output for promotion target
 * @param demotion_target_out Output for demotion target
 * @param error_out Error output
 * @return True if cluster is suitable and server parameters were valid
 */
bool MariaDBMonitor::switchover_prepare(SERVER* promotion_server, SERVER* demotion_server,
                                        Log log_mode,
                                        MariaDBServer** promotion_target_out,
                                        MariaDBServer** demotion_target_out,
                                        json_t** error_out)
{
    const auto op = ClusterOperation::SWITCHOVER;
    // Check that both servers are ok if specified, or autoselect them. Demotion target must be checked
    // first since the promotion target depends on it.
    mxb_assert(promotion_target_out && demotion_target_out &&
               !*promotion_target_out && !*demotion_target_out);
    const char NO_SERVER[] = "Server '%s' is not a member of monitor '%s'.";

    MariaDBServer* demotion_target = NULL;
    string demotion_msg;
    if (demotion_server)
    {
        // Manual select.
        MariaDBServer* demotion_candidate = get_server(demotion_server);
        if (demotion_candidate == NULL)
        {
            PRINT_ERROR_IF(log_mode, error_out, NO_SERVER, demotion_server->name, m_monitor->name);
        }
        else if (!demotion_candidate->can_be_demoted_switchover(&demotion_msg))
        {
            PRINT_ERROR_IF(log_mode, error_out,  "'%s' is not a valid demotion target for switchover: %s",
                           demotion_candidate->name(), demotion_msg.c_str());
        }
        else
        {
            demotion_target = demotion_candidate;
        }
    }
    else
    {
        // Autoselect current master as demotion target.
        if (m_master == NULL)
        {
            const char msg[] = "Can not autoselect a demotion target for switchover: cluster does "
                               "not have a master.";
            PRINT_ERROR_IF(log_mode, error_out, msg);
        }
        else if (!m_master->can_be_demoted_switchover(&demotion_msg))
        {
            const char msg[] = "Can not autoselect '%s' as a demotion target for switchover because %s";
            PRINT_ERROR_IF(log_mode, error_out, msg, m_master->name(), demotion_msg.c_str());
        }
        else
        {
            demotion_target = m_master;
        }
    }

    MariaDBServer* promotion_target = NULL;
    if (demotion_target)
    {
        string promotion_msg;
        if (promotion_server)
        {
            // Manual select.
            MariaDBServer* promotion_candidate = get_server(promotion_server);
            if (promotion_candidate == NULL)
            {
                PRINT_ERROR_IF(log_mode, error_out, NO_SERVER, promotion_server->name, m_monitor->name);
            }
            else if (!promotion_candidate->can_be_promoted(op, demotion_target, &promotion_msg))
            {
                const char msg[] = "'%s' is not a valid promotion target for switchover because %s";
                PRINT_ERROR_IF(log_mode, error_out, msg, promotion_candidate->name(), promotion_msg.c_str());
            }
            else
            {
                promotion_target = promotion_candidate;
            }
        }
        else
        {
            // Autoselect. More involved than the autoselecting the demotion target.
            MariaDBServer* promotion_candidate = select_promotion_target(demotion_target, op,
                                                                         log_mode, error_out);
            if (promotion_candidate)
            {
                promotion_target = promotion_candidate;
            }
            else
            {
                PRINT_ERROR_IF(log_mode, error_out, "Could not autoselect promotion target for switchover.");
            }
        }
    }

    bool gtid_ok = false;
    if (demotion_target)
    {
        gtid_ok = check_gtid_replication(log_mode, demotion_target, error_out);
    }

    if (promotion_target && demotion_target && gtid_ok)
    {
        *demotion_target_out = demotion_target;
        *promotion_target_out = promotion_target;
        return true;
    }
    return false;
}

void MariaDBMonitor::enforce_read_only_on_slaves()
{
    const char QUERY[] = "SET GLOBAL read_only=1;";
    for (MariaDBServer* server : m_servers)
    {
        if (server->is_slave() && !server->is_read_only() &&
            (server->m_version != MariaDBServer::version::BINLOG_ROUTER))
        {
            MYSQL* conn = server->m_server_base->con;
            if (mxs_mysql_query(conn, QUERY) == 0)
            {
                MXS_NOTICE("read_only set to ON on server '%s'.", server->name());
            }
            else
            {
                MXS_ERROR("Setting read_only on server '%s' failed: '%s.", server->name(), mysql_error(conn));
            }
        }
    }
}

void MariaDBMonitor::set_low_disk_slaves_maintenance()
{
    // Only set pure slave and standalone servers to maintenance.
    for (MariaDBServer* server : m_servers)
    {
        if (server->has_status(SERVER_DISK_SPACE_EXHAUSTED) && server->is_usable() &&
            !server->is_master() && !server->is_relay_master())
        {
            server->set_status(SERVER_MAINT);
            m_cluster_modified = true;
        }
    }
}

void MariaDBMonitor::handle_low_disk_space_master()
{
    if (m_master && m_master->is_master() && m_master->is_low_on_disk_space())
    {
        if (m_warn_switchover_precond)
        {
            MXS_WARNING("Master server '%s' is low on disk space. Attempting to switch it with a slave.",
                        m_master->name());
        }

        // Looks like the master should be swapped out. Before trying it, check if there is even
        // a likely valid slave to swap to.
        MariaDBServer* demotion_target = NULL;
        MariaDBServer* promotion_target = NULL;
        Log log_mode = m_warn_switchover_precond ? Log::ON : Log::OFF;
        auto ok_to_switch = switchover_prepare(NULL, m_master->m_server_base->server, log_mode,
                                               &promotion_target, &demotion_target,
                                               NULL);
        if (ok_to_switch)
        {
            m_warn_switchover_precond = true;
            bool switched = do_switchover(demotion_target, promotion_target, NULL);
            if (switched)
            {
                MXS_NOTICE("Switchover %s -> %s performed.",
                           demotion_target->name(), promotion_target->name());
            }
            else
            {
                report_and_disable("switchover", CN_SWITCHOVER_ON_LOW_DISK_SPACE,
                                   &m_switchover_on_low_disk_space);
            }
        }
        else
        {
            // Switchover was not attempted because of errors, however these errors are not permanent.
            // Servers were not modified, so it's ok to try this again.
            if (m_warn_switchover_precond)
            {
                MXS_WARNING("Not performing automatic switchover. Will keep retrying with this message "
                            "suppressed.");
                m_warn_switchover_precond = false;
            }
        }
    }
    else
    {
        m_warn_switchover_precond = true;
    }
}

void MariaDBMonitor::report_and_disable(const string& operation, const string& setting_name,
                                        bool* setting_var)
{
    string p1 = string_printf("Automatic %s failed, disabling automatic %s.", operation.c_str(),
                              operation.c_str());
    string p2 = string_printf(RE_ENABLE_FMT, operation.c_str(), setting_name.c_str(), m_monitor->name);
    string error_msg = p1 + " " + p2;
    MXS_ERROR("%s", error_msg.c_str());
    *setting_var = false;
    disable_setting(setting_name.c_str());
}

/**
 * Check that the slaves to demotion target are using gtid replication and that the gtid domain of the
 * cluster is defined. Only the slave connections to the demotion target are checked.
 *
 * @param log_mode Logging mode
 * @param demotion_target The server whose slaves should be checked
 * @param error_out Error output
 * @return True if gtid is used
 */
bool MariaDBMonitor::check_gtid_replication(Log log_mode,
                                            const MariaDBServer* demotion_target,
                                            json_t** error_out)
{
    bool gtid_domain_ok = false;
    if (m_master_gtid_domain == GTID_DOMAIN_UNKNOWN)
    {
        PRINT_ERROR_IF(log_mode, error_out,
                       "Cluster gtid domain is unknown. This is usually caused by the cluster never "
                       "having a master server while MaxScale was running.");
    }
    else
    {
        gtid_domain_ok = true;
    }

    // Check that all slaves are using gtid-replication.
    bool gtid_ok = true;
    for (MariaDBServer* server : demotion_target->m_node.children)
    {
        auto sstatus = server->slave_connection_status(demotion_target);
        if (sstatus && sstatus->gtid_io_pos.empty())
        {
            PRINT_ERROR_IF(log_mode, error_out,
                           "The slave connection '%s' -> '%s' is not using gtid replication.",
                           server->name(), demotion_target->name());
            gtid_ok = false;
        }
    }

    return gtid_domain_ok && gtid_ok;
}

/**
 * List slaves which should be redirected to the new master.
 *
 * @param promotion_target The server which will be promoted
 * @param demotion_target The server which will be demoted
 * @return A list of slaves to redirect
 */
ServerArray MariaDBMonitor::get_redirectables(const MariaDBServer* promotion_target,
                                              const MariaDBServer* demotion_target)
{
    ServerArray redirectable_slaves;
    for (MariaDBServer* slave : demotion_target->m_node.children)
    {
        if (slave != promotion_target)
        {
            auto sstatus = slave->slave_connection_status(demotion_target);
            if (sstatus && !sstatus->gtid_io_pos.empty())
            {
                redirectable_slaves.push_back(slave);
            }
        }
    }
    return redirectable_slaves;
}
