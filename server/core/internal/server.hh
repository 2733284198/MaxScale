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
#include <maxscale/server.hh>
#include <maxscale/resultset.hh>
#include <maxscale/routingworker.hh>

std::unique_ptr<ResultSet> serverGetList();

// Private server implementation
class Server : public SERVER
{
public:
    Server()
        : m_response_time(maxbase::EMAverage {0.04, 0.35, 500})
    {
    }

    ~Server() override
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
    static Server* find_by_unique_name(const std::string& name);
    static void dprintServer(DCB*, const Server*);
    static void dprintPersistentDCBs(DCB*, const Server*);

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
