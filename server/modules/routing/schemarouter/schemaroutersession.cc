/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "schemarouter.hh"

#include <maxscale/alloc.h>
#include <maxscale/query_classifier.h>
#include <maxscale/modutil.h>

#include "schemaroutersession.hh"
#include "schemarouterinstance.hh"

bool connect_backend_servers(BackendList& backends, MXS_SESSION* session);

enum route_target get_shard_route_target(uint32_t qtype);
bool change_current_db(string& dest, Shard& shard, GWBUF* buf);
bool extract_database(GWBUF* buf, char* str);
bool detect_show_shards(GWBUF* query);
void write_error_to_client(DCB* dcb, int errnum, const char* mysqlstate, const char* errmsg);

Backend::Backend(SERVER_REF *ref):
    m_backend(ref),
    m_dcb(NULL),
    m_map_queue(NULL),
    m_mapped(false),
    m_num_mapping_eof(0),
    m_num_result_wait(0),
    m_pending_cmd(NULL),
    m_state(0)
{
}

Backend::~Backend()
{
    gwbuf_free(m_map_queue);
    gwbuf_free(m_pending_cmd);
}


bool Backend::execute_sescmd()
{
    if (BREF_IS_CLOSED(this))
    {
        return false;
    }

    CHK_DCB(m_dcb);

    int rc = 0;

    /** Return if there are no pending ses commands */
    if (m_session_commands.size() == 0)
    {
        MXS_INFO("Cursor had no pending session commands.");
        return false;
    }

    SessionCommandList::iterator iter = m_session_commands.begin();
    GWBUF *buffer = iter->copy_buffer().release();

    switch (iter->get_command())
    {
    case MYSQL_COM_CHANGE_USER:
        /** This makes it possible to handle replies correctly */
        gwbuf_set_type(buffer, GWBUF_TYPE_SESCMD);
        rc = m_dcb->func.auth(m_dcb, NULL, m_dcb->session, buffer);
        break;

    case MYSQL_COM_QUERY:
    default:
        /**
         * Mark session command buffer, it triggers writing
         * MySQL command to protocol
         */
        gwbuf_set_type(buffer, GWBUF_TYPE_SESCMD);
        rc = m_dcb->func.write(m_dcb, buffer);
        break;
    }

    return rc == 1;
}

void Backend::clear_state(enum bref_state state)
{
    if (state != BREF_WAITING_RESULT)
    {
        m_state &= ~state;
    }
    else
    {
        /** Decrease global operation count */
        ss_debug(int prev2 = )atomic_add(&m_backend->server->stats.n_current_ops, -1);
        ss_dassert(prev2 > 0);
    }
}

void Backend::set_state(enum bref_state state)
{
    if (state != BREF_WAITING_RESULT)
    {
        m_state |= state;
    }
    else
    {
        /** Increase global operation count */
        ss_debug(int prev2 = )atomic_add(&m_backend->server->stats.n_current_ops, 1);
        ss_dassert(prev2 >= 0);
    }
}

SchemaRouterSession::SchemaRouterSession(MXS_SESSION* session, SchemaRouter* router):
    mxs::RouterSession(session),
    m_closed(false),
    m_client(session->client_dcb),
    m_mysql_session((MYSQL_session*)session->client_dcb->data),
    m_config(&m_router->m_config),
    m_router(router),
    m_shard(m_router->m_shard_manager.get_shard(m_client->user, m_config->refresh_min_interval)),
    m_state(0),
    m_sent_sescmd(0),
    m_replied_sescmd(0)
{
    char db[MYSQL_DATABASE_MAXLEN + 1] = "";
    MySQLProtocol* protocol = (MySQLProtocol*)session->client_dcb->protocol;
    MYSQL_session* data = (MYSQL_session*)session->client_dcb->data;
    bool using_db = false;
    bool have_db = false;

    /* To enable connecting directly to a sharded database we first need
     * to disable it for the client DCB's protocol so that we can connect to them*/
    if (protocol->client_capabilities & GW_MYSQL_CAPABILITIES_CONNECT_WITH_DB &&
        (have_db = strnlen(data->db, MYSQL_DATABASE_MAXLEN) > 0))
    {
        protocol->client_capabilities &= ~GW_MYSQL_CAPABILITIES_CONNECT_WITH_DB;
        strcpy(db, data->db);
        *data->db = 0;
        using_db = true;
        MXS_INFO("Client logging in directly to a database '%s', "
                 "postponing until databases have been mapped.", db);
    }

    if (!have_db)
    {
        MXS_INFO("Client'%s' connecting with empty database.", data->user);
    }

    if (using_db)
    {
        m_state |= INIT_USE_DB;
    }

    for (SERVER_REF *ref = m_router->m_service->dbref; ref; ref = ref->next)
    {
        if (ref->active)
        {
            m_backends.push_back(Backend(ref));
        }
    }

    if (!connect_backend_servers(m_backends, session))
    {
        // TODO: Figure out how to avoid this throw
        throw std::runtime_error("Failed to connect to backend servers");
    }

    if (db[0])
    {
        /* Store the database the client is connecting to */
        m_connect_db = db;
    }

    atomic_add(&m_router->m_stats.sessions, 1);
}

SchemaRouterSession::~SchemaRouterSession()
{
}

void SchemaRouterSession::close()
{
    ss_dassert(!m_closed);

    /**
     * Lock router client session for secure read and update.
     */
    if (!m_closed)
    {
        m_closed = true;

        for (BackendList::iterator it = m_backends.begin(); it != m_backends.end(); it++)
        {
            DCB* dcb = it->m_dcb;
            /** Close those which had been connected */
            if (BREF_IS_IN_USE(it))
            {
                CHK_DCB(dcb);

                /** Clean operation counter in bref and in SERVER */
                while (BREF_IS_WAITING_RESULT(it))
                {
                    it->clear_state(BREF_WAITING_RESULT);
                }
                it->clear_state(BREF_IN_USE);
                it->set_state(BREF_CLOSED);
                /**
                 * closes protocol and dcb
                 */
                dcb_close(dcb);
                /** decrease server current connection counters */
                atomic_add(&it->m_backend->connections, -1);
            }
        }

        spinlock_acquire(&m_router->m_lock);
        if (m_router->m_stats.longest_sescmd < m_stats.longest_sescmd)
        {
            m_router->m_stats.longest_sescmd = m_stats.longest_sescmd;
        }
        double ses_time = difftime(time(NULL), m_client->session->stats.connect);
        if (m_router->m_stats.ses_longest < ses_time)
        {
            m_router->m_stats.ses_longest = ses_time;
        }
        if (m_router->m_stats.ses_shortest > ses_time && m_router->m_stats.ses_shortest > 0)
        {
            m_router->m_stats.ses_shortest = ses_time;
        }

        m_router->m_stats.ses_average =
            (ses_time + ((m_router->m_stats.sessions - 1) * m_router->m_stats.ses_average)) /
            (m_router->m_stats.sessions);

        spinlock_release(&m_router->m_lock);
    }
}

