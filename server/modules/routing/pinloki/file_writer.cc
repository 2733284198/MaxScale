/*
 * Copyright (c) 2020 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-08-24
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "pinloki.hh"
#include "file_writer.hh"
#include "file_reader.hh"
#include "config.hh"

#include <maxscale/protocol/mariadb/mysql.hh>
#include <mariadb_rpl.h>
#include <iostream>
#include <iomanip>

namespace
{

/** The next file name has the same base name as the file from the master, but
 *    the counter portion is generated here.  By using the master base_name the
 *    event length stays the same, which means the 'next_pos' in the events do
 *    not have to be modifed.
 */
std::string next_file_name(const std::string& master, const std::string& prev)
{
    using namespace  std;

    auto base_name = master.substr(0, master.find_last_of('.'));

    auto num = 1;
    if (!prev.empty())
    {
        auto num_str = prev.substr(prev.find_last_of(".") + 1);
        num = 1 + atoi(num_str.c_str());
    }

    return MAKE_STR(base_name << '.' << setfill('0') << setw(6) << num);
}
}

namespace pinloki
{
FileWriter::FileWriter(Inventory* inv, const Writer& writer)
    : m_inventory(*inv)
    , m_writer(writer)
{
}

void FileWriter::begin_txn()
{
    mxb_assert(m_in_transaction == false);
    m_in_transaction = true;
}

void FileWriter::commit_txn()
{
    mxb_assert(m_in_transaction == true);
    m_in_transaction = false;

    m_current_pos.file.seekp(m_current_pos.write_pos);
    const auto& buf = m_tx_buffer.str();
    m_current_pos.file.write(buf.data(), buf.size());

    m_current_pos.write_pos = m_current_pos.file.tellp();
    m_current_pos.file.flush();

    m_tx_buffer.str("");
}

void FileWriter::add_event(maxsql::RplEvent& rpl_event)     // FIXME, move into here
{
    bool is_artificial = rpl_event.flags() & LOG_EVENT_ARTIFICIAL_F;
    auto etype = rpl_event.event_type();
    if (etype == HEARTBEAT_LOG_EVENT)
    {
        // Heartbeat event, don't process it
    }
    else if (is_artificial)
    {
        if (etype == ROTATE_EVENT)
        {
            rotate_event(rpl_event.rotate());
        }
    }
    else
    {
        rpl_event.set_next_pos(m_current_pos.write_pos + rpl_event.buffer().size()
                               + m_tx_buffer.str().size());

        if (m_in_transaction)
        {
            m_tx_buffer.write(rpl_event.buffer().data(), rpl_event.buffer().size());
        }
        else if (etype == GTID_LIST_EVENT)
        {
            write_gtid_list(m_current_pos);
        }
        else if (etype != STOP_EVENT && etype != ROTATE_EVENT && etype != BINLOG_CHECKPOINT_EVENT)
        {
            write_to_file(m_current_pos, rpl_event);
        }
    }
}

void FileWriter::rotate_event(const maxsql::Rotate& rotate)
{
    auto master_file_name = rotate.file_name;
    auto last_file_name = m_inventory.last();

    auto new_file_name = next_file_name(master_file_name, last_file_name);
    auto file_name = m_inventory.config().path(new_file_name);

    WritePosition previous_pos {std::move(m_current_pos)};

    m_current_pos.name = file_name;
    m_current_pos.file.open(m_current_pos.name, std::ios_base::out | std::ios_base::binary);
    m_current_pos.file.write(PINLOKI_MAGIC.data(), PINLOKI_MAGIC.size());
    m_current_pos.write_pos = PINLOKI_MAGIC.size();
    m_current_pos.file.flush();

    m_inventory.add(m_current_pos.name);

    if (previous_pos.file.is_open())
    {
        write_rotate(previous_pos, file_name);
        previous_pos.file.close();

        if (!previous_pos.file.good())
        {
            MXB_THROW(BinlogWriteError,
                      "File " << previous_pos.name
                              << " did not close (flush) properly during rotate: "
                              << errno << ", " << mxb_strerror(errno));
        }
    }
    else
    {
        if (!last_file_name.empty())
        {
            write_stop(last_file_name);
        }
    }
}

void FileWriter::write_to_file(WritePosition& fn, const maxsql::RplEvent& rpl_event)
{
    fn.file.seekp(fn.write_pos);
    fn.file.write(rpl_event.buffer().data(), rpl_event.buffer().size());
    fn.file.flush();

    fn.write_pos = rpl_event.next_event_pos();

    if (!fn.file.good())
    {
        MXB_THROW(BinlogWriteError, "Could not write event to " << fn.name);
    }
}

void FileWriter::write_stop(const std::string& file_name)
{
    MXB_SINFO("write stop to " << file_name);

    auto file = std::fstream(file_name, std::ios_base::in | std::ios_base::out | std::ios_base::binary);
    if (!file.good())
    {
        MXB_THROW(BinlogWriteError,
                  "Could not open " << file_name << " for  STOP_EVENT addition");
    }

    constexpr int HEADER_LEN = 19;
    const size_t EVENT_LEN = HEADER_LEN + 4;        // header plus crc

    // TODO this probably always works, but the position should be read from
    // the next_pos of the last event in the file.
    file.seekp(0, std::ios_base::end);
    const size_t end_pos = file.tellp();

    std::vector<char> data(EVENT_LEN);
    uint8_t* ptr = (uint8_t*)&data[0];

    // Zero timestamp
    mariadb::set_byte4(ptr, 0);
    ptr += 4;

    // A stop event
    *ptr++ = STOP_EVENT;

    // server id
    mariadb::set_byte4(ptr, m_inventory.config().server_id());
    ptr += 4;

    // Event length
    mariadb::set_byte4(ptr, EVENT_LEN);
    ptr += 4;

    // Next position
    mariadb::set_byte4(ptr, end_pos + EVENT_LEN);
    ptr += 4;

    // No flags (this is a real event)
    mariadb::set_byte2(ptr, 0);
    ptr += 2;

    // Checksum
    mariadb::set_byte4(ptr, crc32(0, (uint8_t*)data.data(), data.size() - 4));

    file.write(data.data(), data.size());
    file.flush();

    if (!file.good())
    {
        MXB_THROW(BinlogWriteError, "Could not write STOP_EVENT to " << file_name);
    }
}

void FileWriter::write_rotate(FileWriter::WritePosition& fn, const std::string& to_file_name)
{
    auto vec = maxsql::create_rotate_event(basename(to_file_name.c_str()),
                                           m_inventory.config().server_id(),
                                           fn.write_pos,
                                           mxq::Kind::Real);

    fn.file.seekp(fn.write_pos);
    fn.file.write(vec.data(), vec.size());
    fn.file.flush();

    if (!fn.file.good())
    {
        MXB_THROW(BinlogWriteError, "Could not write final ROTATE to " << fn.name);
    }
}

void FileWriter::write_gtid_list(WritePosition& fn)
{
    constexpr int HEADER_LEN = 19;
    auto gtid_list = m_writer.get_gtid_io_pos();
    const auto NUM_GTIDS = gtid_list.gtids().size();
    const size_t EVENT_LEN = HEADER_LEN + 4 + NUM_GTIDS * (4 + 4 + 8) + 4;

    std::vector<char> data(EVENT_LEN);
    uint8_t* ptr = (uint8_t*)&data[0];

    // Zero timestamp
    mariadb::set_byte4(ptr, 0);
    ptr += 4;

    // The event
    *ptr++ = GTID_LIST_EVENT;

    // server id
    mariadb::set_byte4(ptr, m_inventory.config().server_id());
    ptr += 4;

    // Event length
    mariadb::set_byte4(ptr, EVENT_LEN);
    ptr += 4;

    // Next position
    mariadb::set_byte4(ptr, fn.write_pos + EVENT_LEN);
    ptr += 4;

    // No flags (this is a real event)
    mariadb::set_byte2(ptr, 0);
    ptr += 2;

    // Number of gtids to follow:
    mariadb::set_byte4(ptr, NUM_GTIDS);
    ptr += 4;
    // Gtids:
    for (const auto& gtid : gtid_list.gtids())
    {
        mariadb::set_byte4(ptr, gtid.domain_id());
        ptr += 4;
        mariadb::set_byte4(ptr, gtid.server_id());
        ptr += 4;
        mariadb::set_byte8(ptr, gtid.sequence_nr());
        ptr += 8;
    }


    // Checksum
    mariadb::set_byte4(ptr, crc32(0, (uint8_t*)data.data(), data.size() - 4));

    fn.file.write(data.data(), data.size());
    fn.file.flush();
    fn.write_pos += EVENT_LEN;

    if (!fn.file.good())
    {
        MXB_THROW(BinlogWriteError, "Could not write GTID_EVENT to " << fn.name);
    }
}
}
