/*
 * Copyright (c) 2020 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-09-25
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#pragma once
#include "gtid.hh"

#include <string>
#include <vector>
#include <mutex>

#include "config.hh"


namespace pinloki
{

/**
 * @brief List of binlog file names. Thread safe, writable inventory file.
 */
class InventoryWriter
{
public:
    InventoryWriter(const Config& config);
    InventoryWriter(const InventoryWriter&) = delete;
    InventoryWriter& operator=(const InventoryWriter&) = delete;

    /**
     * @brief push a file name to the end of the list
     * @param file_name
     */
    void push_back(const std::string& file_name);

    /**
     * @brief pop the first file
     * @param file_name must match the name of the first file
     */
    void pop_front(const std::string& file_name);

    /**
     * @brief file_names
     * @return the file names
     */
    std::vector<std::string> file_names() const;

    std::string rpl_state() const;

    const Config& config() const
    {
        return m_config;
    }

private:
    // Read or re-read the file
    void read_file() const;

    // Saves the file list on disk
    void persist();

    // The configuration used to create this inventory
    const Config& m_config;

    mutable std::mutex               m_mutex;
    mutable std::vector<std::string> m_file_names;
};

/**
 * @brief List of binlog file names for single-threaded readers, in
 *        any process.
 */
class InventoryReader
{
public:
    InventoryReader(const Config& config);
    const std::vector<std::string>& file_names() const;
    std::string                     rpl_state() const;

    const Config& config() const
    {
        return m_config;
    }

private:
    // The configuration used to create this inventory
    const Config&                    m_config;
    mutable std::vector<std::string> m_file_names;
};

// Return the string after str in a vector of unique strings, or empty if not found
std::string next_string(const std::vector<std::string>& strs, const std::string& str);
// Return the first string in vector or an empty string if the vector is empty
std::string first_string(const std::vector<std::string>& strs);
// Return the last string in vector or an empty string if the vector is empty
std::string last_string(const std::vector<std::string>& strs);
}
