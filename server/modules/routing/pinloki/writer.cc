#include "writer.hh"
#include "config.hh"
#include "file_writer.hh"
#include "inventory.hh"
#include "pinloki.hh"
#include <maxbase/hexdump.hh>

#include <fstream>
#include <iostream>
#include <iomanip>
#include <thread>
#include <assert.h>

#include <assert.h>
#include <mariadb_rpl.h>

using namespace std::literals::string_literals;

// TODO multidomain is not handled, except for the state of replication (or m_current_gtid_list).
//      Incidentally this works with multidomain, as long as the master and any new master have
//      the same exact binlogs.
namespace pinloki
{

Writer::Writer()
{
    std::string gtid_list_str;
    std::ifstream ifs(config().gtid_file_path());

    if (ifs.good())
    {
        ifs >> gtid_list_str;
    }
    else
    {
        gtid_list_str = config().boot_strap_gtid_list();
        m_is_bootstrap = true;
    }
    maxsql::Gtid gtid = maxsql::Gtid::from_string(gtid_list_str);

    std::cout << "Boot state = " << gtid_list_str << "\n";

    maxbase::Host host = maxbase::Host::from_string("127.0.0.1:4001");
    m_sConnection.reset(new maxsql::Connection({host, "test", "maxskysql", "skysql"}));
    m_sConnection->start_replication(42, gtid);
}

void Writer::run()
{
    FileWriter file(!m_is_bootstrap);

    for (;;)
    {
        std::cout << "******************************\n";

        auto rpl_msg = m_sConnection->get_rpl_msg();
        auto& rpl_event = rpl_msg.event();

        maxbase::hexdump(std::cout, rpl_msg.raw_data(), rpl_msg.raw_data_size());

        switch (rpl_event.event_type)
        {
        case GTID_EVENT:
            {
                save_gtid_list();

                auto& egtid = rpl_event.event.gtid;     // TODO, make

                auto gtid = maxsql::Gtid(egtid.domain_id, rpl_event.server_id, egtid.sequence_nr);
                std::cout << "XXX egtid = " << gtid << "\n";

                m_current_gtid_list.replace(gtid);
            }
            break;

        // TODO, which events can be commits?
        case QUERY_EVENT:
        case XID_EVENT:
            save_gtid_list();
            break;

        default:
            // pass
            ;
        }

        std::cout << rpl_msg << std::endl;

        file.add_event(rpl_msg);

        std::cout.flush();
    }
}

void Writer::save_gtid_list()
{
    if (m_current_gtid_list.is_valid())
    {
        std::ofstream ofs(config().gtid_file_path());
        ofs << m_current_gtid_list;
        // m_current_gtid_list.clear(); TODO change of logic after gitid => gtid list change
    }
}
}