static void inspect_query(GWBUF* pPacket, uint32_t* type, qc_query_op_t* op, uint8_t* command)
{
    uint8_t* data = GWBUF_DATA(pPacket);
    *command = data[4];

    switch (*command)
    {
    case MYSQL_COM_QUIT: /*< 1 QUIT will close all sessions */
    case MYSQL_COM_INIT_DB: /*< 2 DDL must go to the master */
    case MYSQL_COM_REFRESH: /*< 7 - I guess this is session but not sure */
    case MYSQL_COM_DEBUG: /*< 0d all servers dump debug info to stdout */
    case MYSQL_COM_PING: /*< 0e all servers are pinged */
    case MYSQL_COM_CHANGE_USER: /*< 11 all servers change it accordingly */
    case MYSQL_COM_STMT_CLOSE: /*< free prepared statement */
    case MYSQL_COM_STMT_SEND_LONG_DATA: /*< send data to column */
    case MYSQL_COM_STMT_RESET: /*< resets the data of a prepared statement */
        *type = QUERY_TYPE_SESSION_WRITE;
        break;

    case MYSQL_COM_CREATE_DB: /**< 5 DDL must go to the master */
    case MYSQL_COM_DROP_DB: /**< 6 DDL must go to the master */
        *type = QUERY_TYPE_WRITE;
        break;

    case MYSQL_COM_QUERY:
        *type = qc_get_type_mask(pPacket);
        *op = qc_get_operation(pPacket);
        break;

    case MYSQL_COM_STMT_PREPARE:
        *type = qc_get_type_mask(pPacket);
        *type |= QUERY_TYPE_PREPARE_STMT;
        break;

    case MYSQL_COM_STMT_EXECUTE:
        /** Parsing is not needed for this type of packet */
        *type = QUERY_TYPE_EXEC_STMT;
        break;

    case MYSQL_COM_SHUTDOWN: /**< 8 where should shutdown be routed ? */
    case MYSQL_COM_STATISTICS: /**< 9 ? */
    case MYSQL_COM_PROCESS_INFO: /**< 0a ? */
    case MYSQL_COM_CONNECT: /**< 0b ? */
    case MYSQL_COM_PROCESS_KILL: /**< 0c ? */
    case MYSQL_COM_TIME: /**< 0f should this be run in gateway ? */
    case MYSQL_COM_DELAYED_INSERT: /**< 10 ? */
    case MYSQL_COM_DAEMON: /**< 1d ? */
    default:
        break;
    }

    if (MXS_LOG_PRIORITY_IS_ENABLED(LOG_INFO))
    {
        char *sql;
        int sql_len;
        char* qtypestr = qc_typemask_to_string(*type);
        modutil_extract_SQL(pPacket, &sql, &sql_len);

        MXS_INFO("> Command: %s, stmt: %.*s %s%s",
                 STRPACKETTYPE(*command), sql_len, sql,
                 (pPacket->hint == NULL ? "" : ", Hint:"),
                 (pPacket->hint == NULL ? "" : STRHINTTYPE(pPacket->hint->type)));

        MXS_FREE(qtypestr);
    }
}

SERVER* SchemaRouterSession::resolve_query_target(GWBUF* pPacket,
                                                  uint32_t type,
                                                  uint8_t command,
                                                  enum route_target& route_target)
{
    SERVER* target = NULL;

    if (route_target != TARGET_NAMED_SERVER)
    {
        /** We either don't know or don't care where this query should go */
        target = get_shard_target(pPacket, type);

        if (target && SERVER_IS_RUNNING(target))
        {
            route_target = TARGET_NAMED_SERVER;
        }
    }

    if (TARGET_IS_UNDEFINED(route_target))
    {
        /** We don't know where to send this. Route it to either the server with
         * the current default database or to the first available server. */
        target = get_shard_target(pPacket, type);

        if ((target == NULL && command != MYSQL_COM_INIT_DB && m_current_db.length() == 0) ||
            command == MYSQL_COM_FIELD_LIST ||
            m_current_db.length() == 0)
        {
            /** No current database and no databases in query or the database is
             * ignored, route to first available backend. */
            route_target = TARGET_ANY;
        }
    }

    if (TARGET_IS_ANY(route_target))
    {
        for (BackendList::iterator it = m_backends.begin(); it != m_backends.end(); it++)
        {
            SERVER *server = it->m_backend->server;
            if (SERVER_IS_RUNNING(server))
            {
                route_target = TARGET_NAMED_SERVER;
                target = server;
                break;
            }
        }

        if (TARGET_IS_ANY(route_target))
        {
            /**No valid backends alive*/
            MXS_ERROR("Failed to route query, no backends are available.");
        }
    }

    return target;
}

