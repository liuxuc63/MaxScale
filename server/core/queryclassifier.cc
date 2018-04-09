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

#include <maxscale/queryclassifier.hh>
#include <tr1/unordered_map>
#include <maxscale/alloc.h>
#include <maxscale/modutil.h>
#include <maxscale/query_classifier.h>
#include <maxscale/protocol/mysql.h>

namespace
{

const int QC_TRACE_MSG_LEN = 1000;


bool are_multi_statements_allowed(MXS_SESSION* pSession)
{
    MySQLProtocol* pPcol = static_cast<MySQLProtocol*>(pSession->client_dcb->protocol);

    if (pPcol->client_capabilities & GW_MYSQL_CAPABILITIES_MULTI_STATEMENTS)
    {
        return true;
    }
    else
    {
        return false;
    }
}

uint32_t get_prepare_type(GWBUF* buffer)
{
    uint32_t type;

    if (mxs_mysql_get_command(buffer) == MXS_COM_STMT_PREPARE)
    {
        // TODO: This could be done inside the query classifier
        size_t packet_len = gwbuf_length(buffer);
        size_t payload_len = packet_len - MYSQL_HEADER_LEN;
        GWBUF* stmt = gwbuf_alloc(packet_len);
        uint8_t* ptr = GWBUF_DATA(stmt);

        // Payload length
        *ptr++ = payload_len;
        *ptr++ = (payload_len >> 8);
        *ptr++ = (payload_len >> 16);
        // Sequence id
        *ptr++ = 0x00;
        // Command
        *ptr++ = MXS_COM_QUERY;

        gwbuf_copy_data(buffer, MYSQL_HEADER_LEN + 1, payload_len - 1, ptr);
        type = qc_get_type_mask(stmt);

        gwbuf_free(stmt);
    }
    else
    {
        GWBUF* stmt = qc_get_preparable_stmt(buffer);
        ss_dassert(stmt);

        type = qc_get_type_mask(stmt);
    }

    ss_dassert((type & (QUERY_TYPE_PREPARE_STMT | QUERY_TYPE_PREPARE_NAMED_STMT)) == 0);

    return type;
}

std::string get_text_ps_id(GWBUF* buffer)
{
    std::string rval;
    char* name = qc_get_prepare_name(buffer);

    if (name)
    {
        rval = name;
        MXS_FREE(name);
    }

    return rval;
}

void replace_binary_ps_id(GWBUF* buffer, uint32_t id)
{
    uint8_t* ptr = GWBUF_DATA(buffer) + MYSQL_PS_ID_OFFSET;
    gw_mysql_set_byte4(ptr, id);
}

}

namespace maxscale
{

class QueryClassifier::PSManager
{
    PSManager(const PSManager&) = delete;
    PSManager& operator = (const PSManager&) = delete;

public:
    PSManager()
    {
    }

    ~PSManager()
    {
    }

    void store(GWBUF* buffer, uint32_t id)
    {
        ss_dassert(mxs_mysql_get_command(buffer) == MXS_COM_STMT_PREPARE ||
                   qc_query_is_type(qc_get_type_mask(buffer),
                                    QUERY_TYPE_PREPARE_NAMED_STMT));

        switch (mxs_mysql_get_command(buffer))
        {
        case MXS_COM_QUERY:
            m_text_ps[get_text_ps_id(buffer)] = get_prepare_type(buffer);
            break;

        case MXS_COM_STMT_PREPARE:
            m_binary_ps[id] = get_prepare_type(buffer);
            break;

        default:
            ss_dassert(!true);
            break;
        }
    }

    uint32_t get_type(uint32_t id) const
    {
        uint32_t rval = QUERY_TYPE_UNKNOWN;
        BinaryPSMap::const_iterator it = m_binary_ps.find(id);

        if (it != m_binary_ps.end())
        {
            rval = it->second;
        }
        else
        {
            MXS_WARNING("Using unknown prepared statement with ID %u", id);
        }

        return rval;
    }

    uint32_t get_type(std::string id) const
    {
        uint32_t rval = QUERY_TYPE_UNKNOWN;
        TextPSMap::const_iterator it = m_text_ps.find(id);

        if (it != m_text_ps.end())
        {
            rval = it->second;
        }
        else
        {
            MXS_WARNING("Using unknown prepared statement with ID '%s'", id.c_str());
        }

        return rval;
    }

