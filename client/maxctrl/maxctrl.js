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
require('./common.js')()

'use strict';

const maxctrl_version = '1.0.0';

program
    .version(maxctrl_version)
    .group(['u', 'p', 'h', 'p', 'P', 's'], 'Global Options:')
    .option('u', {
        alias:'user',
        global: true,
        default: 'mariadb',
        describe: 'Username to use',
        type: 'string'
    })
    .option('p', {
        alias: 'password',
        describe: 'Password for the user',
        default: 'admin',
        type: 'string'
    })
    .option('h', {
        alias: 'host',
        describe: 'The hostname or address where MaxScale is located',
        default: 'localhost',
        type: 'string'
    })
    .option('P', {
        alias: 'port',
        describe: 'The port where MaxScale REST API listens on',
        default: 8989,
        type: 'number'
    })
    .option('s', {
        alias: 'secure',
        describe: 'Enable TLS encryption of connections',
        default: 'false',
        type: 'boolean'
    })
    .command(require('./lib/list.js'))
    .command(require('./lib/show.js'))
    .command(require('./lib/set.js'))
    .command(require('./lib/clear.js'))
    .help()
    .demandCommand(1, 'At least one command is required')
    .command('*', 'the default command', {}, () => {
        console.log("Unknown command. See output of 'help' for a list of commands.")
    })
    .argv
