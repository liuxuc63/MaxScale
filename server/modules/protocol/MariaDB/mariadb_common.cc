/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2023-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/*
 * MySQL Protocol common routines for client to gateway and gateway to backend
 */

#include <maxscale/protocol/mariadb/protocol_classes.hh>
#include <maxscale/protocol/mariadb/authenticator.hh>

#include <mysql.h>
#include <maxsql/mariadb.hh>
#include <maxbase/alloc.h>
#include <maxscale/modutil.hh>
#include <maxscale/mysql_utils.hh>
#include <maxscale/utils.h>
#include <maxscale/poll.hh>
#include <maxscale/routing.hh>
#include <maxscale/service.hh>
#include <maxscale/target.hh>
#include "packet_parser.hh"

using std::string;
using mxs::ReplyState;
using UserEntry = mariadb::UserEntry;

uint8_t null_client_sha1[MYSQL_SCRAMBLE_LEN] = "";

GWBUF* mysql_create_com_quit(GWBUF* bufparam,
                             int packet_number)
{
    uint8_t* data;
    GWBUF* buf;

    if (bufparam == NULL)
    {
        buf = gwbuf_alloc(COM_QUIT_PACKET_SIZE);
    }
    else
    {
        buf = bufparam;
    }

    if (buf == NULL)
    {
        return 0;
    }
    mxb_assert(GWBUF_LENGTH(buf) == COM_QUIT_PACKET_SIZE);

    data = GWBUF_DATA(buf);

    *data++ = 0x1;
    *data++ = 0x0;
    *data++ = 0x0;
    *data++ = packet_number;
    *data = 0x1;

    return buf;
}

GWBUF* mysql_create_custom_error(int packet_number, int affected_rows, uint16_t errnum, const char* errmsg)
{
    uint8_t mysql_packet_header[4];
    uint8_t field_count = 0xff;
    uint8_t mysql_err[2];
    uint8_t mysql_statemsg[6];
    const char* mysql_state = "HY000";

    mariadb::set_byte2(mysql_err, errnum);
    mysql_statemsg[0] = '#';
    memcpy(mysql_statemsg + 1, mysql_state, 5);


    uint32_t mysql_payload_size =
        sizeof(field_count)
        + sizeof(mysql_err)
        + sizeof(mysql_statemsg)
        + strlen(errmsg);

    /** allocate memory for packet header + payload */
    GWBUF* errbuf = gwbuf_alloc(sizeof(mysql_packet_header) + mysql_payload_size);
    uint8_t* outbuf = GWBUF_DATA(errbuf);

    /** write packet header and packet number */
    mariadb::set_byte3(mysql_packet_header, mysql_payload_size);
    mysql_packet_header[3] = packet_number;

    /** write header */
    memcpy(outbuf, mysql_packet_header, sizeof(mysql_packet_header));

    uint8_t* mysql_payload = outbuf + sizeof(mysql_packet_header);

    /** write field */
    memcpy(mysql_payload, &field_count, sizeof(field_count));
    mysql_payload = mysql_payload + sizeof(field_count);

    /** write errno */
    memcpy(mysql_payload, mysql_err, sizeof(mysql_err));
    mysql_payload = mysql_payload + sizeof(mysql_err);

    /** write sqlstate */
    memcpy(mysql_payload, mysql_statemsg, sizeof(mysql_statemsg));
    mysql_payload = mysql_payload + sizeof(mysql_statemsg);

    /** write error message */
    memcpy(mysql_payload, errmsg, strlen(errmsg));

    return errbuf;
}

