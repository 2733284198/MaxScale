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

#include "rules.hh"

#include <algorithm>

#include <maxscale/alloc.h>
#include <maxscale/buffer.h>
#include <maxscale/log_manager.h>
#include <maxscale/modutil.h>
#include <maxscale/protocol/mysql.h>

static inline bool query_is_sql(GWBUF* query)
{
    return modutil_is_SQL(query) || modutil_is_SQL_prepare(query);
}

Rule::Rule(std::string name):
    data(NULL),
    name(name),
    type(RT_PERMISSION),
    on_queries(FW_OP_UNDEFINED),
    times_matched(0),
    active(NULL)
{
}

Rule::~Rule()
{
}

bool Rule::matches_query(FW_SESSION* session, GWBUF* buffer, char** msg)
{
    *msg = create_error("Permission denied at this time.");
    MXS_NOTICE("rule '%s': query denied at this time.", name.c_str());
    return true;
}

bool Rule::need_full_parsing(GWBUF* buffer) const
{
    bool rval = false;

    if (type == RT_COLUMN ||
        type == RT_FUNCTION ||
        type == RT_USES_FUNCTION ||
        type == RT_WILDCARD ||
        type == RT_CLAUSE)
    {
        switch (qc_get_operation(buffer))
        {
        case QUERY_OP_SELECT:
        case QUERY_OP_UPDATE:
        case QUERY_OP_INSERT:
        case QUERY_OP_DELETE:
            rval = true;
            break;

        default:
            break;
        }
    }

    return rval;
}

bool Rule::matches_query_type(GWBUF* buffer)
{
    qc_query_op_t optype = qc_get_operation(buffer);

    return on_queries == FW_OP_UNDEFINED ||
           (on_queries & qc_op_to_fw_op(optype)) ||
           (MYSQL_IS_COM_INIT_DB(GWBUF_DATA(buffer)) &&
            (on_queries & FW_OP_CHANGE_DB));
}

bool WildCardRule::matches_query(FW_SESSION* session, GWBUF *queue, char **msg)
{
    bool rval = false;

    if (query_is_sql(queue))
    {
        const QC_FIELD_INFO* infos;
        size_t n_infos;
        qc_get_field_info(queue, &infos, &n_infos);

        for (size_t i = 0; i < n_infos; ++i)
        {
            if (strcmp(infos[i].column, "*") == 0)
            {
                MXS_NOTICE("rule '%s': query contains a wildcard.", name.c_str());
                rval = true;
                *msg = create_error("Usage of wildcard denied.");
            }
        }
    }

    return rval;
}

bool NoWhereClauseRule::matches_query(FW_SESSION* session, GWBUF* buffer, char** msg)
{
    bool rval = false;

    if (query_is_sql(buffer) && !qc_query_has_clause(buffer))
    {
        rval = true;
        *msg = create_error("Required WHERE/HAVING clause is missing.");
        MXS_NOTICE("rule '%s': query has no where/having "
                   "clause, query is denied.", name.c_str());
    }

    return rval;
}

bool RegexRule::matches_query(FW_SESSION* session, GWBUF* buffer, char** msg)
{
    bool rval = false;

    if (query_is_sql(buffer))
    {
        pcre2_code* re = m_re.get();
        pcre2_match_data *mdata = pcre2_match_data_create_from_pattern(re, NULL);
        MXS_ABORT_IF_NULL(mdata);

        char* sql;
        int len;
        modutil_extract_SQL(buffer, &sql, &len);

        if (pcre2_match(re, (PCRE2_SPTR)sql, (size_t)len, 0, 0, mdata, NULL) > 0)
        {
            MXS_NOTICE("rule '%s': regex matched on query", name.c_str());
            rval = true;
            *msg = create_error("Permission denied, query matched regular expression.");
        }

        pcre2_match_data_free(mdata);
    }

    return rval;
}