    void erase(std::string id)
    {
        if (m_text_ps.erase(id) == 0)
        {
            MXS_WARNING("Closing unknown prepared statement with ID '%s'", id.c_str());
        }
    }

    void erase(uint32_t id)
    {
        if (m_binary_ps.erase(id) == 0)
        {
            MXS_WARNING("Closing unknown prepared statement with ID %u", id);
        }
    }

private:
    typedef std::tr1::unordered_map<uint32_t, uint32_t>    BinaryPSMap;
    typedef std::tr1::unordered_map<std::string, uint32_t> TextPSMap;

private:
    BinaryPSMap m_binary_ps;
    TextPSMap   m_text_ps;
};

//
// QueryClassifier
//

QueryClassifier::QueryClassifier(Handler* pHandler,
                                 MXS_SESSION* pSession,
                                 mxs_target_t use_sql_variables_in)
    : m_pHandler(pHandler)
    , m_pSession(pSession)
    , m_use_sql_variables_in(use_sql_variables_in)
    , m_load_data_state(LOAD_DATA_INACTIVE)
    , m_load_data_sent(0)
    , m_have_tmp_tables(false)
    , m_large_query(false)
    , m_multi_statements_allowed(are_multi_statements_allowed(pSession))
    , m_sPs_manager(new PSManager)
{
}

void QueryClassifier::ps_store(GWBUF* pBuffer, uint32_t id)
{
    return m_sPs_manager->store(pBuffer, id);
}

uint32_t QueryClassifier::ps_get_type(uint32_t id) const
{
    return m_sPs_manager->get_type(id);
}

uint32_t QueryClassifier::ps_get_type(std::string id) const
{
    return m_sPs_manager->get_type(id);
}

void QueryClassifier::ps_erase(std::string id)
{
    return m_sPs_manager->erase(id);
}

void QueryClassifier::ps_erase(uint32_t id)
{
    return m_sPs_manager->erase(id);
}

uint32_t QueryClassifier::get_route_target(uint8_t command, uint32_t qtype, HINT* pHints)
{
    bool trx_active = session_trx_is_active(m_pSession);
    uint32_t target = TARGET_UNDEFINED;
    bool load_active = (m_load_data_state != LOAD_DATA_INACTIVE);

    /**
     * Prepared statements preparations should go to all servers
     */
    if (qc_query_is_type(qtype, QUERY_TYPE_PREPARE_STMT) ||
        qc_query_is_type(qtype, QUERY_TYPE_PREPARE_NAMED_STMT) ||
        command == MXS_COM_STMT_CLOSE ||
        command == MXS_COM_STMT_RESET)
    {
        target = TARGET_ALL;
    }
    /**
     * These queries should be routed to all servers
     */
    else if (!load_active &&
             (qc_query_is_type(qtype, QUERY_TYPE_SESSION_WRITE) ||
              /** Configured to allow writing user variables to all nodes */
              (m_use_sql_variables_in == TYPE_ALL &&
               qc_query_is_type(qtype, QUERY_TYPE_USERVAR_WRITE)) ||
              qc_query_is_type(qtype, QUERY_TYPE_GSYSVAR_WRITE) ||
              /** enable or disable autocommit are always routed to all */
              qc_query_is_type(qtype, QUERY_TYPE_ENABLE_AUTOCOMMIT) ||
              qc_query_is_type(qtype, QUERY_TYPE_DISABLE_AUTOCOMMIT)))
    {
        /**
         * This is problematic query because it would be routed to all
         * backends but since this is SELECT that is not possible:
         * 1. response set is not handled correctly in clientReply and
         * 2. multiple results can degrade performance.
         *
         * Prepared statements are an exception to this since they do not
         * actually do anything but only prepare the statement to be used.
         * They can be safely routed to all backends since the execution
         * is done later.
         *
         * With prepared statement caching the task of routing
         * the execution of the prepared statements to the right server would be
         * an easy one. Currently this is not supported.
         */
        if (qc_query_is_type(qtype, QUERY_TYPE_READ))
        {
            MXS_WARNING("The query can't be routed to all "
                        "backend servers because it includes SELECT and "
                        "SQL variable modifications which is not supported. "
                        "Set use_sql_variables_in=master or split the "
                        "query to two, where SQL variable modifications "
                        "are done in the first and the SELECT in the "
                        "second one.");

            target = TARGET_MASTER;
        }
        target |= TARGET_ALL;
    }
    /**
     * Hints may affect on routing of the following queries
     */
    else if (!trx_active && !load_active &&
             !qc_query_is_type(qtype, QUERY_TYPE_MASTER_READ) &&
             !qc_query_is_type(qtype, QUERY_TYPE_WRITE) &&
             (qc_query_is_type(qtype, QUERY_TYPE_READ) ||
              qc_query_is_type(qtype, QUERY_TYPE_SHOW_TABLES) ||
              qc_query_is_type(qtype, QUERY_TYPE_USERVAR_READ) ||
              qc_query_is_type(qtype, QUERY_TYPE_SYSVAR_READ) ||
              qc_query_is_type(qtype, QUERY_TYPE_GSYSVAR_READ)))
    {
        if (qc_query_is_type(qtype, QUERY_TYPE_USERVAR_READ))
        {
            if (m_use_sql_variables_in == TYPE_ALL)
            {
                target = TARGET_SLAVE;
            }
        }
        else if (qc_query_is_type(qtype, QUERY_TYPE_READ) || // Normal read
                 qc_query_is_type(qtype, QUERY_TYPE_SHOW_TABLES) || // SHOW TABLES
                 qc_query_is_type(qtype, QUERY_TYPE_SYSVAR_READ) || // System variable
                 qc_query_is_type(qtype, QUERY_TYPE_GSYSVAR_READ)) // Global system variable
        {
            target = TARGET_SLAVE;
        }

        /** If nothing matches then choose the master */
        if ((target & (TARGET_ALL | TARGET_SLAVE | TARGET_MASTER)) == 0)
        {
            target = TARGET_MASTER;
        }
    }
    else if (session_trx_is_read_only(m_pSession))
    {
        /* Force TARGET_SLAVE for READ ONLY transaction (active or ending) */
        target = TARGET_SLAVE;
    }
    else
    {
        ss_dassert(trx_active || load_active ||
                   (qc_query_is_type(qtype, QUERY_TYPE_WRITE) ||
                    qc_query_is_type(qtype, QUERY_TYPE_MASTER_READ) ||
                    qc_query_is_type(qtype, QUERY_TYPE_SESSION_WRITE) ||
                    (qc_query_is_type(qtype, QUERY_TYPE_USERVAR_READ) &&
                     m_use_sql_variables_in == TYPE_MASTER) ||
                    (qc_query_is_type(qtype, QUERY_TYPE_SYSVAR_READ) &&
                     m_use_sql_variables_in == TYPE_MASTER) ||
                    (qc_query_is_type(qtype, QUERY_TYPE_GSYSVAR_READ) &&
                     m_use_sql_variables_in == TYPE_MASTER) ||
                    (qc_query_is_type(qtype, QUERY_TYPE_GSYSVAR_WRITE) &&
                     m_use_sql_variables_in == TYPE_MASTER) ||
                    (qc_query_is_type(qtype, QUERY_TYPE_USERVAR_WRITE) &&
                     m_use_sql_variables_in == TYPE_MASTER) ||
                    qc_query_is_type(qtype, QUERY_TYPE_BEGIN_TRX) ||
                    qc_query_is_type(qtype, QUERY_TYPE_ENABLE_AUTOCOMMIT) ||
                    qc_query_is_type(qtype, QUERY_TYPE_DISABLE_AUTOCOMMIT) ||
                    qc_query_is_type(qtype, QUERY_TYPE_ROLLBACK) ||
                    qc_query_is_type(qtype, QUERY_TYPE_COMMIT) ||
                    qc_query_is_type(qtype, QUERY_TYPE_EXEC_STMT) ||
                    qc_query_is_type(qtype, QUERY_TYPE_CREATE_TMP_TABLE) ||
                    qc_query_is_type(qtype, QUERY_TYPE_READ_TMP_TABLE) ||
                    qc_query_is_type(qtype, QUERY_TYPE_UNKNOWN)) ||
                   qc_query_is_type(qtype, QUERY_TYPE_EXEC_STMT));

        target = TARGET_MASTER;
    }

    /** Process routing hints */
    HINT* pHint = pHints;

    while (pHint)
    {
        if (m_pHandler->supports_hint(pHint->type))
        {
            switch (pHint->type)
            {
            case HINT_ROUTE_TO_MASTER:
                // This means override, so we bail out immediately.
                target = TARGET_MASTER;
                MXS_DEBUG("Hint: route to master");
                pHint = NULL;
                break;

            case HINT_ROUTE_TO_NAMED_SERVER:
                // The router is expected to look up the named server.
                target |= TARGET_NAMED_SERVER;
                MXS_DEBUG("Hint: route to named server: %s", (char*)pHint->data);
                break;

            case HINT_ROUTE_TO_UPTODATE_SERVER:
                // TODO: Add generic target type, never to be seem by RWS.
                ss_dassert(false);
                break;

            case HINT_ROUTE_TO_ALL:
                // TODO: Add generic target type, never to be seem by RWS.
                ss_dassert(false);
                break;

            case HINT_PARAMETER:
                if (strncasecmp((char*)pHint->data, "max_slave_replication_lag",
                                strlen("max_slave_replication_lag")) == 0)
                {
                    target |= TARGET_RLAG_MAX;
                }
                else
                {
                    MXS_ERROR("Unknown hint parameter '%s' when "
                              "'max_slave_replication_lag' was expected.",
                              (char*)pHint->data);
                }
                break;

            case HINT_ROUTE_TO_SLAVE:
                target = TARGET_SLAVE;
                MXS_DEBUG("Hint: route to slave.");
            }
        }

        if (pHint)
        {
            pHint = pHint->next;
        }
    }

    return target;
}

namespace
{

// Copy of mxs_mysql_extract_ps_id() in modules/protocol/MySQL/mysql_common.cc,
// but we do not want to create a dependency from maxscale-common to that.

uint32_t mysql_extract_ps_id(GWBUF* buffer)
{
    uint32_t rval = 0;
    uint8_t id[MYSQL_PS_ID_SIZE];

    if (gwbuf_copy_data(buffer, MYSQL_PS_ID_OFFSET, sizeof(id), id) == sizeof(id))
    {
        rval = gw_mysql_get_byte4(id);
    }

    return rval;
}

}

uint32_t QueryClassifier::ps_id_internal_get(GWBUF* pBuffer)
{
    uint32_t internal_id = 0;

    // All COM_STMT type statements store the ID in the same place
    uint32_t external_id = mysql_extract_ps_id(pBuffer);
    auto it = m_ps_handles.find(external_id);

    if (it != m_ps_handles.end())
    {
        internal_id = it->second;
    }
    else
    {
        MXS_WARNING("Client requests unknown prepared statement ID '%u' that "
                    "does not map to an internal ID", external_id);
    }

    return internal_id;
}

void QueryClassifier::ps_id_internal_put(uint32_t external_id, uint32_t internal_id)
{
    m_ps_handles[external_id] = internal_id;
}

void QueryClassifier::log_transaction_status(GWBUF *querybuf, uint32_t qtype)
{
    if (large_query())
    {
        MXS_INFO("> Processing large request with more than 2^24 bytes of data");
    }
    else if (load_data_state() == QueryClassifier::LOAD_DATA_INACTIVE)
    {
        uint8_t *packet = GWBUF_DATA(querybuf);
        unsigned char command = packet[4];
        int len = 0;
        char* sql;
        char *qtypestr = qc_typemask_to_string(qtype);
        if (!modutil_extract_SQL(querybuf, &sql, &len))
        {
            sql = (char*)"<non-SQL>";
        }

        if (len > QC_TRACE_MSG_LEN)
        {
            len = QC_TRACE_MSG_LEN;
        }

        MXS_SESSION *ses = session();
        const char *autocommit = session_is_autocommit(ses) ? "[enabled]" : "[disabled]";
        const char *transaction = session_trx_is_active(ses) ? "[open]" : "[not open]";
        uint32_t plen = MYSQL_GET_PACKET_LEN(querybuf);
        const char *querytype = qtypestr == NULL ? "N/A" : qtypestr;
        const char *hint = querybuf->hint == NULL ? "" : ", Hint:";
        const char *hint_type = querybuf->hint == NULL ? "" : STRHINTTYPE(querybuf->hint->type);

        MXS_INFO("> Autocommit: %s, trx is %s, cmd: (0x%02x) %s, plen: %u, type: %s, stmt: %.*s%s %s",
                 autocommit, transaction, command, STRPACKETTYPE(command), plen,
                 querytype, len, sql, hint, hint_type);

        MXS_FREE(qtypestr);
    }
    else
    {
        MXS_INFO("> Processing LOAD DATA LOCAL INFILE: %lu bytes sent.", load_data_sent());
    }
}

uint32_t QueryClassifier::determine_query_type(GWBUF *querybuf, int command)
{
    uint32_t type = QUERY_TYPE_UNKNOWN;

    switch (command)
    {
    case MXS_COM_QUIT: /*< 1 QUIT will close all sessions */
    case MXS_COM_INIT_DB: /*< 2 DDL must go to the master */
    case MXS_COM_REFRESH: /*< 7 - I guess this is session but not sure */
    case MXS_COM_DEBUG: /*< 0d all servers dump debug info to stdout */
    case MXS_COM_PING: /*< 0e all servers are pinged */
    case MXS_COM_CHANGE_USER: /*< 11 all servers change it accordingly */
    case MXS_COM_SET_OPTION: /*< 1b send options to all servers */
        type = QUERY_TYPE_SESSION_WRITE;
        break;

    case MXS_COM_CREATE_DB: /**< 5 DDL must go to the master */
    case MXS_COM_DROP_DB: /**< 6 DDL must go to the master */
    case MXS_COM_STMT_CLOSE: /*< free prepared statement */
    case MXS_COM_STMT_SEND_LONG_DATA: /*< send data to column */
    case MXS_COM_STMT_RESET: /*< resets the data of a prepared statement */
        type = QUERY_TYPE_WRITE;
        break;

    case MXS_COM_QUERY:
        type = qc_get_type_mask(querybuf);
        break;

    case MXS_COM_STMT_PREPARE:
        type = qc_get_type_mask(querybuf);
        type |= QUERY_TYPE_PREPARE_STMT;
        break;

    case MXS_COM_STMT_EXECUTE:
        /** Parsing is not needed for this type of packet */
        type = QUERY_TYPE_EXEC_STMT;
        break;

    case MXS_COM_SHUTDOWN: /**< 8 where should shutdown be routed ? */
    case MXS_COM_STATISTICS: /**< 9 ? */
    case MXS_COM_PROCESS_INFO: /**< 0a ? */
    case MXS_COM_CONNECT: /**< 0b ? */
    case MXS_COM_PROCESS_KILL: /**< 0c ? */
    case MXS_COM_TIME: /**< 0f should this be run in gateway ? */
    case MXS_COM_DELAYED_INSERT: /**< 10 ? */
    case MXS_COM_DAEMON: /**< 1d ? */
    default:
        break;
    }

    return type;
}

namespace
{

// Copied from mysql_common.c
// TODO: The current database should somehow be available in a generic fashion.
const char* qc_mysql_get_current_db(MXS_SESSION* session)
{
    MYSQL_session* data = (MYSQL_session*)session->client_dcb->data;
    return data->db;
}

}

void QueryClassifier::check_create_tmp_table(GWBUF *querybuf, uint32_t type)
{
    if (qc_query_is_type(type, QUERY_TYPE_CREATE_TMP_TABLE))
    {
        set_have_tmp_tables(true);
        char* tblname = qc_get_created_table_name(querybuf);
        std::string table;

        if (tblname && *tblname && strchr(tblname, '.') == NULL)
        {
            const char* db = qc_mysql_get_current_db(session());
            table += db;
            table += ".";
            table += tblname;
        }

        /** Add the table to the set of temporary tables */
        add_tmp_table(table);

        MXS_FREE(tblname);
    }
}

}