GWBUF* mxs_mysql_create_ok(int sequence, uint8_t affected_rows, const char* message)
{
    uint8_t* outbuf = NULL;
    uint32_t mysql_payload_size = 0;
    uint8_t mysql_packet_header[4];
    uint8_t* mysql_payload = NULL;
    uint8_t field_count = 0;
    uint8_t insert_id = 0;
    uint8_t mysql_server_status[2];
    uint8_t mysql_warning_counter[2];
    GWBUF* buf;


    mysql_payload_size =
        sizeof(field_count)
        + sizeof(affected_rows)
        + sizeof(insert_id)
        + sizeof(mysql_server_status)
        + sizeof(mysql_warning_counter);

    if (message != NULL)
    {
        mysql_payload_size += strlen(message);
    }

    // allocate memory for packet header + payload
    if ((buf = gwbuf_alloc(sizeof(mysql_packet_header) + mysql_payload_size)) == NULL)
    {
        return 0;
    }
    outbuf = GWBUF_DATA(buf);

    // write packet header with packet number
    mariadb::set_byte3(mysql_packet_header, mysql_payload_size);
    mysql_packet_header[3] = sequence;

    // write header
    memcpy(outbuf, mysql_packet_header, sizeof(mysql_packet_header));

    mysql_payload = outbuf + sizeof(mysql_packet_header);

    mysql_server_status[0] = 2;
    mysql_server_status[1] = 0;
    mysql_warning_counter[0] = 0;
    mysql_warning_counter[1] = 0;

    // write data
    memcpy(mysql_payload, &field_count, sizeof(field_count));
    mysql_payload = mysql_payload + sizeof(field_count);

    memcpy(mysql_payload, &affected_rows, sizeof(affected_rows));
    mysql_payload = mysql_payload + sizeof(affected_rows);

    memcpy(mysql_payload, &insert_id, sizeof(insert_id));
    mysql_payload = mysql_payload + sizeof(insert_id);

    memcpy(mysql_payload, mysql_server_status, sizeof(mysql_server_status));
    mysql_payload = mysql_payload + sizeof(mysql_server_status);

    memcpy(mysql_payload, mysql_warning_counter, sizeof(mysql_warning_counter));
    mysql_payload = mysql_payload + sizeof(mysql_warning_counter);

    if (message != NULL)
    {
        memcpy(mysql_payload, message, strlen(message));
    }

    return buf;
}

/**
 * @brief Computes the size of the response to the DB initial handshake
 *
 * When the connection is to be SSL, but an SSL connection has not yet been
 * established, only a basic 36 byte response is sent, including the SSL
 * capability flag.
 *
 * Otherwise, the packet size is computed, based on the minimum size and
 * increased by the optional or variable elements.
 *
 * @param with_ssl        SSL is used
 * @param ssl_established SSL is established
 * @param user            Name of the user seeking to connect
 * @param passwd          Password for the user seeking to connect
 * @param dbname          Name of the database to be made default, if any
 *
 * @return The length of the response packet
 */
int response_length(bool with_ssl, bool ssl_established, const char* user,
                    const uint8_t* passwd, const char* dbname, const char* auth_module)
{
    long bytes;

    if (with_ssl && !ssl_established)
    {
        return MYSQL_AUTH_PACKET_BASE_SIZE;
    }

    // Protocol MySQL HandshakeResponse for CLIENT_PROTOCOL_41
    // 4 bytes capabilities + 4 bytes max packet size + 1 byte charset + 23 '\0' bytes
    // 4 + 4 + 1 + 23  = 32
    bytes = 32;

    if (user)
    {
        bytes += strlen(user);
    }
    // the NULL
    bytes++;

    // next will be + 1 (scramble_len) + 20 (fixed_scramble) + 1 (user NULL term) + 1 (db NULL term)

    if (passwd)
    {
        bytes += GW_MYSQL_SCRAMBLE_SIZE;
    }
    bytes++;

    if (dbname && strlen(dbname))
    {
        bytes += strlen(dbname);
        bytes++;
    }

    bytes += strlen(auth_module);
    bytes++;

    // the packet header
    bytes += 4;

    return bytes;
}

void mxs_mysql_calculate_hash(const uint8_t* scramble, const uint8_t* passwd, uint8_t* output)
{
    uint8_t hash1[GW_MYSQL_SCRAMBLE_SIZE] = "";
    uint8_t hash2[GW_MYSQL_SCRAMBLE_SIZE] = "";
    uint8_t new_sha[GW_MYSQL_SCRAMBLE_SIZE] = "";

    // hash1 is the function input, SHA1(real_password)
    memcpy(hash1, passwd, GW_MYSQL_SCRAMBLE_SIZE);

    // hash2 is the SHA1(input data), where input_data = SHA1(real_password)
    gw_sha1_str(hash1, GW_MYSQL_SCRAMBLE_SIZE, hash2);

    // new_sha is the SHA1(CONCAT(scramble, hash2)
    gw_sha1_2_str(scramble, GW_MYSQL_SCRAMBLE_SIZE, hash2, GW_MYSQL_SCRAMBLE_SIZE, new_sha);

    // compute the xor in client_scramble
    mxs::bin_bin_xor(new_sha, hash1, GW_MYSQL_SCRAMBLE_SIZE, output);
}

