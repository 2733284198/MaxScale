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

#include "rwsplitsession.hh"
#include "routeinfo.hh"

#include <cmath>

using namespace maxscale;

RWSplitSession::RWSplitSession(RWSplit* instance, MXS_SESSION* session,
                               const SRWBackendList& backends,
                               const SRWBackend& master):
    mxs::RouterSession(session),
    m_backends(backends),
    m_current_master(master),
    m_config(instance->config()),
    m_nbackends(instance->service()->n_dbref),
    m_load_data_sent(0),
    m_client(session->client_dcb),
    m_sescmd_count(1), // Needs to be a positive number to work
    m_expected_responses(0),
    m_query_queue(NULL),
    m_router(instance),
    m_sent_sescmd(0),
    m_recv_sescmd(0),
    m_gtid_pos(""),
    m_wait_gtid_state(EXPECTING_NOTHING),
    m_next_seq(0),
    m_qc(session, instance->config().use_sql_variables_in)
{
    if (m_config.rw_max_slave_conn_percent)
    {
        int n_conn = 0;
        double pct = (double)m_config.rw_max_slave_conn_percent / 100.0;
        n_conn = MXS_MAX(floor((double)m_nbackends * pct), 1);
        m_config.max_slave_connections = n_conn;
    }
}

RWSplitSession* RWSplitSession::create(RWSplit* router, MXS_SESSION* session)
{
    RWSplitSession* rses = NULL;

    if (router->have_enough_servers())
    {
        SRWBackendList backends = RWBackend::from_servers(router->service()->dbref);

        /**
         * At least the master must be found if the router is in the strict mode.
         * If sessions without master are allowed, only a slave must be found.
         */

        SRWBackend master;

        if (router->select_connect_backend_servers(session, backends, master, NULL,
                                                   NULL, connection_type::ALL))
        {
            if ((rses = new RWSplitSession(router, session, backends, master)))
            {
                router->stats().n_sessions += 1;
            }
        }
    }

    return rses;
}

void close_all_connections(SRWBackendList& backends)
{
    for (SRWBackendList::iterator it = backends.begin(); it != backends.end(); it++)
    {
        SRWBackend& backend = *it;

        if (backend->in_use())
        {
            backend->close();
        }
    }
}

void RWSplitSession::close()
{
    close_all_connections(m_backends);

    if (MXS_LOG_PRIORITY_IS_ENABLED(LOG_INFO) &&
        m_sescmd_list.size())
    {
        std::string sescmdstr;

        for (mxs::SessionCommandList::iterator it = m_sescmd_list.begin();
             it != m_sescmd_list.end(); it++)
        {
            mxs::SSessionCommand& scmd = *it;
            sescmdstr += scmd->to_string();
            sescmdstr += "\n";
        }

        MXS_INFO("Executed session commands:\n%s", sescmdstr.c_str());
    }
}

int32_t RWSplitSession::routeQuery(GWBUF* querybuf)
{
    int rval = 0;

    if (m_query_queue == NULL &&
        (m_expected_responses == 0 ||
         mxs_mysql_get_command(querybuf) == MXS_COM_STMT_FETCH ||
         m_load_data_state == LOAD_DATA_ACTIVE ||
         m_large_query))
    {
        /** Gather the information required to make routing decisions */
        RouteInfo info(this, querybuf);

        /** No active or pending queries */
        if (route_single_stmt(querybuf, info))
        {
            rval = 1;
        }
    }
    else
    {
        /**
         * We are already processing a request from the client. Store the
         * new query and wait for the previous one to complete.
         */
        ss_dassert(m_expected_responses || m_query_queue);
        MXS_INFO("Storing query (len: %d cmd: %0x), expecting %d replies to current command",
                 gwbuf_length(querybuf), GWBUF_DATA(querybuf)[4], m_expected_responses);
        m_query_queue = gwbuf_append(m_query_queue, querybuf);
        querybuf = NULL;
        rval = 1;
        ss_dassert(m_expected_responses > 0);

        if (m_expected_responses == 0 && !route_stored_query())
        {
            rval = 0;
        }
    }

    if (querybuf != NULL)
    {
        gwbuf_free(querybuf);
    }

    return rval;
}

/**
 * @brief Route a stored query
 *
 * When multiple queries are executed in a pipeline fashion, the readwritesplit
 * stores the extra queries in a queue. This queue is emptied after reading a
 * reply from the backend server.
 *
 * @param rses Router client session
 * @return True if a stored query was routed successfully
 */