int32_t SchemaRouterSession::routeQuery(GWBUF* pPacket)
{
    ss_dassert(!GWBUF_IS_TYPE_UNDEFINED(pPacket));

    if (m_closed)
    {
        return 0;
    }

    if (m_shard.empty())
    {
        /* Generate database list */
        gen_databaselist();
    }

    int ret = 0;

    /**
     * If the databases are still being mapped or if the client connected
     * with a default database but no database mapping was performed we need
     * to store the query. Once the databases have been mapped and/or the
     * default database is taken into use we can send the query forward.
     */
    if (m_state & (INIT_MAPPING | INIT_USE_DB))
    {
        m_queue.push_back(pPacket);

        if (m_state  == (INIT_READY | INIT_USE_DB))
        {
            /**
             * This state is possible if a client connects with a default database
             * and the shard map was found from the router cache
             */
            if (handle_default_db())
            {
                ret = 1;
            }
        }

        return ret;
    }

    uint8_t command = 0;
    SERVER* target = NULL;
    uint32_t type = QUERY_TYPE_UNKNOWN;
    qc_query_op_t op = QUERY_OP_UNDEFINED;
    enum route_target route_target = TARGET_UNDEFINED;

    inspect_query(pPacket, &type, &op, &command);

    /** Create the response to the SHOW DATABASES from the mapped databases */
    if (qc_query_is_type(type, QUERY_TYPE_SHOW_DATABASES))
    {
        if (send_database_list())
        {
            ret = 1;
        }

        gwbuf_free(pPacket);
        return ret;
    }
    else if (detect_show_shards(pPacket))
    {
        if (process_show_shards())
        {
            ret = 1;
        }
        gwbuf_free(pPacket);
        return ret;
    }

    /** The default database changes must be routed to a specific server */
    if (command == MYSQL_COM_INIT_DB || op == QUERY_OP_CHANGE_DB)
    {
        if (!change_current_db(m_current_db, m_shard, pPacket))
        {
            char db[MYSQL_DATABASE_MAXLEN + 1];
            extract_database(pPacket, db);
            gwbuf_free(pPacket);

            char errbuf[128 + MYSQL_DATABASE_MAXLEN];
            snprintf(errbuf, sizeof(errbuf), "Unknown database: %s", db);

            if (m_config->debug)
            {
                sprintf(errbuf + strlen(errbuf),
                        " ([%lu]: DB change failed)",
                        m_client->session->ses_id);
            }

            write_error_to_client(m_client,
                                  SCHEMA_ERR_DBNOTFOUND,
                                  SCHEMA_ERRSTR_DBNOTFOUND,
                                  errbuf);
            return 1;
        }

        route_target = TARGET_UNDEFINED;
        target = m_shard.get_location(m_current_db);

        if (target)
        {
            MXS_INFO("INIT_DB for database '%s' on server '%s'",
                     m_current_db.c_str(), target->unique_name);
            route_target = TARGET_NAMED_SERVER;
        }
        else
        {
            MXS_INFO("INIT_DB with unknown database");
        }
    }
    else
    {
        route_target = get_shard_route_target(type);
    }

    /**
     * Find a suitable server that matches the requirements of @c route_target
     */

    if (TARGET_IS_ALL(route_target))
    {
        /** Session commands, route to all servers */
        if (route_session_write(pPacket, command))
        {
            atomic_add(&m_router->m_stats.n_sescmd, 1);
            atomic_add(&m_router->m_stats.n_queries, 1);
            ret = 1;
        }
    }
    else
    {
        target = resolve_query_target(pPacket, type, command, route_target);
    }

    DCB* target_dcb = NULL;

    if (TARGET_IS_NAMED_SERVER(route_target) && target &&
        get_shard_dcb(&target_dcb, target->unique_name))
    {
        /** We know where to route this query */
        Backend *bref = get_bref_from_dcb(target_dcb);

        MXS_INFO("Route query to \t%s:%d <", bref->m_backend->server->name, bref->m_backend->server->port);

        if (bref->m_session_commands.size() > 0)
        {
            /** Store current statement if execution of the previous
             * session command hasn't been completed. */
            ss_dassert((bref->m_pending_cmd == NULL || m_closed));
            bref->m_pending_cmd = pPacket;
            ret = 1;
        }
        else if ((ret = target_dcb->func.write(target_dcb, gwbuf_clone(pPacket))) == 1)
        {
            Backend* bref;

            atomic_add(&m_router->m_stats.n_queries, 1);

            /**
             * Add one query response waiter to backend reference
             */
            bref = get_bref_from_dcb(target_dcb);
            bref->set_state(BREF_QUERY_ACTIVE);
            bref->set_state(BREF_WAITING_RESULT);
        }
        else
        {
            MXS_ERROR("Routing query failed.");
        }
    }

    gwbuf_free(pPacket);
    return ret;
}
void SchemaRouterSession::handle_mapping_reply(Backend* bref, GWBUF* pPacket)
{
    int rc = inspect_backend_mapping_states(bref, &pPacket);

    if (rc == 1)
    {
        synchronize_shard_map();
        m_state &= ~INIT_MAPPING;

        /* Check if the session is reconnecting with a database name
         * that is not in the hashtable. If the database is not found
         * then close the session. */

        if (m_state & INIT_USE_DB)
        {
            if (!handle_default_db())
            {
                rc = -1;
            }
        }

        if (m_queue.size() && rc != -1)
        {
            ss_dassert(m_state == INIT_READY);
            route_queued_query();
        }
    }

    if (rc == -1)
    {
        poll_fake_hangup_event(m_client);
    }
}

void SchemaRouterSession::process_response(Backend* bref, GWBUF** ppPacket)
{
    if (bref->m_session_commands.size() > 0)
    {
        /** We are executing a session command */
        if (GWBUF_IS_TYPE_SESCMD_RESPONSE((*ppPacket)))
        {
            if (m_replied_sescmd < m_sent_sescmd &&
                bref->m_session_commands.front().get_position() == m_replied_sescmd + 1)
            {
                /** First reply to this session command, route it to the client */
                ++m_replied_sescmd;
            }
            else
            {
                /** The reply to this session command has already been sent to
                 * the client, discard it */
                gwbuf_free(*ppPacket);
                *ppPacket = NULL;
            }

            bref->m_session_commands.pop_front();
        }

        if (*ppPacket)
        {
            bref->clear_state(BREF_WAITING_RESULT);
        }
    }
    else if (BREF_IS_QUERY_ACTIVE(bref))
    {
        bref->clear_state(BREF_QUERY_ACTIVE);
        /** Set response status as replied */
        bref->clear_state(BREF_WAITING_RESULT);
    }
}

void SchemaRouterSession::clientReply(GWBUF* pPacket, DCB* pDcb)
{
    Backend* bref = get_bref_from_dcb(pDcb);

    if (m_closed || bref == NULL)
    {
        gwbuf_free(pPacket);
        return;
    }

    MXS_DEBUG("Reply from [%s] session [%p]"
              " mapping [%s] queries queued [%s]",
              bref->m_backend->server->unique_name,
              m_client->session,
              m_state & INIT_MAPPING ? "true" : "false",
              m_queue.size() == 0 ? "none" :
              m_queue.size() > 0 ? "multiple" : "one");

    if (m_state & INIT_MAPPING)
    {
        handle_mapping_reply(bref, pPacket);
    }
    else if (m_state & INIT_USE_DB)
    {
        MXS_DEBUG("Reply to USE '%s' received for session %p",
                  m_connect_db.c_str(), m_client->session);
        m_state &= ~INIT_USE_DB;
        m_current_db = m_connect_db;
        ss_dassert(m_state == INIT_READY);

        if (m_queue.size())
        {
            route_queued_query();
        }
    }

    else if (m_queue.size())
    {
        ss_dassert(m_state == INIT_READY);
        route_queued_query();
    }
    else
    {
        process_response(bref, &pPacket);

        if (pPacket)
        {
            MXS_SESSION_ROUTE_REPLY(pDcb->session, pPacket);
            pPacket = NULL;
        }

        if (bref->m_session_commands.size() > 0)
        {
            /** There are pending session commands to be executed. */
            MXS_INFO("Backend %s:%d processed reply and starts to execute active cursor.",
                     bref->m_backend->server->name, bref->m_backend->server->port);
            bref->execute_sescmd();
        }
        else if (bref->m_pending_cmd) /*< non-sescmd is waiting to be routed */
        {
            CHK_GWBUF(bref->m_pending_cmd);
            int ret = bref->m_dcb->func.write(bref->m_dcb, bref->m_pending_cmd);
            bref->m_pending_cmd = NULL;

            if (ret == 1)
            {
                atomic_add(&m_router->m_stats.n_queries, 1);
                bref->set_state(BREF_QUERY_ACTIVE);
                bref->set_state(BREF_WAITING_RESULT);
            }
            else
            {
                MXS_ERROR("Routing of pending query failed.");
            }
        }
    }

    gwbuf_free(pPacket);
}

void SchemaRouterSession::handleError(GWBUF* pMessage,
                                      DCB* pProblem,
                                      mxs_error_action_t action,
                                      bool* pSuccess)
{
    ss_dassert(pProblem->dcb_role == DCB_ROLE_BACKEND_HANDLER);
    CHK_DCB(pProblem);
    MXS_SESSION *session = pProblem->session;
    ss_dassert(session);

    CHK_SESSION(session);

    switch (action)
    {
    case ERRACT_NEW_CONNECTION:
        *pSuccess = handle_error_new_connection(pProblem, pMessage);
        break;

    case ERRACT_REPLY_CLIENT:
        handle_error_reply_client(pProblem, pMessage);
        *pSuccess = false; /*< no new backend servers were made available */
        break;

    default:
        *pSuccess = false;
        break;
    }

    dcb_close(pProblem);
}

/**
 * Private functions
 */