/**
 * @brief Helper function to load hashed password
 *
 * @param payload Destination where hashed password is written
 * @param passwd Client's double SHA1 password
 *
 * @return Address of the next byte after the end of the stored password
 */
uint8_t* load_hashed_password(const uint8_t* scramble, uint8_t* payload, const uint8_t* passwd)
{
    *payload++ = GW_MYSQL_SCRAMBLE_SIZE;
    mxs_mysql_calculate_hash(scramble, passwd, payload);
    return payload + GW_MYSQL_SCRAMBLE_SIZE;
}

bool mxs_mysql_is_ok_packet(GWBUF* buffer)
{
    uint8_t cmd = 0xff;     // Default should differ from the OK packet
    gwbuf_copy_data(buffer, MYSQL_HEADER_LEN, 1, &cmd);
    return cmd == MYSQL_REPLY_OK;
}

bool mxs_mysql_is_err_packet(GWBUF* buffer)
{
    uint8_t cmd = 0x00;     // Default should differ from the ERR packet
    gwbuf_copy_data(buffer, MYSQL_HEADER_LEN, 1, &cmd);
    return cmd == MYSQL_REPLY_ERR;
}

uint16_t mxs_mysql_get_mysql_errno(GWBUF* buffer)
{
    uint16_t rval = 0;

    if (mxs_mysql_is_err_packet(buffer))
    {
        uint8_t buf[2];
        // First two bytes after the 0xff byte are the error code
        gwbuf_copy_data(buffer, MYSQL_HEADER_LEN + 1, 2, buf);
        rval = mariadb::get_byte2(buf);
    }

    return rval;
}

bool mxs_mysql_is_local_infile(GWBUF* buffer)
{
    uint8_t cmd = 0xff;     // Default should differ from the OK packet
    gwbuf_copy_data(buffer, MYSQL_HEADER_LEN, 1, &cmd);
    return cmd == MYSQL_REPLY_LOCAL_INFILE;
}

bool mxs_mysql_is_prep_stmt_ok(GWBUF* buffer)
{
    bool rval = false;
    uint8_t cmd;

    if (gwbuf_copy_data(buffer, MYSQL_HEADER_LEN, 1, &cmd)
        && cmd == MYSQL_REPLY_OK)
    {
        rval = true;
    }

    return rval;
}

bool mxs_mysql_is_ps_command(uint8_t cmd)
{
    return cmd == MXS_COM_STMT_EXECUTE
           || cmd == MXS_COM_STMT_BULK_EXECUTE
           || cmd == MXS_COM_STMT_SEND_LONG_DATA
           || cmd == MXS_COM_STMT_CLOSE
           || cmd == MXS_COM_STMT_FETCH
           || cmd == MXS_COM_STMT_RESET;
}

bool mxs_mysql_extract_ps_response(GWBUF* buffer, MXS_PS_RESPONSE* out)
{
    bool rval = false;
    uint8_t id[MYSQL_PS_ID_SIZE];
    uint8_t cols[MYSQL_PS_COLS_SIZE];
    uint8_t params[MYSQL_PS_PARAMS_SIZE];
    uint8_t warnings[MYSQL_PS_WARN_SIZE];

    if (gwbuf_copy_data(buffer, MYSQL_PS_ID_OFFSET, sizeof(id), id) == sizeof(id)
        && gwbuf_copy_data(buffer, MYSQL_PS_COLS_OFFSET, sizeof(cols), cols) == sizeof(cols)
        && gwbuf_copy_data(buffer, MYSQL_PS_PARAMS_OFFSET, sizeof(params), params) == sizeof(params)
        && gwbuf_copy_data(buffer, MYSQL_PS_WARN_OFFSET, sizeof(warnings), warnings) == sizeof(warnings))
    {
        out->id = mariadb::get_byte4(id);
        out->columns = mariadb::get_byte2(cols);
        out->parameters = mariadb::get_byte2(params);
        out->warnings = mariadb::get_byte2(warnings);
        rval = true;
    }

    return rval;
}

uint32_t mxs_mysql_extract_ps_id(GWBUF* buffer)
{
    uint32_t rval = 0;
    uint8_t id[MYSQL_PS_ID_SIZE];

    if (gwbuf_copy_data(buffer, MYSQL_PS_ID_OFFSET, sizeof(id), id) == sizeof(id))
    {
        rval = mariadb::get_byte4(id);
    }

    return rval;
}