bool RWSplitSession::route_stored_query()
{
    bool rval = true;

    /** Loop over the stored statements as long as the routeQuery call doesn't
     * append more data to the queue. If it appends data to the queue, we need
     * to wait for a response before attempting another reroute */
    while (m_query_queue)
    {
        GWBUF* query_queue = modutil_get_next_MySQL_packet(&query_queue);
        query_queue = gwbuf_make_contiguous(query_queue);

        /** Store the query queue locally for the duration of the routeQuery call.
         * This prevents recursive calls into this function. */
        GWBUF *temp_storage = query_queue;
        query_queue = NULL;

        // TODO: Move the handling of queued queries to the client protocol
        // TODO: module where the command tracking is done automatically.
        uint8_t cmd = mxs_mysql_get_command(query_queue);
        mysql_protocol_set_current_command(m_client, (mxs_mysql_cmd_t)cmd);

        if (!routeQuery(query_queue))
        {
            rval = false;
            MXS_ERROR("Failed to route queued query.");
        }

        if (query_queue == NULL)
        {
            /** Query successfully routed and no responses are expected */
            query_queue = temp_storage;
        }
        else
        {
            /** Routing was stopped, we need to wait for a response before retrying */
            query_queue = gwbuf_append(temp_storage, query_queue);
            break;
        }
    }

    return rval;
}

bool RWSplitSession::reroute_stored_statement(const SRWBackend& old, GWBUF *stored)
{
    bool success = false;

    if (!session_trx_is_active(m_client->session))
    {
        /**
         * Only try to retry the read if autocommit is enabled and we are
         * outside of a transaction
         */
        for (auto it = m_backends.begin(); it != m_backends.end(); it++)
        {
            SRWBackend& backend = *it;

            if (backend->in_use() && backend != old &&
                !backend->is_master() && backend->is_slave())
            {
                /** Found a valid candidate; a non-master slave that's in use */
                if (backend->write(stored))
                {
                    MXS_INFO("Retrying failed read at '%s'.", backend->name());
                    ss_dassert(backend->get_reply_state() == REPLY_STATE_DONE);
                    backend->set_reply_state(REPLY_STATE_START);
                    m_expected_responses++;
                    success = true;
                    break;
                }
            }
        }

        if (!success && m_current_master && m_current_master->in_use())
        {
            /**
             * Either we failed to write to the slave or no valid slave was found.
             * Try to retry the read on the master.
             */
            if (m_current_master->write(stored))
            {
                MXS_INFO("Retrying failed read at '%s'.", m_current_master->name());
                ss_dassert(m_current_master->get_reply_state() == REPLY_STATE_DONE);
                m_current_master->set_reply_state(REPLY_STATE_START);
                m_expected_responses++;
                success = true;
            }
        }
    }

    return success;
}

/**
 * @bref discard the result of wait gtid statment, the result will be an error
 * packet or an error packet.
 * @param buffer origin reply buffer
 * @param proto  MySQLProtocol
 * @return reset buffer
 */
GWBUF* RWSplitSession::discard_master_wait_gtid_result(GWBUF *buffer)
{
    uint8_t header_and_command[MYSQL_HEADER_LEN + 1];
    uint8_t packet_len = 0;
    uint8_t offset = 0;
    mxs_mysql_cmd_t com;

    gwbuf_copy_data(buffer, 0, MYSQL_HEADER_LEN + 1, header_and_command);
    /* ignore error packet */
    if (MYSQL_GET_COMMAND(header_and_command) == MYSQL_REPLY_ERR)
    {
        m_wait_gtid_state = EXPECTING_NOTHING;
        return buffer;
    }

    /* this packet must be an ok packet now */
    ss_dassert(MYSQL_GET_COMMAND(header_and_command) == MYSQL_REPLY_OK);
    packet_len = MYSQL_GET_PAYLOAD_LEN(header_and_command) + MYSQL_HEADER_LEN;
    m_wait_gtid_state = EXPECTING_REAL_RESULT;
    m_next_seq = 1;

    return gwbuf_consume(buffer, packet_len);
}

/**
 * @brief Find the backend reference that matches the given DCB
 *
 * @param dcb DCB to match
 *
 * @return The correct reference
 */
SRWBackend& RWSplitSession::get_backend_from_dcb(DCB *dcb)
{
    ss_dassert(dcb->dcb_role == DCB_ROLE_BACKEND_HANDLER);
    CHK_DCB(dcb);

    for (auto it = m_backends.begin(); it != m_backends.end(); it++)
    {
        SRWBackend& backend = *it;

        if (backend->in_use() && backend->dcb() == dcb)
        {
            return backend;
        }
    }

    /** We should always have a valid backend reference and in case we don't,
     * something is terribly wrong. */
    MXS_ALERT("No reference to DCB %p found, aborting.", dcb);
    raise(SIGABRT);

    // To make the compiler happy, we return a reference to a static value.
    static SRWBackend this_should_not_happen;
    return this_should_not_happen;
}

