# Server Resource

A server resource represents a backend database server.

## Resource Operations

### Get a server

```
GET /v1/servers/:name
```

Get a single server. The _:name_ in the URI must be a valid server name with all
whitespace replaced with hyphens. The server names are case-insensitive.

**Note**: The _parameters_ field contains all custom parameters for
  servers, including the server weighting parameters.

#### Response

```
Status: 200 OK
```

```javascript
{
    "links": {
        "self": "http://localhost:8989/v1/servers/server1"
    },
    "data": {
        "id": "server1", // Resource identifier
        "type": "servers", // Resource type
        "relationships": { // Resource relationships to other resources
            "services": { // Services that use this server
                "links": {
                    "self": "http://localhost:8989/v1/services/"
                },
                "data": [ // References to service resources
                    {
                        "id": "RW-Split-Router",
                        "type": "services"
                    },
                    {
                        "id": "Read-Connection-Router",
                        "type": "services"
                    }
                ]
            },
            "monitors": { // The monitor that is monitoring this server
                "links": {
                    "self": "http://localhost:8989/v1/monitors/"
                },
                "data": [
                    {
                        "id": "MySQL-Monitor",
                        "type": "monitors"
                    }
                ]
            }
        },
        "attributes": { // Resource attributes
            "parameters": { // Server parameters
                "address": "127.0.0.1",
                "port": 3000,
                "protocol": "MySQLBackend"
            },
            "state": "Master, Running", // Server state string
            "version_string": "10.1.22-MariaDB", // Server version
            "node_id": 3000, // Server node ID i.e. value of @@server_id
            "master_id": -1,
            "replication_depth": 0,
            "slaves": [ // List of slave server IDs
                3001
            ],
            "statistics": { // Server statistics
                "connections": 0,
                "total_connections": 0,
                "active_operations": 0
            }
        },
        "links": { // Link to the server itself
            "self": "http://localhost:8989/v1/servers/server1"
        }
    }
}
```

Server not found:

```
Status: 404 Not Found
```

### Get all servers

```
GET /v1/servers
```

#### Response

Response contains a resource collection with all servers.

```
Status: 200 OK
```

```javascript
{
    "links": {
        "self": "http://localhost:8989/v1/servers/"
    },
    "data": [ // List of server resouces
        {
            "id": "server1",
            "type": "servers",
            "relationships": {
                "services": {
                    "links": {
                        "self": "http://localhost:8989/v1/services/"
                    },
                    "data": [
                        {
                            "id": "RW-Split-Router",
                            "type": "services"
                        },
                        {
                            "id": "Read-Connection-Router",
                            "type": "services"
                        }
                    ]
                },
                "monitors": {
                    "links": {
                        "self": "http://localhost:8989/v1/monitors/"
                    },
                    "data": [
                        {
                            "id": "MySQL-Monitor",
                            "type": "monitors"
                        }
                    ]
                }
            },
            "attributes": {
                "parameters": {
                    "address": "127.0.0.1",
                    "port": 3000,
                    "protocol": "MySQLBackend"
                },
                "state": "Master, Running",
                "version_string": "10.1.22-MariaDB",
                "node_id": 3000,
                "master_id": -1,
                "replication_depth": 0,
                "slaves": [
                    3001
                ],
                "statistics": {
                    "connections": 0,
                    "total_connections": 0,
                    "active_operations": 0
                }
            },
            "links": {
                "self": "http://localhost:8989/v1/servers/server1"
            }
        },
        {
            "id": "server2",
            "type": "servers",
            "relationships": {
                "services": {
                    "links": {
                        "self": "http://localhost:8989/v1/services/"
                    },
                    "data": [
                        {
                            "id": "RW-Split-Router",
                            "type": "services"
                        }
                    ]
                },
                "monitors": {
                    "links": {
                        "self": "http://localhost:8989/v1/monitors/"
                    },
                    "data": [
                        {
                            "id": "MySQL-Monitor",
                            "type": "monitors"
                        }
                    ]
                }
            },
            "attributes": {
                "parameters": {
                    "address": "127.0.0.1",
                    "port": 3001,
                    "protocol": "MySQLBackend"
                },
                "state": "Slave, Running",
                "version_string": "10.1.22-MariaDB",
                "node_id": 3001,
                "master_id": 3000,
                "replication_depth": 1,
                "slaves": [],
                "statistics": {
                    "connections": 0,
                    "total_connections": 0,
                    "active_operations": 0
                }
            },
            "links": {
                "self": "http://localhost:8989/v1/servers/server2"
            }
        }
    ]
}
```