bool mxs_mysql_command_will_respond(uint8_t cmd)
{
    return cmd != MXS_COM_STMT_SEND_LONG_DATA
           && cmd != MXS_COM_QUIT
           && cmd != MXS_COM_STMT_CLOSE;
}

static inline bool complete_ps_response(GWBUF* buffer)
{
    mxb_assert(gwbuf_is_contiguous(buffer));
    MXS_PS_RESPONSE resp;
    bool rval = false;

    if (mxs_mysql_extract_ps_response(buffer, &resp))
    {
        int expected_packets = 1;

        if (resp.columns > 0)
        {
            // Column definition packets plus one for the EOF
            expected_packets += resp.columns + 1;
        }

        if (resp.parameters > 0)
        {
            // Parameter definition packets plus one for the EOF
            expected_packets += resp.parameters + 1;
        }

        int n_packets = modutil_count_packets(buffer);

        MXS_DEBUG("Expecting %u packets, have %u", n_packets, expected_packets);

        rval = n_packets == expected_packets;
    }

    return rval;
}

bool MYSQL_session::ssl_capable() const
{
    return (client_info.m_client_capabilities & GW_MYSQL_CAPABILITIES_SSL) != 0;
}

uint32_t MYSQL_session::client_capabilities() const
{
    return client_info.m_client_capabilities;
}

uint32_t MYSQL_session::extra_capabilitites() const
{
    return client_info.m_extra_capabilities;
}

MYSQL_session::MYSQL_session(const MYSQL_session& rhs)
    : user(rhs.user)
    , remote(rhs.remote)
    , db(rhs.db)
    , plugin(rhs.plugin)
    , next_sequence(rhs.next_sequence)
    , connect_attrs(rhs.connect_attrs)
    , client_info(rhs.client_info)
    , auth_token(rhs.auth_token)
    , auth_token_phase2(rhs.auth_token_phase2)
    , m_current_authenticator(rhs.m_current_authenticator)
    , user_search_settings(rhs.user_search_settings)
    , user_entry(rhs.user_entry)
{
    memcpy(scramble, rhs.scramble, MYSQL_SCRAMBLE_LEN);
}

std::string MYSQL_session::user_and_host() const
{
    return "'" + user + "'@'" + remote + "'";
}

uint64_t mariadb::AuthenticatorModule::capabilities() const
{
    return 0;
}

bool UserEntry::operator==(const UserEntry& rhs) const
{
    return username == rhs.username && host_pattern == rhs.host_pattern && plugin == rhs.plugin
           && password == rhs.password && auth_string == rhs.auth_string && ssl == rhs.ssl
           && global_db_priv == rhs.global_db_priv && proxy_priv == rhs.proxy_priv && is_role == rhs.is_role
           && default_role == rhs.default_role;
}

bool UserEntry::host_pattern_is_more_specific(const UserEntry& lhs, const UserEntry& rhs)
{
    // Order entries according to https://mariadb.com/kb/en/library/create-user/
    const string& lhost = lhs.host_pattern;
    const string& rhost = rhs.host_pattern;
    const char wildcards[] = "%_";
    auto lwc_pos = lhost.find_first_of(wildcards);
    auto rwc_pos = rhost.find_first_of(wildcards);
    bool lwc = (lwc_pos != string::npos);
    bool rwc = (rwc_pos != string::npos);

    // The host without wc:s sorts earlier than the one with them,
    return (!lwc && rwc)
            // ... and if both have wc:s, the one with the later wc wins (ties broken by strcmp),
           || (lwc && rwc && ((lwc_pos > rwc_pos) || (lwc_pos == rwc_pos && lhost < rhost)))
            // ... and if neither have wildcards, use string order.
           || (!lwc && !rwc && lhost < rhost);
}

/**
 * Read a complete MySQL-protocol packet to output buffer. Returns false on read error.
 *
 * @param dcb Dcb to read from
 * @param output Output for read packet. Should be empty before calling.
 * @return True, if reading succeeded. Also returns true if the entire packet was not yet available and
 * the function should be called again later.
 */
