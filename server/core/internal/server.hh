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
#pragma once

/**
 * Internal header for the server type
 */

#include <maxbase/ccdefs.hh>

#include <mutex>
#include <maxbase/average.hh>
#include <maxscale/config.hh>
#include <maxscale/server.hh>
#include <maxscale/resultset.hh>

std::unique_ptr<ResultSet> serverGetList();

// Private server implementation
class Server : public SERVER
{
public:
    Server()
        : SERVER()
        , m_response_time(maxbase::EMAverage {0.04, 0.35, 500})
    {
    }

    int response_time_num_samples() const
    {
        return m_response_time.num_samples();
    }

    double response_time_average() const
    {
        return m_response_time.average();
    }

    void response_time_add(double ave, int num_samples);

    bool have_disk_space_limits() const override
    {
        std::lock_guard<std::mutex> guard(m_settings.lock);
        return !m_settings.disk_space_limits.empty();
    }

    MxsDiskSpaceThreshold get_disk_space_limits() const override
    {
        std::lock_guard<std::mutex> guard(m_settings.lock);
        return m_settings.disk_space_limits;
    }

    void set_disk_space_limits(const MxsDiskSpaceThreshold& new_limits) override
    {
        std::lock_guard<std::mutex> guard(m_settings.lock);
        m_settings.disk_space_limits = new_limits;
    }

    /**
     * Print server details to a dcb.
     *
     * @param dcb Dcb to print to
     */
    void print_to_dcb(DCB* dcb) const;

    /**
     * @brief Allocate a new server
     *
     * This will create a new server that represents a backend server that services
     * can use. This function will add the server to the running configuration but
     * will not persist the changes.
     *
     * @param name   Unique server name
     * @param params Parameters for the server
     *
     * @return       The newly created server or NULL if an error occurred
     */
    static Server* server_alloc(const char* name, MXS_CONFIG_PARAMETER* params);

    /**
     * @brief Find a server with the specified name
     *
     * @param name Name of the server
     * @return The server or NULL if not found
     */
    static Server* find_by_unique_name(const std::string& name);

    /**
     * Print server details to a DCB
     *
     * Designed to be called within a debugger session in order
     * to display all active servers within the gateway
     */
    static void dprintServer(DCB*, const Server*);

    /**
     * Diagnostic to print number of DCBs in persistent pool for a server
     *
     * @param       pdcb    DCB to print results to
     * @param       server  SERVER for which DCBs are to be printed
     */
    static void dprintPersistentDCBs(DCB*, const Server*);

    /**
     * Print all servers to a DCB
     *
     * Designed to be called within a debugger session in order
     * to display all active servers within the gateway
     */
    static void dprintAllServers(DCB*);

    /**
     * Print all servers in Json format to a DCB
     */
    static void dprintAllServersJson(DCB*);

    /**
     * List all servers in a tabular form to a DCB
     *
     */
    static void dListServers(DCB*);

    mutable std::mutex m_lock;

private:
    struct Settings
    {
        mutable std::mutex lock;                 /**< Protects array-like settings from concurrent access */
        MxsDiskSpaceThreshold disk_space_limits; /**< Disk space thresholds */
    };

    maxbase::EMAverage m_response_time;   /**< Response time calculations for this server */
    Settings m_settings;                  /**< Server settings */
};

void server_free(Server* server);
