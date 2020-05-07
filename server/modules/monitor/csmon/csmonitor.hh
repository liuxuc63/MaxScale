/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-03-10
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
#pragma once

#include "csmon.hh"
#include <chrono>
#include <maxbase/http.hh>
#include <maxbase/semaphore.hh>
#include <maxscale/modulecmd.hh>
#include "csconfig.hh"
#include "csmonitorserver.hh"
#include "columnstore.hh"

class CsMonitor : public maxscale::MonitorWorkerSimple
{
public:
    using Base = mxs::MonitorWorkerSimple;

    CsMonitor(const CsMonitor&) = delete;
    CsMonitor& operator=(const CsMonitor&) = delete;

    ~CsMonitor();
    static CsMonitor* create(const std::string& name, const std::string& module);

public:
    using ServerVector = std::vector<CsMonitorServer*>;

    ServerVector get_monitored_serverlist(const std::string& key, bool* error_out)
    {
        const auto& sl = Base::get_monitored_serverlist(key, error_out);

        return reinterpret_cast<const ServerVector&>(sl);
    }

    CsMonitorServer* get_monitored_server(SERVER* search_server)
    {
        return static_cast<CsMonitorServer*>(Base::get_monitored_server(search_server));
    }

    const ServerVector& servers() const
    {
        return reinterpret_cast<const ServerVector&>(Base::servers());
    }

    // Only to be called by the module call command mechanism.
    bool command_add_node(json_t** ppOutput, CsMonitorServer* pServer, const std::chrono::seconds& timeout);
    bool command_config_get(json_t** ppOutput, CsMonitorServer* pServer);
    bool command_config_set(json_t** ppOutput, const char* zJson, CsMonitorServer* pServer);
    bool command_mode_set(json_t** ppOutput, const char* zEnum);
    bool command_ping(json_t** ppOutput, CsMonitorServer* pServer);
    bool command_remove_node(json_t** ppOutput,
                             CsMonitorServer* pServer, const std::chrono::seconds& timeout, bool force);
    bool command_scan(json_t** ppOutput, CsMonitorServer* pServer, const std::chrono::seconds& timeout);
    bool command_shutdown(json_t** ppOutput, const std::chrono::seconds& timeout);
    bool command_start(json_t** ppOutput);
    bool command_status(json_t** ppOutput, CsMonitorServer* pServer);

#if defined(CSMON_EXPOSE_TRANSACTIONS)
    bool command_begin(json_t** ppOutput, const std::chrono::seconds& timeout, CsMonitorServer* pServer);
    bool command_commit(json_t** ppOutput, CsMonitorServer* pServer);
    bool command_rollback(json_t** ppOutput, CsMonitorServer* pServer);
#endif

    using ResponseHandler = std::function<void(CsMonitorServer*,
                                               const mxb::http::Result&,
                                               json_t*)>;


private:
    bool ready_to_run(json_t** ppOutput) const;
    static bool is_valid_json(json_t** ppOutput, const char* zJson, size_t len);

    bool command(json_t** ppOutput, mxb::Semaphore& sem, const char* zCmd, std::function<void()> cmd);

    void cs_add_node(json_t** ppOutput, mxb::Semaphore* pSem,
                     CsMonitorServer* pServer, const std::chrono::seconds& timeout);
    void cs_config_get(json_t** ppOutput, mxb::Semaphore* pSem, CsMonitorServer* pServer);
    void cs_config_set(json_t** ppOutput, mxb::Semaphore* pSem,
                       std::string&& body, CsMonitorServer* pServer);
    void cs_mode_set(json_t** ppOuput, mxb::Semaphore* pSem, cs::ClusterMode mode);
    void cs_ping(json_t** ppOutput, mxb::Semaphore* pSem, CsMonitorServer* pServer);
    void cs_remove_node(json_t** ppOutput, mxb::Semaphore* pSem,
                        CsMonitorServer* pServer, const std::chrono::seconds& timeout, bool force);
    void cs_scan(json_t** ppOutput, mxb::Semaphore* pSem,
                 CsMonitorServer* pServer,
                 const std::chrono::seconds& timeout);
    void cs_shutdown(json_t** ppOutput, mxb::Semaphore* pSem, const std::chrono::seconds& timeout);
    void cs_start(json_t** ppOutput, mxb::Semaphore* pSem);
    void cs_status(json_t** ppOutput, mxb::Semaphore* pSem, CsMonitorServer* pServer);
#if defined(CSMON_EXPOSE_TRANSACTIONS)
    void cs_begin(json_t** ppOutput, mxb::Semaphore* pSem,
                  const std::chrono::seconds& timeout, CsMonitorServer* pServer);
    void cs_commit(json_t** ppOutput, mxb::Semaphore* pSem, CsMonitorServer* pServer);
    void cs_rollback(json_t** ppOutput, mxb::Semaphore* pSem, CsMonitorServer* pServer);
#endif

    bool cs_add_first_multi_node(json_t* pOutput,
                                 CsMonitorServer* pServer,
                                 const std::chrono::seconds& timeout);
    bool cs_add_additional_multi_node(json_t* pOutput,
                                      CsMonitorServer* pServer,
                                      const std::chrono::seconds& timeout);

    std::string create_add_config(CsMonitorServer::Config& config, CsMonitorServer* pServer);
    std::string create_remove_config(CsMonitorServer::Config& config,
                                     CsMonitorServer* pServer,
                                     bool force,
                                     bool is_critical);

    bool has_sufficient_permissions();
    void update_server_status(mxs::MonitorServer* pServer);
    int update_10_server_status(CsMonitorServer* pServer);
    int update_12_server_status(CsMonitorServer* pServer);
    int update_15_server_status(CsMonitorServer* pServer);

    CsMonitorServer* create_server(SERVER* server, const mxs::MonitorServer::SharedSettings& shared) override;

private:
    CsMonitor(const std::string& name, const std::string& module);
    bool configure(const mxs::ConfigParameters* pParams) override;

    CsConfig          m_config;
    mxb::http::Config m_http_config;
    cs::Version       m_version  { cs::CS_UNKNOWN };
    int               m_version_number { -1 };
    const char*       m_zAlive_query { nullptr };
};