/**
 * Synchronize the router client session shard map with the global shard map for
 * this user.
 *
 * If the router doesn't have a shard map for this user then the current shard map
 * of the client session is added to the m_router-> If the shard map in the router is
 * out of date, its contents are replaced with the contents of the current client
 * session. If the router has a usable shard map, the current shard map of the client
 * is discarded and the router's shard map is used.
 * @param client Router session
 */
void SchemaRouterSession::synchronize_shard_map()
{
    m_router->m_stats.shmap_cache_miss++;
    m_router->m_shard_manager.update_shard(m_shard, m_client->user);
}

/**
 * Extract the database name from a COM_INIT_DB or literal USE ... query.
 * @param buf Buffer with the database change query
 * @param str Pointer where the database name is copied
 * @return True for success, false for failure
 */
bool extract_database(GWBUF* buf, char* str)
{
    uint8_t* packet;
    char *saved, *tok, *query = NULL;
    bool succp = true;
    unsigned int plen;

    packet = GWBUF_DATA(buf);
    plen = gw_mysql_get_byte3(packet) - 1;

    /** Copy database name from MySQL packet to session */
    if (qc_get_operation(buf) == QUERY_OP_CHANGE_DB)
    {
        const char *delim = "` \n\t;";

        query = modutil_get_SQL(buf);
        tok = strtok_r(query, delim, &saved);

        if (tok == NULL || strcasecmp(tok, "use") != 0)
        {
            MXS_ERROR("extract_database: Malformed chage database packet.");
            succp = false;
            goto retblock;
        }

        tok = strtok_r(NULL, delim, &saved);

        if (tok == NULL)
        {
            MXS_ERROR("extract_database: Malformed change database packet.");
            succp = false;
            goto retblock;
        }

        strncpy(str, tok, MYSQL_DATABASE_MAXLEN);
    }
    else
    {
        memcpy(str, packet + 5, plen);
        memset(str + plen, 0, 1);
    }
retblock:
    MXS_FREE(query);
    return succp;
}

/**
 * Execute in backends used by current router session.
 * Save session variable commands to router session property
 * struct. Thus, they can be replayed in backends which are
 * started and joined later.
 *
 * Suppress redundant OK packets sent by backends.
 *
 * The first OK packet is replied to the client.
 * Return true if succeed, false is returned if router session was closed or
 * if execute_sescmd_in_backend failed.
 */
bool SchemaRouterSession::route_session_write(GWBUF* querybuf, uint8_t command)
{
    bool succp = false;

    MXS_INFO("Session write, routing to all servers.");
    atomic_add(&m_stats.longest_sescmd, 1);

    /** Increment the session command count */
    ++m_sent_sescmd;

    for (BackendList::iterator it = m_backends.begin(); it != m_backends.end(); it++)
    {
        if (BREF_IS_IN_USE(it))
        {
            GWBUF *buffer = gwbuf_clone(querybuf);
            it->m_session_commands.push_back(SessionCommand(buffer, m_sent_sescmd));

            if (MXS_LOG_PRIORITY_IS_ENABLED(LOG_INFO))
            {
                MXS_INFO("Route query to %s\t%s:%d",
                         SERVER_IS_MASTER(it->m_backend->server) ? "master" : "slave",
                         it->m_backend->server->name,
                         it->m_backend->server->port);
            }

            if (it->m_session_commands.size() == 1)
            {
                /** Only one command, execute it */
                switch (command)
                {
                /** These types of commands don't generate responses */
                case MYSQL_COM_QUIT:
                case MYSQL_COM_STMT_CLOSE:
                    break;

                default:
                    it->set_state(BREF_WAITING_RESULT);
                    break;
                }

                if (it->execute_sescmd())
                {
                    succp = true;
                }
                else
                {
                    MXS_ERROR("Failed to execute session "
                              "command in %s:%d",
                              it->m_backend->server->name,
                              it->m_backend->server->port);
                }
            }
            else
            {
                ss_dassert(it->m_session_commands.size() > 1);
                /** The server is already executing a session command */
                MXS_INFO("Backend %s:%d already executing sescmd.",
                         it->m_backend->server->name,
                         it->m_backend->server->port);
                succp = true;
            }
        }
    }

    return succp;
}

void SchemaRouterSession::handle_error_reply_client(DCB* dcb, GWBUF* errmsg)
{
    Backend*  bref = get_bref_from_dcb(dcb);

    if (bref)
    {

        bref->clear_state(BREF_IN_USE);
        bref->set_state(BREF_CLOSED);
    }

    if (dcb->session->state == SESSION_STATE_ROUTER_READY)
    {
        dcb->session->client_dcb->func.write(dcb->session->client_dcb, gwbuf_clone(errmsg));
    }
}

/**
 * Check if a router session has servers in use
 * @param rses Router client session
 * @return True if session has a single backend server in use that is running.
 * False if no backends are in use or running.
 */
bool SchemaRouterSession::have_servers()
{
    for (BackendList::iterator it = m_backends.begin(); it != m_backends.end(); it++)
    {
        if (BREF_IS_IN_USE(it) && !BREF_IS_CLOSED(it))
        {
            return true;
        }
    }

    return false;
}

/**
 * Check if there is backend reference pointing at failed DCB, and reset its
 * flags. Then clear DCB's callback and finally try to reconnect.
 *
 * This must be called with router lock.
 *
 * @param inst          router instance
 * @param rses          router client session
 * @param dcb           failed DCB
 * @param errmsg        error message which is sent to client if it is waiting
 *
 * @return true if there are enough backend connections to continue, false if not
 */
bool SchemaRouterSession::handle_error_new_connection(DCB* backend_dcb, GWBUF* errmsg)
{
    MXS_SESSION *ses = backend_dcb->session;
    CHK_SESSION(ses);

    Backend* bref = get_bref_from_dcb(backend_dcb);

    if (bref == NULL)
    {
        /** This should not happen */
        ss_dassert(false);
        return false;
    }

    /**
     * If query was sent through the bref and it is waiting for reply from
     * the backend server it is necessary to send an error to the client
     * because it is waiting for reply.
     */
    if (BREF_IS_WAITING_RESULT(bref))
    {
        DCB* client_dcb;
        client_dcb = ses->client_dcb;
        client_dcb->func.write(client_dcb, gwbuf_clone(errmsg));
        bref->clear_state(BREF_WAITING_RESULT);
    }
    bref->clear_state(BREF_IN_USE);
    bref->set_state(BREF_CLOSED);

    return have_servers();
}

/**
 * Finds out if there is a backend reference pointing at the DCB given as
 * parameter.
 * @param rses  router client session
 * @param dcb   DCB
 *
 * @return backend reference pointer if succeed or NULL
 */
Backend* SchemaRouterSession::get_bref_from_dcb(DCB* dcb)
{
    CHK_DCB(dcb);

    for (BackendList::iterator it = m_backends.begin(); it != m_backends.end(); it++)
    {
        if (it->m_dcb == dcb)
        {
            return &(*it);
        }
    }

    return NULL;
}

/**
 * Detect if a query contains a SHOW SHARDS query.
 * @param query Query to inspect
 * @return true if the query is a SHOW SHARDS query otherwise false
 */