/**
 * @bref After discarded the wait result, we need correct the seqence number of every packet
 *
 * @param buffer origin reply buffer
 * @param proto  MySQLProtocol
 *
 */
void RWSplitSession::correct_packet_sequence(GWBUF *buffer)
{
    uint8_t header[3];
    uint32_t offset = 0;
    uint32_t packet_len = 0;
    if (m_wait_gtid_state == EXPECTING_REAL_RESULT)
    {
        while (gwbuf_copy_data(buffer, offset, 3, header) == 3)
        {
           packet_len = MYSQL_GET_PAYLOAD_LEN(header) + MYSQL_HEADER_LEN;
           uint8_t *seq = gwbuf_byte_pointer(buffer, offset + MYSQL_SEQ_OFFSET);
           *seq = m_next_seq;
           m_next_seq++;
           offset += packet_len;
        }
    }
}

static void log_unexpected_response(DCB* dcb, GWBUF* buffer)
{
    if (mxs_mysql_is_err_packet(buffer))
    {
        /** This should be the only valid case where the server sends a response
         * without the client sending one first. MaxScale does not yet advertise
         * the progress reporting flag so we don't need to handle it. */
        uint8_t* data = GWBUF_DATA(buffer);
        size_t len = MYSQL_GET_PAYLOAD_LEN(data);
        uint16_t errcode = MYSQL_GET_ERRCODE(data);
        std::string errstr((char*)data + 7, (char*)data + 7 + len - 3);

        if (errcode == ER_CONNECTION_KILLED)
        {
            MXS_INFO("Connection from '%s'@'%s' to '%s' was killed",
                     dcb->session->client_dcb->user,
                     dcb->session->client_dcb->remote,
                     dcb->server->unique_name);
        }
        else
        {
            MXS_WARNING("Server '%s' sent an unexpected error: %hu, %s",
                        dcb->server->unique_name, errcode, errstr.c_str());
        }
    }
    else
    {
        MXS_ERROR("Unexpected internal state: received response 0x%02hhx from "
                  "server '%s' when no response was expected",
                  mxs_mysql_get_command(buffer), dcb->server->unique_name);
        ss_dassert(false);
    }
}

void RWSplitSession::clientReply(GWBUF *writebuf, DCB *backend_dcb)
{
    DCB *client_dcb = backend_dcb->session->client_dcb;

    SRWBackend& backend = get_backend_from_dcb(backend_dcb);

    if (m_config.enable_causal_read &&
        GWBUF_IS_REPLY_OK(writebuf) &&
        backend == m_current_master)
    {
        /** Save gtid position */
        char *tmp = gwbuf_get_property(writebuf, (char *)"gtid");
        if (tmp)
        {
            m_gtid_pos = std::string(tmp);
        }
    }

    if (m_wait_gtid_state == EXPECTING_WAIT_GTID_RESULT)
    {
        ss_dassert(m_config.enable_causal_read);
        if ((writebuf = discard_master_wait_gtid_result(writebuf)) == NULL)
        {
            // Nothing to route, return
            return;
        }
    }
    if (m_wait_gtid_state == EXPECTING_REAL_RESULT)
    {
        correct_packet_sequence(writebuf);
    }

    if (backend->get_reply_state() == REPLY_STATE_DONE)
    {
        /** If we receive an unexpected response from the server, the internal
         * logic cannot handle this situation. Routing the reply straight to
         * the client should be the safest thing to do at this point. */
        log_unexpected_response(backend_dcb, writebuf);
        MXS_SESSION_ROUTE_REPLY(backend_dcb->session, writebuf);
        return;
    }

    if (session_have_stmt(backend_dcb->session))
    {
        /** Statement was successfully executed, free the stored statement */
        session_clear_stmt(backend_dcb->session);
    }

    if (backend->reply_is_complete(writebuf))
    {
        /** Got a complete reply, acknowledge the write and decrement expected response count */
        backend->ack_write();
        m_expected_responses--;
        ss_dassert(m_expected_responses >= 0);
        ss_dassert(backend->get_reply_state() == REPLY_STATE_DONE);
        MXS_INFO("Reply complete, last reply from %s", backend->name());
    }
    else
    {
        MXS_INFO("Reply not yet complete. Waiting for %d replies, got one from %s",
                 m_expected_responses, backend->name());
    }

    if (backend->have_session_commands())
    {
        /** Reply to an executed session command */
        process_sescmd_response(backend, &writebuf);
    }

    if (backend->have_session_commands())
    {
        if (backend->execute_session_command())
        {
            m_expected_responses++;
        }
    }
    else if (m_expected_responses == 0 && m_query_queue)
    {
        route_stored_query();
    }

    if (writebuf)
    {
        ss_dassert(client_dcb);
        /** Write reply to client DCB */
        MXS_SESSION_ROUTE_REPLY(backend_dcb->session, writebuf);
    }
}