bool ColumnsRule::matches_query(FW_SESSION* session, GWBUF* buffer, char** msg)
{
    bool rval = false;

    if (query_is_sql(buffer))
    {
        const QC_FIELD_INFO* infos;
        size_t n_infos;
        qc_get_field_info(buffer, &infos, &n_infos);

        for (size_t i = 0; !rval && i < n_infos; ++i)
        {
            std::string tok = infos[i].column;
            ValueList::const_iterator it = std::find(m_values.begin(), m_values.end(), tok);

            if (it != m_values.end())
            {
                MXS_NOTICE("rule '%s': query targets forbidden column: %s",
                           name.c_str(), tok.c_str());
                *msg = create_error("Permission denied to column '%s'.", tok.c_str());
                rval = true;
                break;
            }
        }
    }

    return rval;
}


bool FunctionRule::matches_query(FW_SESSION* session, GWBUF* buffer, char** msg)
{
    bool rval = false;

    if (query_is_sql(buffer))
    {
        const QC_FUNCTION_INFO* infos;
        size_t n_infos;
        qc_get_function_info(buffer, &infos, &n_infos);

        if (n_infos == 0 && session->instance->action == FW_ACTION_ALLOW)
        {
            rval = true;
        }
        else
        {
            for (size_t i = 0; i < n_infos; ++i)
            {
                std::string tok = infos[i].name;
                ValueList::const_iterator it = std::find(m_values.begin(), m_values.end(), tok);

                if (it != m_values.end())
                {
                    MXS_NOTICE("rule '%s': query uses forbidden function: %s",
                               name.c_str(), tok.c_str());
                    *msg = create_error("Permission denied to function '%s'.", tok.c_str());
                    rval = true;
                    break;
                }

            }
        }
    }

    return rval;
}

bool FunctionUsageRule::matches_query(FW_SESSION* session, GWBUF* buffer, char** msg)
{
    if (query_is_sql(buffer))
    {
        const QC_FUNCTION_INFO* infos;
        size_t n_infos;
        qc_get_function_info(buffer, &infos, &n_infos);

        for (size_t i = 0; i < n_infos; ++i)
        {
            for (size_t j = 0; j < infos[i].n_fields; j++)
            {
                std::string tok = infos[i].fields[j].column;
                ValueList::const_iterator it = std::find(m_values.begin(), m_values.end(), tok);

                if (it != m_values.end())
                {
                    MXS_NOTICE("rule '%s': query uses a function with forbidden column: %s",
                               name.c_str(), tok.c_str());
                    *msg = create_error("Permission denied to column '%s' with function.", tok.c_str());
                    return true;
                }
            }
        }
    }

    return false;
}

bool LimitQueriesRule::matches_query(FW_SESSION* session, GWBUF* buffer, char** msg)
{
    if (session->query_speed == NULL)
    {
        session->query_speed = new QuerySpeed(m_timeperiod, m_holdoff, m_max);
    }

    QuerySpeed* queryspeed = session->query_speed;
    time_t time_now = time(NULL);
    bool matches = false;

    if (queryspeed->active)
    {
        if (difftime(time_now, queryspeed->triggered) < queryspeed->cooldown)
        {
            double blocked_for = queryspeed->cooldown - difftime(time_now, queryspeed->triggered);
            *msg = create_error("Queries denied for %f seconds", blocked_for);
            matches = true;

            MXS_INFO("rule '%s': user denied for %f seconds",
                     name.c_str(), blocked_for);
        }
        else
        {
            queryspeed->active = false;
            queryspeed->count = 0;
        }
    }
    else
    {
        if (queryspeed->count >= queryspeed->limit)
        {
            MXS_INFO("rule '%s': query limit triggered (%d queries in %d seconds), "
                     "denying queries from user for %d seconds.", name.c_str(),
                     queryspeed->limit, queryspeed->period, queryspeed->cooldown);

            queryspeed->triggered = time_now;
            queryspeed->active = true;
            matches = true;

            double blocked_for = queryspeed->cooldown - difftime(time_now, queryspeed->triggered);
            *msg = create_error("Queries denied for %f seconds", blocked_for);
        }
        else if (queryspeed->count > 0 &&
                 difftime(time_now, queryspeed->first_query) <= queryspeed->period)
        {
            queryspeed->count++;
        }
        else
        {
            queryspeed->first_query = time_now;
            queryspeed->count = 1;
        }
    }

    return matches;
}