bool detect_show_shards(GWBUF* query)
{
    bool rval = false;
    char *querystr, *tok, *sptr;

    if (query == NULL)
    {
        MXS_ERROR("NULL value passed at %s:%d", __FILE__, __LINE__);
        return false;
    }

    if (!modutil_is_SQL(query) && !modutil_is_SQL_prepare(query))
    {
        return false;
    }

    if ((querystr = modutil_get_SQL(query)) == NULL)
    {
        MXS_ERROR("Failure to parse SQL at  %s:%d", __FILE__, __LINE__);
        return false;
    }

    tok = strtok_r(querystr, " ", &sptr);
    if (tok && strcasecmp(tok, "show") == 0)
    {
        tok = strtok_r(NULL, " ", &sptr);
        if (tok && strcasecmp(tok, "shards") == 0)
        {
            rval = true;
        }
    }

    MXS_FREE(querystr);
    return rval;
}

/**
 * Callback for the shard list result set creation
 */
RESULT_ROW* shard_list_cb(struct resultset* rset, void* data)
{
    ServerMap* pContent = (ServerMap*)data;
    RESULT_ROW* rval = resultset_make_row(rset);

    if (rval)
    {
        resultset_row_set(rval, 0, pContent->begin()->first.c_str());
        resultset_row_set(rval, 1, pContent->begin()->second->unique_name);
        pContent->erase(pContent->begin());
    }

    return rval;
}

/**
 * Send a result set of all shards and their locations to the client.
 * @param rses Router client session
 * @return 0 on success, -1 on error
 */
bool SchemaRouterSession::process_show_shards()
{
    bool rval = false;

    ServerMap pContent;
    m_shard.get_content(pContent);
    RESULTSET* rset = resultset_create(shard_list_cb, &pContent);

    if (rset)
    {
        resultset_add_column(rset, "Database", MYSQL_DATABASE_MAXLEN, COL_TYPE_VARCHAR);
        resultset_add_column(rset, "Server", MYSQL_DATABASE_MAXLEN, COL_TYPE_VARCHAR);
        resultset_stream_mysql(rset, m_client);
        resultset_free(rset);
        rval = true;
    }

    return rval;
}

/**
 *
 * @param dcb
 * @param errnum
 * @param mysqlstate
 * @param errmsg
 */
void write_error_to_client(DCB* dcb, int errnum, const char* mysqlstate, const char* errmsg)
{
    GWBUF* errbuff = modutil_create_mysql_err_msg(1, 0, errnum, mysqlstate, errmsg);
    if (errbuff)
    {
        if (dcb->func.write(dcb, errbuff) != 1)
        {
            MXS_ERROR("Failed to write error packet to client.");
        }
    }
    else
    {
        MXS_ERROR("Memory allocation failed when creating error packet.");
    }
}

/**
 *
 * @param router_cli_ses
 * @return
 */
bool SchemaRouterSession::handle_default_db()
{
    bool rval = false;
    SERVER* target = m_shard.get_location(m_connect_db);

    if (target)
    {
        /* Send a COM_INIT_DB packet to the server with the right database
         * and set it as the client's active database */

        unsigned int qlen = m_connect_db.length();
        GWBUF* buffer = gwbuf_alloc(qlen + 5);

        if (buffer)
        {
            uint8_t *data = GWBUF_DATA(buffer);
            gw_mysql_set_byte3(data, qlen + 1);
            gwbuf_set_type(buffer, GWBUF_TYPE_MYSQL);
            data[3] = 0x0;
            data[4] = 0x2;
            memcpy(data + 5, m_connect_db.c_str(), qlen);
            DCB* dcb = NULL;

            if (get_shard_dcb(&dcb, target->unique_name))
            {
                dcb->func.write(dcb, buffer);
                MXS_DEBUG("USE '%s' sent to %s for session %p",
                          m_connect_db.c_str(),
                          target->unique_name,
                          m_client->session);
                rval = true;
            }
            else
            {
                MXS_INFO("Couldn't find target DCB for '%s'.", target->unique_name);
            }
        }
        else
        {
            MXS_ERROR("Buffer allocation failed.");
        }
    }
    else
    {
        /** Unknown database, hang up on the client*/
        MXS_INFO("Connecting to a non-existent database '%s'", m_connect_db.c_str());
        char errmsg[128 + MYSQL_DATABASE_MAXLEN + 1];
        sprintf(errmsg, "Unknown database '%s'", m_connect_db.c_str());
        if (m_config->debug)
        {
            sprintf(errmsg + strlen(errmsg), " ([%lu]: DB not found on connect)",
                    m_client->session->ses_id);
        }
        write_error_to_client(m_client,
                              SCHEMA_ERR_DBNOTFOUND,
                              SCHEMA_ERRSTR_DBNOTFOUND,
                              errmsg);
    }

    return rval;
}

void SchemaRouterSession::route_queued_query()
{
    GWBUF* tmp = m_queue.front().release();
    m_queue.pop_front();

#ifdef SS_DEBUG
    char* querystr = modutil_get_SQL(tmp);
    MXS_DEBUG("Sending queued buffer for session %p: %s",
              m_client->session,
              querystr);
    MXS_FREE(querystr);
#endif

    poll_add_epollin_event_to_dcb(m_client, tmp);
}

/**
 *
 * @param router_cli_ses Router client session
 * @return 1 if mapping is done, 0 if it is still ongoing and -1 on error
 */
int SchemaRouterSession::inspect_backend_mapping_states(Backend *bref,
                                                        GWBUF** wbuf)
{
    bool mapped = true;
    GWBUF* writebuf = *wbuf;

    for (BackendList::iterator it = m_backends.begin(); it != m_backends.end(); it++)
    {
        if (bref->m_dcb == it->m_dcb && !BREF_IS_MAPPED(it))
        {
            if (bref->m_map_queue)
            {
                writebuf = gwbuf_append(bref->m_map_queue, writebuf);
                bref->m_map_queue = NULL;
            }
            enum showdb_response rc = parse_showdb_response(&(*it),
                                                            &writebuf);
            if (rc == SHOWDB_FULL_RESPONSE)
            {
                it->m_mapped = true;
                MXS_DEBUG("Received SHOW DATABASES reply from %s for session %p",
                          it->m_backend->server->unique_name,
                          m_client->session);
            }
            else if (rc == SHOWDB_PARTIAL_RESPONSE)
            {
                bref->m_map_queue = writebuf;
                writebuf = NULL;
                MXS_DEBUG("Received partial SHOW DATABASES reply from %s for session %p",
                          it->m_backend->server->unique_name,
                          m_client->session);
            }
            else
            {
                DCB* client_dcb = NULL;

                if ((m_state & INIT_FAILED) == 0)
                {
                    if (rc == SHOWDB_DUPLICATE_DATABASES)
                    {
                        MXS_ERROR("Duplicate databases found, closing session.");
                    }
                    else
                    {
                        MXS_ERROR("Fatal error when processing SHOW DATABASES response, closing session.");
                    }
                    client_dcb = m_client;

                    /** This is the first response to the database mapping which
                     * has duplicate database conflict. Set the initialization bitmask
                     * to INIT_FAILED */
                    m_state |= INIT_FAILED;

                    /** Send the client an error about duplicate databases
                     * if there is a queued query from the client. */
                    if (m_queue.size())
                    {
                        GWBUF* error = modutil_create_mysql_err_msg(1, 0,
                                                                    SCHEMA_ERR_DUPLICATEDB,
                                                                    SCHEMA_ERRSTR_DUPLICATEDB,
                                                                    "Error: duplicate databases "
                                                                    "found on two different shards.");

                        if (error)
                        {
                            client_dcb->func.write(client_dcb, error);
                        }
                        else
                        {
                            MXS_ERROR("Creating buffer for error message failed.");
                        }
                    }
                }
                *wbuf = writebuf;
                return -1;
            }
        }

        if (BREF_IS_IN_USE(it) && !BREF_IS_MAPPED(it))
        {
            mapped = false;
            MXS_DEBUG("Still waiting for reply to SHOW DATABASES from %s for session %p",
                      it->m_backend->server->unique_name,
                      m_client->session);
        }
    }
    *wbuf = writebuf;
    return mapped ? 1 : 0;
}

