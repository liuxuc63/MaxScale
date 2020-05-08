/*
 * Copyright (c) 2016 MariaDB Corporation Ab
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

#include "csmonitor.hh"
#include <chrono>

namespace
{

const char ARG_MONITOR_DESC[] = "Monitor name";

const char CSMON_ADD_NODE_DESC[]    = "Add a node to a Columnstore cluster.";
const char CSMON_CONFIG_GET_DESC[]  = "Get Columnstore cluster [or server] config.";
const char CSMON_CONFIG_SET_DESC[]  = "Set Columnstore cluster [or server] config.";
const char CSMON_MODE_SET_DESC[]    = "Set Columnstore cluster mode.";
const char CSMON_PING_DESC[]        = "Ping Columnstore cluster [or server].";
const char CSMON_REMOVE_NODE_DESC[] = "Remove a node from a Columnstore cluster.";
const char CSMON_SCAN_DESC[]        = "Scan Columnstore cluster [or server].";
const char CSMON_SHUTDOWN_DESC[]    = "Shutdown Columnstore cluster [or server].";
const char CSMON_START_DESC[]       = "Start Columnstore cluster [or server].";
const char CSMON_STATUS_DESC[]      = "Get Columnstore cluster [or server] status.";


const modulecmd_arg_type_t csmon_add_node_argv[] =
{
    { MODULECMD_ARG_MONITOR | MODULECMD_ARG_NAME_MATCHES_DOMAIN, ARG_MONITOR_DESC },
    { MODULECMD_ARG_SERVER, "Server to add to Columnstore cluster" },
    { MODULECMD_ARG_STRING, "Timeout, 0 means no timeout." }
};

const modulecmd_arg_type_t csmon_config_get_argv[] =
{
    { MODULECMD_ARG_MONITOR | MODULECMD_ARG_NAME_MATCHES_DOMAIN, ARG_MONITOR_DESC },
    { MODULECMD_ARG_SERVER | MODULECMD_ARG_OPTIONAL, "Specific server to to obtain config from" }
};

const modulecmd_arg_type_t csmon_config_set_argv[] =
{
    { MODULECMD_ARG_MONITOR | MODULECMD_ARG_NAME_MATCHES_DOMAIN, ARG_MONITOR_DESC },
    { MODULECMD_ARG_STRING, "Configuration as JSON object" },
    { MODULECMD_ARG_SERVER | MODULECMD_ARG_OPTIONAL, "Specific server to configure" }
};

const modulecmd_arg_type_t csmon_mode_set_argv[]
{
    { MODULECMD_ARG_MONITOR | MODULECMD_ARG_NAME_MATCHES_DOMAIN, ARG_MONITOR_DESC },
    { MODULECMD_ARG_STRING, "Cluster mode; readonly or readwrite" }
};

const modulecmd_arg_type_t csmon_ping_argv[] =
{
    { MODULECMD_ARG_MONITOR | MODULECMD_ARG_NAME_MATCHES_DOMAIN, ARG_MONITOR_DESC },
    { MODULECMD_ARG_SERVER | MODULECMD_ARG_OPTIONAL, "Specific server to ping" }
};

const modulecmd_arg_type_t csmon_remove_node_argv[] =
{
    { MODULECMD_ARG_MONITOR | MODULECMD_ARG_NAME_MATCHES_DOMAIN, ARG_MONITOR_DESC },
    { MODULECMD_ARG_SERVER, "Server to remove from Columnstore cluster" },
    { MODULECMD_ARG_STRING, "Timeout, 0 means no timeout." },
    { MODULECMD_ARG_BOOLEAN, "Whether force should be in effect or not" }
};

const modulecmd_arg_type_t csmon_scan_argv[] =
{
    { MODULECMD_ARG_MONITOR | MODULECMD_ARG_NAME_MATCHES_DOMAIN, ARG_MONITOR_DESC },
    { MODULECMD_ARG_SERVER, "Server to scan" },
    { MODULECMD_ARG_STRING, "Timeout, 0 means no timeout." }
};

const modulecmd_arg_type_t csmon_shutdown_argv[] =
{
    { MODULECMD_ARG_MONITOR | MODULECMD_ARG_NAME_MATCHES_DOMAIN, ARG_MONITOR_DESC },
    { MODULECMD_ARG_STRING, "Timeout, 0 means no timeout." }
};

const modulecmd_arg_type_t csmon_start_argv[] =
{
    { MODULECMD_ARG_MONITOR | MODULECMD_ARG_NAME_MATCHES_DOMAIN, ARG_MONITOR_DESC }
};

const modulecmd_arg_type_t csmon_status_argv[] =
{
    { MODULECMD_ARG_MONITOR | MODULECMD_ARG_NAME_MATCHES_DOMAIN, ARG_MONITOR_DESC },
    { MODULECMD_ARG_SERVER | MODULECMD_ARG_OPTIONAL, "Specific server to query status" }
};


bool get_args(const MODULECMD_ARG* pArgs,
              json_t** ppOutput,
              CsMonitor** ppMonitor)
{
    bool rv = true;

    mxb_assert(MODULECMD_GET_TYPE(&pArgs->argv[0].type) == MODULECMD_ARG_MONITOR);

    CsMonitor* pMonitor = static_cast<CsMonitor*>(pArgs->argv[0].value.monitor);

    *ppMonitor = pMonitor;

    return rv;
}

bool get_args(const MODULECMD_ARG* pArgs,
              json_t** ppOutput,
              CsMonitor** ppMonitor,
              CsMonitorServer** ppServer)
{
    bool rv = true;

    mxb_assert(MODULECMD_GET_TYPE(&pArgs->argv[0].type) == MODULECMD_ARG_MONITOR);
    mxb_assert(pArgs->argc == 1 || MODULECMD_GET_TYPE(&pArgs->argv[1].type) == MODULECMD_ARG_SERVER);

    CsMonitor* pMonitor = static_cast<CsMonitor*>(pArgs->argv[0].value.monitor);
    CsMonitorServer* pServer = nullptr;

    if (pArgs->argc >= 2)
    {
        pServer = pMonitor->get_monitored_server(pArgs->argv[1].value.server);

        if (!pServer)
        {
            LOG_APPEND_JSON_ERROR(ppOutput, "The provided server '%s' is not monitored by this monitor.",
                                  pArgs->argv[1].value.server->name());
            rv = false;
        }
    }

    *ppMonitor = pMonitor;
    *ppServer = pServer;

    return rv;
}

bool get_args(const MODULECMD_ARG* pArgs,
              json_t** ppOutput,
              CsMonitor** ppMonitor,
              const char** pzText)
{
    bool rv = true;

    mxb_assert(pArgs->argc >= 2);
    mxb_assert(MODULECMD_GET_TYPE(&pArgs->argv[0].type) == MODULECMD_ARG_MONITOR);
    mxb_assert(MODULECMD_GET_TYPE(&pArgs->argv[1].type) == MODULECMD_ARG_STRING);

    CsMonitor* pMonitor = static_cast<CsMonitor*>(pArgs->argv[0].value.monitor);
    const char* zText = pArgs->argv[1].value.string;

    *ppMonitor = pMonitor;
    *pzText = zText;

    return rv;
}

bool get_args(const MODULECMD_ARG* pArgs,
              json_t** ppOutput,
              CsMonitor** ppMonitor,
              const char** pzText,
              CsMonitorServer** ppServer)
{
    bool rv = true;

    mxb_assert(MODULECMD_GET_TYPE(&pArgs->argv[0].type) == MODULECMD_ARG_MONITOR);
    mxb_assert(pArgs->argc <= 1 || MODULECMD_GET_TYPE(&pArgs->argv[1].type) == MODULECMD_ARG_STRING);
    mxb_assert(pArgs->argc <= 2 || MODULECMD_GET_TYPE(&pArgs->argv[2].type) == MODULECMD_ARG_SERVER);

    CsMonitor* pMonitor = static_cast<CsMonitor*>(pArgs->argv[0].value.monitor);
    const char* zText = nullptr;
    CsMonitorServer* pServer = nullptr;

    if (pArgs->argc >= 2)
    {
        zText = pArgs->argv[1].value.string;

        if (pArgs->argc >= 3)
        {
            pServer = pMonitor->get_monitored_server(pArgs->argv[2].value.server);
        }
    }

    *ppMonitor = pMonitor;
    *pzText = zText;
    *ppServer = pServer;

    return rv;
}

bool get_args(const MODULECMD_ARG* pArgs,
              json_t** ppOutput,
              CsMonitor** ppMonitor,
              CsMonitorServer** ppServer,
              const char** pzText,
              bool* pBool = nullptr)
{
    bool rv = true;

    mxb_assert(MODULECMD_GET_TYPE(&pArgs->argv[0].type) == MODULECMD_ARG_MONITOR);
    mxb_assert(pArgs->argc <= 1 || MODULECMD_GET_TYPE(&pArgs->argv[1].type) == MODULECMD_ARG_SERVER);
    mxb_assert(pArgs->argc <= 2 || MODULECMD_GET_TYPE(&pArgs->argv[2].type) == MODULECMD_ARG_STRING);
    mxb_assert(pArgs->argc <= 3 || MODULECMD_GET_TYPE(&pArgs->argv[3].type) == MODULECMD_ARG_BOOLEAN);

    CsMonitor* pMonitor = static_cast<CsMonitor*>(pArgs->argv[0].value.monitor);
    CsMonitorServer* pServer = nullptr;
    const char* zText = nullptr;
    bool boolean = false;

    if (pArgs->argc >= 2)
    {
        pServer = pMonitor->get_monitored_server(pArgs->argv[1].value.server);

        if (!pServer)
        {
            LOG_APPEND_JSON_ERROR(ppOutput, "The provided server '%s' is not monitored by this monitor.",
                                  pArgs->argv[1].value.server->name());
            rv = false;
        }

        if (pArgs->argc >= 3)
        {
            zText = pArgs->argv[2].value.string;

            if (pArgs->argc >= 4)
            {
                boolean = pArgs->argv[3].value.boolean;
            }
        }
    }

    *ppMonitor = pMonitor;
    *ppServer = pServer;
    *pzText = zText;

    if (pBool)
    {
        *pBool = boolean;
    }

    return rv;
}

bool get_timeout(const char* zTimeout, std::chrono::seconds* pTimeout, json_t** ppOutput)
{
    bool rv = true;

    *pTimeout = std::chrono::seconds(0);

    if (strcmp(zTimeout, "0") != 0)
    {
        std::chrono::milliseconds duration;
        mxs::config::DurationUnit unit;
        rv = get_suffixed_duration(zTimeout, mxs::config::NO_INTERPRETATION, &duration, &unit);

        if (rv)
        {
            if (unit == mxs::config::DURATION_IN_MILLISECONDS)
            {
                MXS_WARNING("Duration specified in milliseconds, will be converted to seconds.");
            }

            *pTimeout = std::chrono::duration_cast<std::chrono::seconds>(duration);
        }
        else
        {
            LOG_APPEND_JSON_ERROR(ppOutput,
                                  "The timeout must be 0, or specified with a s, m, or h suffix");
            rv = false;
        }
    }

    return rv;
}

#define CALL_IF_CS_15(expression)\
    do\
        if (pMonitor->config().version == cs::CS_15)\
        {\
            rv = expression;\
        }\
        else\
        {\
            LOG_APPEND_JSON_ERROR(ppOutput, "The call command is supported only with Columnstore %s.",\
                                  cs::to_version_string(cs::CS_15));\
            rv = false;\
        }\
    while (false)

bool csmon_add_node(const MODULECMD_ARG* pArgs, json_t** ppOutput)
{
    CsMonitor* pMonitor;
    CsMonitorServer* pServer;
    const char* zTimeout;

    bool rv = get_args(pArgs, ppOutput, &pMonitor, &pServer, &zTimeout);

    if (rv)
    {
        std::chrono::seconds timeout(0);

        if (get_timeout(zTimeout, &timeout, ppOutput))
        {
            CALL_IF_CS_15(pMonitor->command_add_node(ppOutput, pServer, timeout));
        }
    }

    return rv;
}

bool csmon_config_get(const MODULECMD_ARG* pArgs, json_t** ppOutput)
{
    CsMonitor* pMonitor;
    CsMonitorServer* pServer;

    bool rv = get_args(pArgs, ppOutput, &pMonitor, &pServer);

    if (rv)
    {
        CALL_IF_CS_15(pMonitor->command_config_get(ppOutput, pServer));
    }

    return rv;
}

bool csmon_config_set(const MODULECMD_ARG* pArgs, json_t** ppOutput)
{
    CsMonitor* pMonitor;
    const char* zJson;
    CsMonitorServer* pServer;

    bool rv = get_args(pArgs, ppOutput, &pMonitor, &zJson, &pServer);

    if (rv)
    {
        CALL_IF_CS_15(pMonitor->command_config_set(ppOutput, zJson, pServer));
    }

    return rv;
}

bool csmon_mode_set(const MODULECMD_ARG* pArgs, json_t** ppOutput)
{
    CsMonitor* pMonitor;
    const char* zMode;

    bool rv = get_args(pArgs, ppOutput, &pMonitor, &zMode);

    if (rv)
    {
        CALL_IF_CS_15(pMonitor->command_mode_set(ppOutput, zMode));
    }

    return rv;
}

bool csmon_ping(const MODULECMD_ARG* pArgs, json_t** ppOutput)
{
    CsMonitor* pMonitor;
    CsMonitorServer* pServer;

    bool rv = get_args(pArgs, ppOutput, &pMonitor, &pServer);

    if (rv)
    {
        CALL_IF_CS_15(pMonitor->command_ping(ppOutput, pServer));
    }

    return rv;
}

bool csmon_remove_node(const MODULECMD_ARG* pArgs, json_t** ppOutput)
{
    CsMonitor* pMonitor;
    CsMonitorServer* pServer;
    const char* zTimeout;
    bool force;

    bool rv = get_args(pArgs, ppOutput, &pMonitor, &pServer, &zTimeout, &force);

    if (rv)
    {
        std::chrono::seconds timeout(0);

        if (get_timeout(zTimeout, &timeout, ppOutput))
        {
            CALL_IF_CS_15(pMonitor->command_remove_node(ppOutput, pServer, timeout, force));
        }
    }

    return rv;
}

bool csmon_scan(const MODULECMD_ARG* pArgs, json_t** ppOutput)
{
    CsMonitor* pMonitor;
    CsMonitorServer* pServer;
    const char* zTimeout;

    bool rv = get_args(pArgs, ppOutput, &pMonitor, &pServer, &zTimeout);

    if (rv)
    {
        std::chrono::seconds timeout(0);

        if (get_timeout(zTimeout, &timeout, ppOutput))
        {
            CALL_IF_CS_15(pMonitor->command_scan(ppOutput, pServer, timeout));
        }
    }

    return rv;
}

bool csmon_shutdown(const MODULECMD_ARG* pArgs, json_t** ppOutput)
{
    CsMonitor* pMonitor;
    const char* zTimeout;

    bool rv = get_args(pArgs, ppOutput, &pMonitor, &zTimeout);

    if (rv)
    {
        std::chrono::seconds timeout(0);

        if (get_timeout(zTimeout, &timeout, ppOutput))
        {
            CALL_IF_CS_15(pMonitor->command_shutdown(ppOutput, timeout));
        }
    }

    return rv;
}

bool csmon_start(const MODULECMD_ARG* pArgs, json_t** ppOutput)
{
    CsMonitor* pMonitor;

    bool rv = get_args(pArgs, ppOutput, &pMonitor);

    if (rv)
    {
        CALL_IF_CS_15(pMonitor->command_start(ppOutput));
    }

    return rv;
}

bool csmon_status(const MODULECMD_ARG* pArgs, json_t** ppOutput)
{
    CsMonitor* pMonitor;
    CsMonitorServer* pServer;

    bool rv = get_args(pArgs, ppOutput, &pMonitor, &pServer);

    if (rv)
    {
        CALL_IF_CS_15(pMonitor->command_status(ppOutput, pServer));
    }

    return rv;
}

#if defined(CSMON_EXPOSE_TRANSACTIONS)
const char CSMON_BEGIN_DESC[]    = "Begin a transaction.";
const char CSMON_COMMIT_DESC[]   = "Commit a transaction.";
const char CSMON_ROLLBACK_DESC[] = "Rollback a trancation.";

const modulecmd_arg_type_t csmon_begin_argv[] =
{
    { MODULECMD_ARG_MONITOR | MODULECMD_ARG_NAME_MATCHES_DOMAIN, ARG_MONITOR_DESC },
    { MODULECMD_ARG_STRING, "Timeout, 0 means no timeout." },
    { MODULECMD_ARG_SERVER | MODULECMD_ARG_OPTIONAL, "Specific server to begin transaction on" }
};

const modulecmd_arg_type_t csmon_commit_argv[] =
{
    { MODULECMD_ARG_MONITOR | MODULECMD_ARG_NAME_MATCHES_DOMAIN, ARG_MONITOR_DESC },
    { MODULECMD_ARG_SERVER | MODULECMD_ARG_OPTIONAL, "Specific server to commit transaction on" }
};

const modulecmd_arg_type_t csmon_rollback_argv[] =
{
    { MODULECMD_ARG_MONITOR | MODULECMD_ARG_NAME_MATCHES_DOMAIN, ARG_MONITOR_DESC },
    { MODULECMD_ARG_SERVER | MODULECMD_ARG_OPTIONAL, "Specific server to rollback transaction on" }
};

bool csmon_begin(const MODULECMD_ARG* pArgs, json_t** ppOutput)
{
    CsMonitor* pMonitor;
    const char* zTimeout;
    CsMonitorServer* pServer;

    bool rv = get_args(pArgs, ppOutput, &pMonitor, &zTimeout, &pServer);

    if (rv)
    {
        std::chrono::seconds timeout(0);

        if (get_timeout(zTimeout, &timeout, ppOutput))
        {
            CALL_IF_CS_15(pMonitor->command_begin(ppOutput, timeout, pServer));
        }
    }

    return rv;
}

bool csmon_commit(const MODULECMD_ARG* pArgs, json_t** ppOutput)
{
    CsMonitor* pMonitor;
    CsMonitorServer* pServer;

    bool rv = get_args(pArgs, ppOutput, &pMonitor, &pServer);

    if (rv)
    {
        CALL_IF_CS_15(pMonitor->command_commit(ppOutput, pServer));
    }

    return rv;
}

bool csmon_rollback(const MODULECMD_ARG* pArgs, json_t** ppOutput)
{
    CsMonitor* pMonitor;
    CsMonitorServer* pServer;

    bool rv = get_args(pArgs, ppOutput, &pMonitor, &pServer);

    if (rv)
    {
        CALL_IF_CS_15(pMonitor->command_rollback(ppOutput, pServer));
    }

    return rv;
}

#endif


void register_commands()
{
    modulecmd_register_command(MXS_MODULE_NAME, "add-node", MODULECMD_TYPE_ACTIVE,
                               csmon_add_node,
                               MXS_ARRAY_NELEMS(csmon_add_node_argv), csmon_add_node_argv,
                               CSMON_ADD_NODE_DESC);

    modulecmd_register_command(MXS_MODULE_NAME, "config-get", MODULECMD_TYPE_PASSIVE,
                               csmon_config_get,
                               MXS_ARRAY_NELEMS(csmon_config_get_argv), csmon_config_get_argv,
                               CSMON_CONFIG_GET_DESC);

    modulecmd_register_command(MXS_MODULE_NAME, "config-set", MODULECMD_TYPE_PASSIVE,
                               csmon_config_set,
                               MXS_ARRAY_NELEMS(csmon_config_set_argv), csmon_config_set_argv,
                               CSMON_CONFIG_SET_DESC);

    modulecmd_register_command(MXS_MODULE_NAME, "mode-set", MODULECMD_TYPE_ACTIVE,
                               csmon_mode_set,
                               MXS_ARRAY_NELEMS(csmon_mode_set_argv), csmon_mode_set_argv,
                               CSMON_MODE_SET_DESC);

    modulecmd_register_command(MXS_MODULE_NAME, "ping", MODULECMD_TYPE_PASSIVE,
                               csmon_ping,
                               MXS_ARRAY_NELEMS(csmon_ping_argv), csmon_ping_argv,
                               CSMON_PING_DESC);

    modulecmd_register_command(MXS_MODULE_NAME, "remove-node", MODULECMD_TYPE_ACTIVE,
                               csmon_remove_node,
                               MXS_ARRAY_NELEMS(csmon_remove_node_argv), csmon_remove_node_argv,
                               CSMON_REMOVE_NODE_DESC);

    modulecmd_register_command(MXS_MODULE_NAME, "scan", MODULECMD_TYPE_ACTIVE,
                               csmon_scan,
                               MXS_ARRAY_NELEMS(csmon_scan_argv), csmon_scan_argv,
                               CSMON_SCAN_DESC);

    modulecmd_register_command(MXS_MODULE_NAME, "shutdown", MODULECMD_TYPE_ACTIVE,
                               csmon_shutdown,
                               MXS_ARRAY_NELEMS(csmon_shutdown_argv), csmon_shutdown_argv,
                               CSMON_SHUTDOWN_DESC);

    modulecmd_register_command(MXS_MODULE_NAME, "start", MODULECMD_TYPE_ACTIVE,
                               csmon_start,
                               MXS_ARRAY_NELEMS(csmon_start_argv), csmon_start_argv,
                               CSMON_START_DESC);

    modulecmd_register_command(MXS_MODULE_NAME, "status", MODULECMD_TYPE_PASSIVE,
                               csmon_status,
                               MXS_ARRAY_NELEMS(csmon_status_argv), csmon_status_argv,
                               CSMON_STATUS_DESC);

#if defined(CSMON_EXPOSE_TRANSACTIONS)
    modulecmd_register_command(MXS_MODULE_NAME, "begin", MODULECMD_TYPE_PASSIVE,
                               csmon_begin,
                               MXS_ARRAY_NELEMS(csmon_begin_argv), csmon_begin_argv,
                               CSMON_BEGIN_DESC);

    modulecmd_register_command(MXS_MODULE_NAME, "commit", MODULECMD_TYPE_PASSIVE,
                               csmon_commit,
                               MXS_ARRAY_NELEMS(csmon_commit_argv), csmon_commit_argv,
                               CSMON_COMMIT_DESC);

    modulecmd_register_command(MXS_MODULE_NAME, "rollback", MODULECMD_TYPE_PASSIVE,
                               csmon_rollback,
                               MXS_ARRAY_NELEMS(csmon_rollback_argv), csmon_rollback_argv,
                               CSMON_ROLLBACK_DESC);
#endif
}
}

extern "C" MXS_MODULE* MXS_CREATE_MODULE()
{
    static MXS_MODULE info =
    {
        MXS_MODULE_API_MONITOR,
        MXS_MODULE_BETA_RELEASE,
        MXS_MONITOR_VERSION,
        "MariaDB ColumnStore monitor",
        "V1.0.0",
        MXS_NO_MODULE_CAPABILITIES,
        &maxscale::MonitorApi<CsMonitor>::s_api,
        NULL,                                   /* Process init. */
        NULL,                                   /* Process finish. */
        NULL,                                   /* Thread init. */
        NULL,                                   /* Thread finish. */
    };

    static bool populated = false;

    if (!populated)
    {
        register_commands();

        CsConfig::populate(info);
        populated = true;
    }

    return &info;
}
