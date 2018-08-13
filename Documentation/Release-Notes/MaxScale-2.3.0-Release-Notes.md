# MariaDB MaxScale 2.3.0 Release Notes

Release 2.2.0 is a Beta release.

This document describes the changes in release 2.3.0, when compared to
release 2.2.

For any problems you encounter, please consider submitting a bug
report at [Jira](https://jira.mariadb.org).

## Changed Features

### `query_retries`

The default value of `query_retries` was changed from 0 to 1. This turns
on failure tolerant queries for monitors and authenticators which prevents
failures due to network problems. A single reconnection attempt is made to
keep responsiveness high even with default values.

### Session Command History

The _readwritesplit_ session command history is now enabled mby default but it
is limited to a total of 50 distict session commands. This default allows most
sessions to leverage the newly improved reconnection mechanism without having to
explicitly enable the history. When the limit of 50 commands is exceeded, the
session command history is disabled. This makes it possible to use pooled
connections by default without having to explicitly disable the history (this
was the case with pre-2.1 versions of MaxScale).

The way that the history is stored has also changed. Instead of storing all
session commands, each session only stores the first and last execution of each
command. This way the history is compressed into a compact representation while
still retaining the relative order of each command.

To keep the old functionality, add `disable_sescmd_history=true` to the service
definition.

### Masking Filter

By default the masking filter rejects statements that use functions on
conjuction with columns that should be masked. Please see the
[Masking Filter](../Filters/Masking.md) documentation for details.

###  Switchover new master autoselection

The switchover command in *mariadbmon* can now be called with just the monitor
name as parameter. In this case the monitor will automatically select a server
for promotion.

### `router_options` in Binlogrouter

The use of `router_options` with the binlogrouter module is deprecated in
MaxScale 2.3 and will be removed in a subsequent release. The
`router_options` should be converted into separate parameters.

### `thread_stack_size`

The `thread_stack_size` configuration parameter is ignored and has been
deprecated. If you need to explicitly set the stack size, do so using
`ulimit -s` before starting MaxScale.

## Dropped Features

### Configuration Reloading

The deprecated `maxadmin reload config` command has been removed.

### `router_options` in Avrorouter

The use of `router_options` with avrorouter was deprecated in MaxScale 2.1. In
MaxScale 2.3, the use of `router_options` is no longer supported and the options
should be given as parameters instead.

### `router_options` in readwritesplit

The use of `router_options` with readwritesplit, which was deprecated in
MaxScale 2.2.0, has been removed in MaxScale 2.3.0.

### `QUERY-LAST-TRANSACTION` and `QUERY-TRANSACTION` CDC commands

The CDC protocol no longer accepts the `QUERY-LAST-TRANSACTION` and
`QUERY-TRANSACTION` commands. They were removed due to the addition of the REST
API that provides the same information in a more easy to process format.

### `ssl` for Servers and Listeners

The `ssl` parameter now accepts boolean values ìn addition to the old `required`
and `disabled` values.

## New Features

### Runtime Configuration of the Cache

With the variable `@maxscale.cache.enabled` it is now possible for a
client to specify whether the cache should be used. Please see the
[Cache](../Filters/Cache.md) documentation for details.

### User Specified Syslog Facility and Level for Authentication Errors

It is now possible for the end user to specify the syslog facility and level
for authentication errors. Please see
[the configuration guide](../Getting-Started/Configuration-Guide.md#events)
for details.

### ReadWriteSplit

A set of new features have been added to readwritesplit.

#### `transaction_replay`

The [`transaction_replay`](../Routers/ReadWriteSplit.md#transaction_replay)
parameter enables replaying of transactions if a master server is lost
mid-transaction. This allows transparent replacement of master servers with a
minimal amount of failed transactions.

#### `master_reconnection`

With the
[`master_reconnection`](../Routers/ReadWriteSplit.md#master_reconnection)
parameter, if the connection to a master server is lost or the master server
changes, readwritesplit can now reconnect seamlessly to the master server
without losing the session state.

#### `delayed_retry`

The [`delayed_retry`](../Routers/ReadWriteSplit.md#delayed_retry) parameter
allows queries to be automatically retried if their execution is interrupted.

#### `causal_reads`

The [`causal_reads`](../Routers/ReadWriteSplit.md#causal_reads) parameter
enables distributed consistent reads with MariaDB version 10.2 and newer.

## Bug fixes

[Here is a list of bugs fixed in MaxScale 2.3.0.](https://jira.mariadb.org/issues/?jql=project%20%3D%20MXS%20AND%20issuetype%20%3D%20Bug%20AND%20status%20%3D%20Closed%20AND%20fixVersion%20%3D%202.3.0)

## Known Issues and Limitations

There are some limitations and known issues within this version of MaxScale.
For more information, please refer to the [Limitations](../About/Limitations.md) document.

## Packaging

RPM and Debian packages are provided for the Linux distributions supported
by MariaDB Enterprise.

Packages can be downloaded [here](https://mariadb.com/resources/downloads).

## Source Code

The source code of MaxScale is tagged at GitHub with a tag, which is identical
with the version of MaxScale. For instance, the tag of version X.Y.Z of MaxScale
is X.Y.Z. Further, *master* always refers to the latest released non-beta version.

The source code is available [here](https://github.com/mariadb-corporation/MaxScale).