/**
 * Create a fake error message from a DCB.
 * @param fail_str Custom error message
 * @param dcb DCB to use as the origin of the error
 */
void create_error_reply(char* fail_str, DCB* dcb)
{
    MXS_INFO("change_current_db: failed to change database: %s", fail_str);
    GWBUF* errbuf = modutil_create_mysql_err_msg(1, 0, 1049, "42000", fail_str);

    if (errbuf == NULL)
    {
        MXS_ERROR("Creating buffer for error message failed.");
        return;
    }
    /** Set flags that help router to identify session commands reply */
    gwbuf_set_type(errbuf, GWBUF_TYPE_MYSQL);
    gwbuf_set_type(errbuf, GWBUF_TYPE_SESCMD_RESPONSE);
    gwbuf_set_type(errbuf, GWBUF_TYPE_RESPONSE_END);

    poll_add_epollin_event_to_dcb(dcb, errbuf);
}

/**
 * Read new database name from MYSQL_COM_INIT_DB packet or a literal USE ... COM_QUERY
 * packet, check that it exists in the hashtable and copy its name to MYSQL_session.
 *
 * @param dest Destination where the database name will be written
 * @param dbhash Hashtable containing valid databases
 * @param buf   Buffer containing the database change query
 *
 * @return true if new database is set, false if non-existent database was tried
 * to be set
 */
bool change_current_db(string& dest, Shard& shard, GWBUF* buf)
{
    bool succp = false;
    char db[MYSQL_DATABASE_MAXLEN + 1];

    if (GWBUF_LENGTH(buf) <= MYSQL_DATABASE_MAXLEN - 5)
    {
        /** Copy database name from MySQL packet to session */
        if (extract_database(buf, db))
        {
            MXS_INFO("change_current_db: INIT_DB with database '%s'", db);
            /**
             * Update the session's active database only if it's in the hashtable.
             * If it isn't found, send a custom error packet to the client.
             */

            SERVER* target = shard.get_location(db);

            if (target)
            {
                dest = db;
                MXS_INFO("change_current_db: database is on server: '%s'.", target->unique_name);
                succp = true;
            }
        }
    }
    else
    {
        MXS_ERROR("change_current_db: failed to change database: Query buffer too large");
    }

    return succp;
}

/**
 * Convert a length encoded string into a C string.
 * @param data Pointer to the first byte of the string
 * @return Pointer to the newly allocated string or NULL if the value is NULL or an error occurred
 */
char* get_lenenc_str(void* data)
{
    unsigned char* ptr = (unsigned char*)data;
    char* rval;
    uintptr_t size;
    long offset;

    if (data == NULL)
    {
        return NULL;
    }

    if (*ptr < 251)
    {
        size = (uintptr_t) * ptr;
        offset = 1;
    }
    else
    {
        switch (*(ptr))
        {
        case 0xfb:
            return NULL;
        case 0xfc:
            size = *(ptr + 1) + (*(ptr + 2) << 8);
            offset = 2;
            break;
        case 0xfd:
            size = *ptr + (*(ptr + 2) << 8) + (*(ptr + 3) << 16);
            offset = 3;
            break;
        case 0xfe:
            size = *ptr + ((*(ptr + 2) << 8)) + (*(ptr + 3) << 16) +
                   (*(ptr + 4) << 24) + ((uintptr_t) * (ptr + 5) << 32) +
                   ((uintptr_t) * (ptr + 6) << 40) +
                   ((uintptr_t) * (ptr + 7) << 48) + ((uintptr_t) * (ptr + 8) << 56);
            offset = 8;
            break;
        default:
            return NULL;
        }
    }

    rval = (char*)MXS_MALLOC(sizeof(char) * (size + 1));
    if (rval)
    {
        memcpy(rval, ptr + offset, size);
        memset(rval + size, 0, 1);

    }
    return rval;
}

/**
 * Parses a response set to a SHOW DATABASES query and inserts them into the
 * router client session's database hashtable. The name of the database is used
 * as the key and the unique name of the server is the value. The function
 * currently supports only result sets that span a single SQL packet.
 * @param rses Router client session
 * @param target Target server where the database is
 * @param buf GWBUF containing the result set
 * @return 1 if a complete response was received, 0 if a partial response was received
 * and -1 if a database was found on more than one server.
 */
enum showdb_response SchemaRouterSession::parse_showdb_response(Backend* bref, GWBUF** buffer)
{
    unsigned char* ptr;
    SERVER* target = bref->m_backend->server;
    GWBUF* buf;
    bool duplicate_found = false;
    enum showdb_response rval = SHOWDB_PARTIAL_RESPONSE;

    if (buffer == NULL || *buffer == NULL)
    {
        return SHOWDB_FATAL_ERROR;
    }

    /** TODO: Don't make the buffer contiguous but process it as a buffer chain */
    *buffer = gwbuf_make_contiguous(*buffer);
    buf = modutil_get_complete_packets(buffer);

    if (buf == NULL)
    {
        return SHOWDB_PARTIAL_RESPONSE;
    }

    ptr = (unsigned char*) buf->start;

    if (PTR_IS_ERR(ptr))
    {
        MXS_INFO("SHOW DATABASES returned an error.");
        gwbuf_free(buf);
        return SHOWDB_FATAL_ERROR;
    }

    if (bref->m_num_mapping_eof == 0)
    {
        /** Skip column definitions */
        while (ptr < (unsigned char*) buf->end && !PTR_IS_EOF(ptr))
        {
            ptr += gw_mysql_get_byte3(ptr) + 4;
        }

        if (ptr >= (unsigned char*) buf->end)
        {
            MXS_INFO("Malformed packet for SHOW DATABASES.");
            *buffer = gwbuf_append(buf, *buffer);
            return SHOWDB_FATAL_ERROR;
        }

        atomic_add(&bref->m_num_mapping_eof, 1);
        /** Skip first EOF packet */
        ptr += gw_mysql_get_byte3(ptr) + 4;
    }

