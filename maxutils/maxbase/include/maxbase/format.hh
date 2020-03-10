/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-03-10
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#pragma once

#include <maxbase/ccdefs.hh>

#include <cstdint>
#include <string>

namespace maxbase
{

/**
 * Convert a number into the IEC human readable representation
 *
 * @param size Value to convert
 *
 * @return Value as a human readable size e.g. 5.01MiB
 */
std::string to_binary_size(int64_t size);
}
