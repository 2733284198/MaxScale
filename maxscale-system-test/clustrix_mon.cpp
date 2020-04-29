/*
 * Copyright (c) 2019 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-04-23
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * @file clustrix_mon.cpp - simple Clustrix monitor test
 * Just creates Clustrix cluster and connect Maxscale to it
 * It can be used as a template for clustrix tests
 *
 * See Clustrix_nodes.h for details about configiration
 */

#include "testconnections.h"

int main(int argc, char* argv[])
{
    int i;
    TestConnections* Test = new TestConnections(argc, argv);

    int rval = Test->global_result;
    delete Test;
    return rval;
}