    while (ptr < (unsigned char*) buf->end && !PTR_IS_EOF(ptr))
    {
        int payloadlen = gw_mysql_get_byte3(ptr);
        int packetlen = payloadlen + 4;
        char* data = get_lenenc_str(ptr + 4);

        if (data)
        {
            if (m_shard.add_location(data, target))
            {
                MXS_INFO("<%s, %s>", target->unique_name, data);
            }
            else
            {
                if (!(m_router->m_ignored_dbs.find(data) != m_router->m_ignored_dbs.end() ||
                      (m_router->m_ignore_regex &&
                       pcre2_match(m_router->m_ignore_regex, (PCRE2_SPTR)data,
                                   PCRE2_ZERO_TERMINATED, 0, 0,
                                   m_router->m_ignore_match_data, NULL) >= 0)))
                {
                    duplicate_found = true;
                    SERVER *duplicate = m_shard.get_location(data);

                    MXS_ERROR("Database '%s' found on servers '%s' and '%s' for user %s@%s.",
                              data, target->unique_name, duplicate->unique_name,
                              m_client->user,
                              m_client->remote);
                }
            }
            MXS_FREE(data);
        }
        ptr += packetlen;
    }

    if (ptr < (unsigned char*) buf->end && PTR_IS_EOF(ptr) && bref->m_num_mapping_eof == 1)
    {
        atomic_add(&bref->m_num_mapping_eof, 1);
        MXS_INFO("SHOW DATABASES fully received from %s.",
                 bref->m_backend->server->unique_name);
    }
    else
    {
        MXS_INFO("SHOW DATABASES partially received from %s.",
                 bref->m_backend->server->unique_name);
    }

    gwbuf_free(buf);

    if (duplicate_found)
    {
        rval = SHOWDB_DUPLICATE_DATABASES;
    }
    else if (bref->m_num_mapping_eof == 2)
    {
        rval = SHOWDB_FULL_RESPONSE;
    }

    return rval;
}

/**
 * Initiate the generation of the database hash table by sending a
 * SHOW DATABASES query to each valid backend server. This sets the session
 * into the mapping state where it queues further queries until all the database
 * servers have returned a result.
 * @param inst Router instance
 * @param session Router client session
 * @return 1 if all writes to backends were succesful and 0 if one or more errors occurred
 */
int SchemaRouterSession::gen_databaselist()
{
    DCB* dcb;
    const char* query = "SHOW DATABASES";
    GWBUF *buffer, *clone;
    int i, rval = 0;
    unsigned int len;

    for (BackendList::iterator it = m_backends.begin(); it != m_backends.end(); it++)
    {
        it->m_mapped = false;
        it->m_num_mapping_eof = 0;
    }

    m_state |= INIT_MAPPING;
    m_state &= ~INIT_UNINT;
    len = strlen(query) + 1;
    buffer = gwbuf_alloc(len + 4);
    uint8_t *data = GWBUF_DATA(buffer);
    *(data) = len;
    *(data + 1) = len >> 8;
    *(data + 2) = len >> 16;
    *(data + 3) = 0x0;
    *(data + 4) = 0x03;
    memcpy(data + 5, query, strlen(query));

    for (BackendList::iterator it = m_backends.begin(); it != m_backends.end(); it++)
    {
        if (BREF_IS_IN_USE(it) &&
            !BREF_IS_CLOSED(it) &
            SERVER_IS_RUNNING(it->m_backend->server))
        {
            clone = gwbuf_clone(buffer);
            dcb = it->m_dcb;
            rval |= !dcb->func.write(dcb, clone);
            MXS_DEBUG("Wrote SHOW DATABASES to %s for session %p: returned %d",
                      it->m_backend->server->unique_name,
                      m_client->session,
                      rval);
        }
    }
    gwbuf_free(buffer);
    return !rval;
}

/**
 * Check the hashtable for the right backend for this query.
 * @param router Router instance
 * @param client Client router session
 * @param buffer Query to inspect
 * @return Name of the backend or NULL if the query contains no known databases.
 */
SERVER* SchemaRouterSession::get_shard_target(GWBUF* buffer, uint32_t qtype)
{
    SERVER *rval = NULL;
    bool has_dbs = false; /**If the query targets any database other than the current one*/
    const QC_FIELD_INFO* info;
    size_t n_info;

    qc_get_field_info(buffer, &info, &n_info);

    for (size_t i = 0; i < n_info; i++)
    {
        if (info[i].database)
        {
            if (strcmp(info[i].database, "information_schema") == 0 && rval == NULL)
            {
                has_dbs = false;
            }
            else
            {
                SERVER* target = m_shard.get_location(info[i].database);

                if (target)
                {
                    if (rval && target != rval)
                    {
                        MXS_ERROR("Query targets databases on servers '%s' and '%s'. "
                                  "Cross database queries across servers are not supported.",
                                  rval->unique_name, target->unique_name);
                    }
                    else if (rval == NULL)
                    {
                        rval = target;
                        has_dbs = true;
                        MXS_INFO("Query targets database '%s' on server '%s'",
                                 info[i].database, rval->unique_name);
                    }
                }
            }
        }
    }

    /* Check if the query is a show tables query with a specific database */

    if (qc_query_is_type(qtype, QUERY_TYPE_SHOW_TABLES))
    {
        char *query = modutil_get_SQL(buffer);
        char *tmp;

        if ((tmp = strcasestr(query, "from")))
        {
            const char *delim = "` \n\t;";
            char *saved, *tok = strtok_r(tmp, delim, &saved);
            tok = strtok_r(NULL, delim, &saved);

            if (tok)
            {
                rval = m_shard.get_location(tok);

                if (rval)
                {
                    MXS_INFO("SHOW TABLES with specific database '%s' on server '%s'", tok, tmp);
                }
            }
        }
        MXS_FREE(query);

        if (rval == NULL)
        {
            rval = m_shard.get_location(m_current_db);

            if (rval)
            {
                MXS_INFO("SHOW TABLES query, current database '%s' on server '%s'",
                         m_current_db.c_str(), rval->unique_name);
            }
        }
        else
        {
            has_dbs = true;
        }
    }
    else if (buffer->hint && buffer->hint->type == HINT_ROUTE_TO_NAMED_SERVER)
    {
        for (BackendList::iterator it = m_backends.begin(); it != m_backends.end(); it++)
        {
            char *srvnm = it->m_backend->server->unique_name;

            if (strcmp(srvnm, (char*)buffer->hint->data) == 0)
            {
                rval = it->m_backend->server;
                MXS_INFO("Routing hint found (%s)", rval->unique_name);
            }
        }

        if (rval == NULL && !has_dbs && m_current_db.length())
        {
            /**
             * If the target name has not been found and the session has an
             * active database, set is as the target
             */

            rval = m_shard.get_location(m_current_db);

            if (rval)
            {
                MXS_INFO("Using active database '%s' on '%s'",
                         m_current_db.c_str(), rval->unique_name);
            }
        }
    }

    return rval;
}

/**
 * Provide the router with a pointer to a suitable backend dcb.
 *
 * Detect failures in server statuses and reselect backends if necessary
 * If name is specified, server name becomes primary selection criteria.
 * Similarly, if max replication lag is specified, skip backends which lag too
 * much.
 *
 * @param p_dcb Address of the pointer to the resulting DCB
 * @param name  Name of the backend which is primarily searched. May be NULL.
 *
 * @return True if proper DCB was found, false otherwise.
 */
