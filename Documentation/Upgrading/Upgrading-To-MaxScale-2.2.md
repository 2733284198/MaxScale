# Upgrading MariaDB MaxScale from 2.1 to 2.2

This document describes possible issues upgrading MariaDB MaxScale from version
2.1 to 2.2.

For more information about MariaDB MaxScale 2.2, please refer to the
[ChangeLog](../Changelog.md).

For a complete list of changes in MaxScale 2.2.0, refer to the
[MaxScale 2.2.0 Release Notes](../Release-Notes/MaxScale-2.2.0-Release-Notes.md).

Before starting the upgrade, we recommend you back up your current configuration
file.

### Regular Expression Parameters

Modules may now use a built-in regular expression string parameter type instead
of a normal string when accepting patterns. The only module using the new regex
parameter type is currently *QLAFilter*. When inputting pattern, enclose the
string in slashes, e.g. `match=/^select/` defines the pattern `^select`.
