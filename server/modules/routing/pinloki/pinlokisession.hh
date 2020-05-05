/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2023-11-05
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
#pragma once

#include <maxscale/ccdefs.hh>

#include <maxscale/router.hh>

#include "rpl_event.hh"
#include "parser.hh"
#include "master_config.hh"
#include "pinloki.hh"
#include "reader.hh"

namespace pinloki
{

class PinlokiSession : public mxs::RouterSession, public pinloki::parser::Handler
{
public:
    PinlokiSession(const PinlokiSession&) = delete;
    PinlokiSession& operator=(const PinlokiSession&) = delete;

    PinlokiSession(MXS_SESSION* pSession, Pinloki* router);
    virtual ~PinlokiSession() = default;

    void    close();
    int32_t routeQuery(GWBUF* pPacket);
    void    clientReply(GWBUF* pPacket, const mxs::ReplyRoute& down, const mxs::Reply& reply);
    bool    handleError(mxs::ErrorType type, GWBUF* pMessage,
                        mxs::Endpoint* pProblem, const mxs::Reply& pReply);

    // pinloki::parser::Handler API
    void select(const std::vector<std::string>& values) override;
    void set(const std::string& key, const std::string& value) override;
    void change_master_to(const parser::ChangeMasterValues& values) override;
    void start_slave() override;
    void stop_slave() override;
    void reset_slave() override;
    void show_slave_status() override;
    void show_master_status() override;
    void show_binlogs() override;
    void show_variables(const std::string& like) override;
    void purge_logs(const std::string& up_to) override;
    void error(const std::string& err) override;

private:
    uint8_t                 m_seq = 1;  // Packet sequence number, incremented for each sent packet
    Pinloki*                m_router;
    mxq::Gtid               m_gtid;
    std::unique_ptr<Reader> m_reader;
    uint32_t                m_dcid = 0;

    bool send_event(const maxsql::RplEvent& event);
    void send(GWBUF* buffer);
};
}