bool SchemaRouterSession::get_shard_dcb(DCB** p_dcb, char* name)
{
    bool succp = false;
    ss_dassert(p_dcb != NULL && *(p_dcb) == NULL);

    for (BackendList::iterator it = m_backends.begin(); it != m_backends.end(); it++)
    {
        SERVER_REF* b = it->m_backend;
        /**
         * To become chosen:
         * backend must be in use, name must match, and
         * the backend state must be RUNNING
         */
        if (BREF_IS_IN_USE((&(*it))) &&
            (strncasecmp(name, b->server->unique_name, PATH_MAX) == 0) &&
            SERVER_IS_RUNNING(b->server))
        {
            *p_dcb = it->m_dcb;
            succp = true;
            ss_dassert(it->m_dcb->state != DCB_STATE_ZOMBIE);
            break;
        }
    }

    return succp;
}


/**
 * Examine the query type, transaction state and routing hints. Find out the
 * target for query routing.
 *
 *  @param qtype      Type of query
 *  @param trx_active Is transacation active or not
 *  @param hint       Pointer to list of hints attached to the query buffer
 *
 *  @return bitfield including the routing target, or the target server name
 *          if the query would otherwise be routed to slave.
 */
enum route_target get_shard_route_target(uint32_t qtype)
{
    enum route_target target = TARGET_UNDEFINED;

    /**
     * These queries are not affected by hints
     */
    if (qc_query_is_type(qtype, QUERY_TYPE_SESSION_WRITE) ||
        qc_query_is_type(qtype, QUERY_TYPE_GSYSVAR_WRITE) ||
        qc_query_is_type(qtype, QUERY_TYPE_USERVAR_WRITE) ||
        qc_query_is_type(qtype, QUERY_TYPE_PREPARE_STMT) ||
        qc_query_is_type(qtype, QUERY_TYPE_PREPARE_NAMED_STMT) ||
        qc_query_is_type(qtype, QUERY_TYPE_ENABLE_AUTOCOMMIT) ||
        qc_query_is_type(qtype, QUERY_TYPE_DISABLE_AUTOCOMMIT))
    {
        /** hints don't affect on routing */
        target = TARGET_ALL;
    }
    else if (qc_query_is_type(qtype, QUERY_TYPE_SYSVAR_READ) ||
             qc_query_is_type(qtype, QUERY_TYPE_GSYSVAR_READ))
    {
        target = TARGET_ANY;
    }

    return target;
}

/**
 * Callback for the database list streaming.
 * @param rset Result set which is being processed
 * @param data Pointer to struct string_array containing the database names
 * @return New resultset row or NULL if no more data is available. If memory allocation
 * failed, NULL is returned.
 */
RESULT_ROW *result_set_cb(struct resultset * rset, void *data)
{
    RESULT_ROW *row = resultset_make_row(rset);
    ServerMap* arr = (ServerMap*) data;

    if (row)
    {
        if (arr->size() > 0 && resultset_row_set(row, 0, arr->begin()->first.c_str()))
        {
            arr->erase(arr->begin());
        }
        else
        {
            resultset_free_row(row);
            row = NULL;
        }
    }

    return row;
}

/**
 * Generates a custom SHOW DATABASES result set from all the databases in the
 * hashtable. Only backend servers that are up and in a proper state are listed
 * in it.
 * @param router Router instance
 * @param client Router client session
 * @return True if the sending of the database list was successful, otherwise false
 */
bool SchemaRouterSession::send_database_list()
{
    bool rval = false;

    ServerMap dblist;
    m_shard.get_content(dblist);

    RESULTSET* resultset = resultset_create(result_set_cb, &dblist);

    if (resultset_add_column(resultset, "Database", MYSQL_DATABASE_MAXLEN,
                             COL_TYPE_VARCHAR))
    {
        resultset_stream_mysql(resultset, m_client);
        rval = true;
    }
    resultset_free(resultset);

    return rval;
}

/**
 * @node Search all RUNNING backend servers and connect
 *
 * Parameters:
 * @param backend_ref - in, use, out
 *      Pointer to backend server reference object array.
 *      NULL is not allowed.
 *
 * @param router_nservers - in, use
 *      Number of backend server pointers pointed to by b.
 *
 * @param session - in, use
 *      MaxScale session pointer used when connection to backend is established.
 *
 * @param  router - in, use
 *      Pointer to router instance. Used when server states are qualified.
 *
 * @return true, if at least one master and one slave was found.
 *
 *
 * @details It is assumed that there is only one available server.
 *      There will be exactly as many backend references than there are
 *      connections because all servers are supposed to be operational. It is,
 *      however, possible that there are less available servers than expected.
 */
bool connect_backend_servers(BackendList& backends, MXS_SESSION* session)
{
    bool succp = false;
    int servers_found = 0;
    int servers_connected = 0;
    int slaves_connected = 0;

    if (MXS_LOG_PRIORITY_IS_ENABLED(LOG_INFO))
    {
        MXS_INFO("Servers and connection counts:");

        for (BackendList::iterator it = backends.begin(); it != backends.end(); it++)
        {
            SERVER_REF* b = it->m_backend;

            MXS_INFO("MaxScale connections : %d (%d) in \t%s:%d %s",
                     b->connections,
                     b->server->stats.n_current,
                     b->server->name,
                     b->server->port,
                     STRSRVSTATUS(b->server));
        }
    }
    /**
     * Scan server list and connect each of them. None should fail or session
     * can't be established.
     */
    for (BackendList::iterator it = backends.begin(); it != backends.end(); it++)
    {
        SERVER_REF* b = it->m_backend;

        if (SERVER_IS_RUNNING(b->server))
        {
            servers_found += 1;

            /** Server is already connected */
            if (BREF_IS_IN_USE(it))
            {
                slaves_connected += 1;
            }
            /** New server connection */
            else
            {
                if ((it->m_dcb = dcb_connect(b->server, session, b->server->protocol)))
                {
                    servers_connected += 1;
                    /**
                     * When server fails, this callback
                     * is called.
                     * !!! Todo, routine which removes
                     * corresponding entries from the hash
                     * table.
                     */

                    it->m_state = 0;
                    it->set_state(BREF_IN_USE);
                    /**
                     * Increase backend connection counter.
                     * Server's stats are _increased_ in
                     * dcb.c:dcb_alloc !
                     * But decreased in the calling function
                     * of dcb_close.
                     */
                    atomic_add(&b->connections, 1);
                }
                else
                {
                    succp = false;
                    MXS_ERROR("Unable to establish "
                              "connection with slave %s:%d",
                              b->server->name,
                              b->server->port);
                    /* handle connect error */
                    break;
                }
            }
        }
    }

    if (servers_connected > 0)
    {
        succp = true;

        if (MXS_LOG_PRIORITY_IS_ENABLED(LOG_INFO))
        {
            for (BackendList::iterator it = backends.begin(); it != backends.end(); it++)
            {
                SERVER_REF* b = it->m_backend;

                if (BREF_IS_IN_USE(it))
                {
                    MXS_INFO("Connected %s in \t%s:%d",
                             STRSRVSTATUS(b->server),
                             b->server->name,
                             b->server->port);
                }
            }
        }
    }

    return succp;
}