void check_and_log_backend_state(const SRWBackend& backend, DCB* problem_dcb)
{
    if (backend)
    {
        /** This is a valid DCB for a backend ref */
        if (backend->in_use() && backend->dcb() == problem_dcb)
        {
            MXS_ERROR("Backend '%s' is still in use and points to the problem DCB.",
                      backend->name());
            ss_dassert(false);
        }
    }
    else
    {
        const char *remote = problem_dcb->state == DCB_STATE_POLLING &&
                             problem_dcb->server ? problem_dcb->server->unique_name : "CLOSED";

        MXS_ERROR("DCB connected to '%s' is not in use by the router "
                  "session, not closing it. DCB is in state '%s'",
                  remote, STRDCBSTATE(problem_dcb->state));
    }
}

/**
 * @brief Router error handling routine
 *
 * Error Handler routine to resolve backend failures. If it succeeds then
 * there are enough operative backends available and connected. Otherwise it
 * fails, and session is terminated.
 *
 * @param instance       The router instance
 * @param router_session The router session
 * @param errmsgbuf      The error message to reply
 * @param backend_dcb    The backend DCB
 * @param action         The action: ERRACT_NEW_CONNECTION or
 *                       ERRACT_REPLY_CLIENT
 * @param succp          Result of action: true if router can continue
 */
void RWSplitSession::handleError(GWBUF *errmsgbuf, DCB *problem_dcb,
                                 mxs_error_action_t action, bool *succp)
{
    ss_dassert(problem_dcb->dcb_role == DCB_ROLE_BACKEND_HANDLER);
    CHK_DCB(problem_dcb);
    MXS_SESSION *session = problem_dcb->session;
    ss_dassert(session);

    SRWBackend& backend = get_backend_from_dcb(problem_dcb);
    ss_dassert(backend->in_use());

    switch (action)
    {
    case ERRACT_NEW_CONNECTION:
        {
            if (m_current_master && m_current_master->in_use() && m_current_master == backend)
            {
                /** The connection to the master has failed */
                SERVER *srv = m_current_master->server();
                bool can_continue = false;

                if (!backend->is_waiting_result())
                {
                    /** The failure of a master is not considered a critical
                     * failure as partial functionality still remains. If
                     * master_failure_mode is not set to fail_instantly, reads
                     * are allowed as long as slave servers are available
                     * and writes will cause an error to be returned.
                     *
                     * If we were waiting for a response from the master, we
                     * can't be sure whether it was executed or not. In this
                     * case the safest thing to do is to close the client
                     * connection. */
                    if (m_config.master_failure_mode != RW_FAIL_INSTANTLY)
                    {
                        can_continue = true;
                    }
                }
                else
                {
                    // We were expecting a response but we aren't going to get one
                    m_expected_responses--;

                    if (m_config.master_failure_mode == RW_ERROR_ON_WRITE)
                    {
                        /** In error_on_write mode, the session can continue even
                         * if the master is lost. Send a read-only error to
                         * the client to let it know that the query failed. */
                        can_continue = true;
                        send_readonly_error(m_client);
                    }

                    if (!SERVER_IS_MASTER(srv) && !srv->master_err_is_logged)
                    {
                        ss_dassert(backend);
                        MXS_ERROR("Server %s (%s) lost the master status while waiting"
                            " for a result. Client sessions will be closed.",
                                  backend->name(), backend->uri());
                        backend->server()->master_err_is_logged = true;
                    }
                }

                if (session_trx_is_active(session))
                {
                    // We have an open transaction, we can't continue
                    can_continue = false;
                }

                *succp = can_continue;
                backend->close();
            }
            else
            {
                if (m_target_node && m_target_node == backend &&
                     session_trx_is_read_only(problem_dcb->session))
                {
                    /**
                     * We were locked to a single node but the node died. Currently
                     * this only happens with read-only transactions so the only
                     * thing we can do is to close the connection.
                     */
                    *succp = false;
                    backend->close(mxs::Backend::CLOSE_FATAL);
                }
                else
                {
                    /** Try to replace the failed connection with a new one */
                    *succp = handle_error_new_connection(problem_dcb, errmsgbuf);
                }
            }

            check_and_log_backend_state(backend, problem_dcb);
            break;
        }

    case ERRACT_REPLY_CLIENT:
        {
            handle_error_reply_client(problem_dcb, errmsgbuf);
            *succp = false; /*< no new backend servers were made available */
            break;
        }

    default:
        ss_dassert(!true);
        *succp = false;
        break;
    }
}