### Create a server

```
POST /v1/servers
```

Create a new server by defining the resource. The posted object must define the
_data.id_ field with the name of the server and the
_data.atttributes.parameters_ field with JSON object containing values for the
_address_ and _port_ parameters. The following is the minimal required JSON
object for defining a new server.

```javascript
{
    "data": {
        "id": "server3",
        "type": "servers",
        "attributes": {
            "parameters": {
                "address": "127.0.0.1",
                "port": 3003,
                "protocol": "MySQLBackend"
            }
        }
    }
}
```

The relationships of a server can also be defined at creation time. This allows
new servers to be created and immediately taken into use.

```javascript
{
    "data": {
        "id": "server4",
        "type": "servers",
        "attributes": {
            "parameters": {
                "address": "127.0.0.1",
                "port": 3002,
                "protocol": "MySQLBackend"
            }
        },
        "relationships": {
            "services": {
                "data": [
                    {
                        "id": "RW-Split-Router",
                        "type": "services"
                    },
                    {
                        "id": "Read-Connection-Router",
                        "type": "services"
                    }
                ]
            },
            "monitors": {
                "data": [
                    {
                        "id": "MySQL-Monitor",
                        "type": "monitors"
                    }
                ]
            }
        }
    }
}
```

The following parameters can be defined when a server is being created.

