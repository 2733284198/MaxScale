# MariaDB MaxScale 2.1.17 Release Notes

Release 2.1.17 is a GA release.

This document describes the changes in release 2.1.17, when compared
to release [2.1.16](MaxScale-2.1.15-Release-Notes.md).

For any problems you encounter, please consider submitting a bug report at
[Jira](https://jira.mariadb.org).

## Changed Features

* Info level messages will now also be logged to syslog, if logging to
  syslog is enabled.

## Bug fixes

[Here is a list of bugs fixed in MaxScale 2.1.17.](https://jira.mariadb.org/issues/?jql=project%20%3D%20MXS%20AND%20issuetype%20%3D%20Bug%20AND%20status%20%3D%20Closed%20AND%20fixVersion%20%3D%202.1.17)

* [MXS-1819](https://jira.mariadb.org/browse/MXS-1819) log_info does not log to syslog
* [MXS-1788](https://jira.mariadb.org/browse/MXS-1788) MaxInfo crash
* [MXS-1767](https://jira.mariadb.org/browse/MXS-1767) Server capabilities are not correct
* [MXS-1762](https://jira.mariadb.org/browse/MXS-1762) The client IP should be considered when choosing a persistent connection

## Packaging

RPM and Debian packages are provided for the Linux distributions supported by
MariaDB Enterprise.

Packages can be downloaded [here](https://mariadb.com/resources/downloads).

## Source Code

The source code of MaxScale is tagged at GitHub with a tag, which is identical
with the version of MaxScale. For instance, the tag of version X.Y.Z of MaxScale
is maxscale-X.Y.Z.

The source code is available [here](https://github.com/mariadb-corporation/MaxScale).