bool read_protocol_packet(DCB* dcb, mxs::Buffer* output)
{
    // TODO: add optimization where the dcb readq is checked first, as it may contain a complete protocol
    // packet.
    const int MAX_PACKET_SIZE = MYSQL_PACKET_LENGTH_MAX + MYSQL_HEADER_LEN;
    GWBUF* read_buffer = nullptr;
    int buffer_len = dcb->read(&read_buffer, MAX_PACKET_SIZE);
    if (buffer_len < 0)
    {
        return false;
    }

    if (buffer_len >= MYSQL_HEADER_LEN)
    {
        // Got enough that the entire packet may be available.

        // Ensure that the HEADER + command byte is contiguous. This simplifies further parsing.
        // In the vast majority of cases the start of the buffer is already contiguous.
        auto link_len = gwbuf_link_length(read_buffer);
        if ((buffer_len == MYSQL_HEADER_LEN && link_len < MYSQL_HEADER_LEN)
            || (buffer_len > MYSQL_HEADER_LEN && link_len <= MYSQL_HEADER_LEN))
        {
            read_buffer = gwbuf_make_contiguous(read_buffer);
        }

        int prot_packet_len = MYSQL_GET_PACKET_LEN(read_buffer);

        // Protocol packet length read. Either received more than the packet, the exact packet or
        // a partial packet.
        if (prot_packet_len < buffer_len)
        {
            // Got more than needed, save extra to DCB and trigger a read.
            auto first_packet = gwbuf_split(&read_buffer, prot_packet_len);
            output->reset(first_packet);
            dcb->readq_prepend(read_buffer);
            dcb->trigger_read_event();
        }
        else if (prot_packet_len == buffer_len)
        {
            // Read exact packet. Return it.
            output->reset(read_buffer);
            if (buffer_len == MAX_PACKET_SIZE && dcb->socket_bytes_readable() > 0)
            {
                // Read a maximally long packet when socket has even more. Route this packet,
                // then read again.
                dcb->trigger_read_event();
            }
        }
        else
        {
            // Could not read enough, try again later. Save results to dcb.
            dcb->readq_prepend(read_buffer);
        }
    }
    else if (buffer_len > 0)
    {
        // Too little data. Save and wait for more.
        dcb->readq_prepend(read_buffer);
    }
    else
    {
        // No data was read even though event handler was called. This may happen due to manually triggered
        // reads (e.g. during SSL-init).
    }
    return true;
}

namespace mariadb
{
void set_byte2(uint8_t* buffer, uint16_t val)
{
    uint16_t le16 = htole16(val);
    auto ple16 = reinterpret_cast<uint16_t*>(buffer);
    *ple16 = le16;
}

void set_byte3(uint8_t* buffer, uint32_t val)
{
    set_byte2(buffer, val);
    buffer[2] = (val >> 16);
}

void set_byte4(uint8_t* buffer, uint32_t val)
{
    uint32_t le32 = htole32(val);
    auto ple32 = reinterpret_cast<uint32_t*>(buffer);
    *ple32 = le32;
}

void set_byte8(uint8_t* buffer, uint64_t val)
{
    uint64_t le64 = htole64(val);
    auto ple64 = reinterpret_cast<uint64_t*>(buffer);
    *ple64 = le64;
}

uint16_t get_byte2(const uint8_t* buffer)
{
    uint16_t le16 = *(reinterpret_cast<const uint16_t*>(buffer));
    auto host16 = le16toh(le16);
    return host16;
}

uint32_t get_byte3(const uint8_t* buffer)
{
    uint32_t low = get_byte2(buffer);
    uint32_t high = buffer[2];
    return low | (high << 16);
}

uint32_t get_byte4(const uint8_t* buffer)
{
    uint32_t le32 = *(reinterpret_cast<const uint32_t*>(buffer));
    auto host32 = le32toh(le32);
    return host32;
}

uint64_t get_byte8(const uint8_t* buffer)
{
    uint64_t le64 = *(reinterpret_cast<const uint64_t*>(buffer));
    auto host64 = le64toh(le64);
    return host64;
}

BackendAuthData::BackendAuthData(const char* srv_name)
    : servername(srv_name)
{
}

AuthSwitchReqContents parse_auth_switch_request(const mxs::Buffer& input)
{
    int datalen = input.length() - MYSQL_HEADER_LEN;
    mxb_assert(datalen >= 0);
    packet_parser::ByteVec data;
    data.resize(datalen);
    gwbuf_copy_data(input.get(), MYSQL_HEADER_LEN, datalen, data.data());
    return packet_parser::parse_auth_switch_request(data);
}
}