- [address](../Getting-Started/Configuration-Guide.md#address)
- [port](../Getting-Started/Configuration-Guide.md#port)
- [protocol](../Getting-Started/Configuration-Guide.md#protocol)
- [authenticator](../Getting-Started/Configuration-Guide.md#authenticator)
- [authenticator_options](../Getting-Started/Configuration-Guide.md#authenticator-options)

#### Response

Server created:

```
Status: 204 No Content
```

Invalid JSON body:

```
Status: 403 Forbidden
```

### Update a server

```
PATCH /v1/servers/:name
```

The _:name_ in the URI must map to a server name with all whitespace replaced
with hyphens and the request body must be a valid JSON document representing the
modified server. If the server in question is not found, a 404 Not Found
response is returned.

### Modifiable Fields

The following standard server parameters can be modified.

- [address](../Getting-Started/Configuration-Guide.md#address)
- [port](../Getting-Started/Configuration-Guide.md#port)
- [monitoruser](../Getting-Started/Configuration-Guide.md#monitoruser)
- [monitorpw](../Getting-Started/Configuration-Guide.md#monitorpw)

Refer to the documentation on these parameters for valid values.

The server weighting parameters can also be added, removed and updated. To
remove a parameter, define the value of that parameter as the _null_ JSON type
e.g.  `{ "my-param": null }`. To add a parameter, add a new key-value pair to
the _parameters_ object with a name that does not conflict with the standard
parameters. To modify a weighting parameter, simply change the value.

In addition to standard parameters, the _services_ and _monitors_ fields of the
_relationships_ object can be modified. Removal, addition and modification of
the links will change which service and monitors use this server.

For example, removing the first value in the _services_ list in the
_relationships_ object from the following JSON document will remove the
_server1_ from the service _RW-Split-Router_.

Removing a service from a server is analogous to removing the server from the
service. Both unlink the two objects from each other.

Response to `GET /v1/server/server1`:

```javascript
{
    "links": {
        "self": "http://localhost:8989/v1/servers/server1"
    },
    "data": {
        "id": "server1",
        "type": "servers",
        "relationships": {
            "services": {
                "links": {
                    "self": "http://localhost:8989/v1/services/"
                },
                "data": [
                    {
                        "id": "RW-Split-Router", // We'll remove this service
                        "type": "services"
                    },
                    {
                        "id": "Read-Connection-Router",
                        "type": "services"
                    }
                ]
            },
            "monitors": {
                "links": {
                    "self": "http://localhost:8989/v1/monitors/"
                },
                "data": [
                    {
                        "id": "MySQL-Monitor",
                        "type": "monitors"
                    }
                ]
            }
        },
        "attributes": {
            "parameters": {
                "address": "127.0.0.1",
                "port": 3000,
                "protocol": "MySQLBackend"
            },
            "state": "Master, Running",
            "version_string": "10.1.22-MariaDB",
            "node_id": 3000,
            "master_id": -1,
            "replication_depth": 0,
            "slaves": [
                3001,
                3002
            ],
            "statistics": {
                "connections": 0,
                "total_connections": 0,
                "active_operations": 0
            }
        },
        "links": {
            "self": "http://localhost:8989/v1/servers/server1"
        }
    }
}
```

Request for `PUT /v1/server/server1`:

```javascript
{
    "data": {
        "id": "server1",
        "type": "servers",
        "relationships": {
            "services": {
                "data": [
                    {
                        "id": "Read-Connection-Router",
                        "type": "services"
                    }
                ]
            },
            "monitors": {
                "data": [
                    {
                        "id": "MySQL-Monitor",
                        "type": "monitors"
                    }
                ]
            }
        }
    }
}
```

The current implementation accepts PATCH requests with partially defined
resources as request body. If parts of the resource are not defined (e.g. the
`attributes` field in the above example), those parts of the resource are not
modified. All parts that are defined are interpreted as the new definition of
those part of the resource. In the above example, the `relationships` of the
resource are completely redefined.

#### Response

Server modified:

```
Status: 204 No Content
```

Server not found:

```
Status: 404 Not Found
```

Invalid JSON body:

```
Status: 403 Forbidden
```

### Destroy a server

```
DELETE /v1/servers/:name
```

The _:name_ in the URI must map to a server name with all whitespace replaced
with hyphens.

A server can only be deleted if it is not used by any services or monitors.

#### Response

OK:

```
Status: 204 No Content
```

Server not found:

```
Status: 404 Not Found
```

Server is in use:

```
Status: 403 Forbidden
```

### Set server state

```
PUT /v1/servers/:name/set
```

The _:name_ in the URI must map to a server name with all whitespace replaced
with hyphens. This endpoint requires that the `state` parameter is passed with
the request. The value of `state` must be one of the following values.

|Value      | State Description              |
|-----------|--------------------------------|
|master     | Server is a Master             |
|slave      | Server is a Slave              |
|maintenance| Server is put into maintenance |
|running    | Server is up and running       |
|synced     | Server is a Galera node        |
|ndb        | Server is a NDBCluster node    |
|stale      | Server is a stale Master       |

For example, to set the server _db-server-1_ into maintenance mode, a request to
the following URL must be made:

```
PUT /v1/servers/db-server-1/set?state=maintenance
```

#### Response

OK:

```
Status: 204 No Content
```

Server not found:

```
Status: 404 Not Found
```

Missing or invalid parameter:

```
Status: 403 Forbidden
```

### Clear server state

```
POST /v1/servers/:name/clear
```

The _:name_ in the URI must map to a server name with all whitespace replaced
with hyphens. This endpoint requires that the `state` parameter is passed with
the request. The value of `state` must be one of the values defined in the
_set_ endpoint documentation.

#### Response

OK:

```
Status: 204 No Content
```

Server not found:

```
Status: 404 Not Found
```

Missing or invalid parameter:

```
Status: 403 Forbidden
```