/**
 * Check if there is backend reference pointing at failed DCB, and reset its
 * flags. Then clear DCB's callback and finally : try to find replacement(s)
 * for failed slave(s).
 *
 * This must be called with router lock.
 *
 * @param inst      router instance
 * @param rses      router client session
 * @param dcb       failed DCB
 * @param errmsg    error message which is sent to client if it is waiting
 *
 * @return true if there are enough backend connections to continue, false if
 * not
 */
bool RWSplitSession::handle_error_new_connection(DCB *backend_dcb, GWBUF *errmsg)
{
    SRWBackend& backend = get_backend_from_dcb(backend_dcb);
    MXS_SESSION* ses = backend_dcb->session;
    bool route_stored = false;
    CHK_SESSION(ses);

    if (backend->is_waiting_result())
    {
        ss_dassert(m_expected_responses > 0);
        m_expected_responses--;

        /**
         * A query was sent through the backend and it is waiting for a reply.
         * Try to reroute the statement to a working server or send an error
         * to the client.
         */
        GWBUF *stored = NULL;
        const SERVER *target = NULL;
        if (!session_take_stmt(backend_dcb->session, &stored, &target) ||
            target != backend->backend()->server ||
            !reroute_stored_statement(backend, stored))
        {
            /**
             * We failed to route the stored statement or no statement was
             * stored for this server. Either way we can safely free the buffer
             * and decrement the expected response count.
             */
            gwbuf_free(stored);

            if (!backend->have_session_commands())
            {
                /**
                 * The backend was executing a command that requires a reply.
                 * Send an error to the client to let it know the query has
                 * failed.
                 */
                DCB *client_dcb = ses->client_dcb;
                client_dcb->func.write(client_dcb, gwbuf_clone(errmsg));
            }

            if (m_expected_responses == 0)
            {
                /** The response from this server was the last one, try to
                 * route all queued queries */
                route_stored = true;
            }
        }
    }

    /** Close the current connection. This needs to be done before routing any
     * of the stored queries. If we route a stored query before the connection
     * is closed, it's possible that the routing logic will pick the failed
     * server as the target. */
    backend->close();

    if (route_stored)
    {
        route_stored_query();
    }

    int max_nslaves = m_router->max_slave_count();
    bool succp;
    /**
     * Try to get replacement slave or at least the minimum
     * number of slave connections for router session.
     */
    if (m_recv_sescmd > 0 && m_config.disable_sescmd_history)
    {
        succp = m_router->have_enough_servers();
    }
    else
    {
        succp = m_router->select_connect_backend_servers(ses, m_backends,
                                                       m_current_master,
                                                       &m_sescmd_list,
                                                       &m_expected_responses,
                                                       connection_type::SLAVE);
    }

    return succp;
}

/**
 * @brief Handle an error reply for a client
 *
 * @param ses           Session
 * @param rses          Router session
 * @param backend_dcb   DCB for the backend server that has failed
 * @param errmsg        GWBUF containing the error message
 */
void RWSplitSession::handle_error_reply_client(DCB *backend_dcb, GWBUF *errmsg)
{
    mxs_session_state_t sesstate = m_client->session->state;
    SRWBackend& backend = get_backend_from_dcb(backend_dcb);

    backend->close();

    if (sesstate == SESSION_STATE_ROUTER_READY)
    {
        CHK_DCB(m_client);
        m_client->func.write(m_client, gwbuf_clone(errmsg));
    }
}

uint32_t get_internal_ps_id(RWSplitSession* rses, GWBUF* buffer)
{
    uint32_t rval = 0;

    // All COM_STMT type statements store the ID in the same place
    uint32_t id = mxs_mysql_extract_ps_id(buffer);
    ClientHandleMap::iterator it = rses->m_ps_handles.find(id);

    if (it != rses->m_ps_handles.end())
    {
        rval = it->second;
    }
    else
    {
        MXS_WARNING("Client requests unknown prepared statement ID '%u' that "
                    "does not map to an internal ID", id);
    }

    return rval;
}
