/*
 * Copyright (c) 2018 MariaDB Corporation Ab
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
#pragma once

#include <maxscale/ccdefs.hh>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <maxscale/buffer.hh>
#include <maxscale/dcb.hh>
#include <maxscale/session.hh>
#include <maxscale/version.h>
#include <maxscale/protocol2.hh>
#include <maxscale/protocol/mariadb/common_constants.hh>

// Default version string sent to clients
#define DEFAULT_VERSION_STRING "5.5.5-10.2.12 " MAXSCALE_VERSION "-maxscale"

#define MYSQL_HEADER_LEN         4
#define MYSQL_CHECKSUM_LEN       4
#define MYSQL_EOF_PACKET_LEN     9
#define MYSQL_OK_PACKET_MIN_LEN  11
#define MYSQL_ERR_PACKET_MIN_LEN 9

/**
 * Offsets and sizes of various parts of the client packet. If the offset is
 * defined but not the size, the size of the value is one byte.
 */
#define MYSQL_SEQ_OFFSET        3
#define MYSQL_COM_OFFSET        4
#define MYSQL_CHARSET_OFFSET    12
#define MYSQL_CLIENT_CAP_OFFSET 4
#define MYSQL_CLIENT_CAP_SIZE   4
#define MARIADB_CAP_OFFSET      MYSQL_CHARSET_OFFSET + 19

#define GW_MYSQL_PROTOCOL_VERSION 10    // version is 10
#define GW_MYSQL_HANDSHAKE_FILLER 0x00
#define GW_MYSQL_SERVER_LANGUAGE  0x08
#define GW_MYSQL_MAX_PACKET_LEN   0xffffffL
#define GW_MYSQL_SCRAMBLE_SIZE    MYSQL_SCRAMBLE_LEN
#define GW_SCRAMBLE_LENGTH_323    8

/**
 * Prepared statement payload response offsets for a COM_STMT_PREPARE response:
 *
 * [0]     OK (1)            -- always 0x00
 * [1-4]   statement_id (4)  -- statement-id
 * [5-6]   num_columns (2)   -- number of columns
 * [7-8]   num_params (2)    -- number of parameters
 * [9]     filler
 * [10-11] warning_count (2) -- number of warnings
 */
#define MYSQL_PS_ID_OFFSET     MYSQL_HEADER_LEN + 1
#define MYSQL_PS_ID_SIZE       4
#define MYSQL_PS_COLS_OFFSET   MYSQL_HEADER_LEN + 5
#define MYSQL_PS_COLS_SIZE     2
#define MYSQL_PS_PARAMS_OFFSET MYSQL_HEADER_LEN + 7
#define MYSQL_PS_PARAMS_SIZE   2
#define MYSQL_PS_WARN_OFFSET   MYSQL_HEADER_LEN + 10
#define MYSQL_PS_WARN_SIZE     2

/** Name of the default server side authentication plugin */
#define DEFAULT_MYSQL_AUTH_PLUGIN "mysql_native_password"

/** All authentication responses are at least this many bytes long */
#define MYSQL_AUTH_PACKET_BASE_SIZE 36

/** Maximum length of a MySQL packet */
#define MYSQL_PACKET_LENGTH_MAX 0x00ffffff


/* Max length of fields in the mysql.user table */
#define MYSQL_PASSWORD_LEN 41
#define MYSQL_HOST_MAXLEN  60
#define MYSQL_TABLE_MAXLEN 64

#define GW_NOINTR_CALL(A) do {errno = 0; A;} while (errno == EINTR)
#define COM_QUIT_PACKET_SIZE (4 + 1)

/** Defines for response codes */
#define MYSQL_REPLY_ERR               0xff
#define MYSQL_REPLY_OK                0x00
#define MYSQL_REPLY_EOF               0xfe
#define MYSQL_REPLY_LOCAL_INFILE      0xfb
#define MYSQL_REPLY_AUTHSWITCHREQUEST 0xfe      /**< Only sent during authentication */

#define MYSQL_GET_ERRCODE(payload)       (gw_mysql_get_byte2(&payload[5]))
#define MYSQL_GET_STMTOK_NPARAM(payload) (gw_mysql_get_byte2(&payload[9]))
#define MYSQL_GET_STMTOK_NATTR(payload)  (gw_mysql_get_byte2(&payload[11]))
#define MYSQL_GET_NATTR(payload)         ((int)payload[4])

class DCB;
class BackendDCB;

/** Protocol packing macros. */
#define gw_mysql_set_byte2(__buffer, __int) \
    do { \
        (__buffer)[0] = (uint8_t)((__int) & 0xFF); \
        (__buffer)[1] = (uint8_t)(((__int) >> 8) & 0xFF);} while (0)
#define gw_mysql_set_byte3(__buffer, __int) \
    do { \
        (__buffer)[0] = (uint8_t)((__int) & 0xFF); \
        (__buffer)[1] = (uint8_t)(((__int) >> 8) & 0xFF); \
        (__buffer)[2] = (uint8_t)(((__int) >> 16) & 0xFF);} while (0)
#define gw_mysql_set_byte4(__buffer, __int) \
    do { \
        (__buffer)[0] = (uint8_t)((__int) & 0xFF); \
        (__buffer)[1] = (uint8_t)(((__int) >> 8) & 0xFF); \
        (__buffer)[2] = (uint8_t)(((__int) >> 16) & 0xFF); \
        (__buffer)[3] = (uint8_t)(((__int) >> 24) & 0xFF);} while (0)

/** Protocol unpacking macros. */
#define gw_mysql_get_byte2(__buffer) \
    (uint16_t)((__buffer)[0]   \
               | ((__buffer)[1] << 8))
#define gw_mysql_get_byte3(__buffer) \
    (uint32_t)((__buffer)[0]   \
               | ((__buffer)[1] << 8)   \
               | ((__buffer)[2] << 16))
#define gw_mysql_get_byte4(__buffer) \
    (uint32_t)((__buffer)[0]   \
               | ((__buffer)[1] << 8)   \
               | ((__buffer)[2] << 16)   \
               | ((__buffer)[3] << 24))
#define gw_mysql_get_byte8(__buffer) \
    ((uint64_t)(__buffer)[0]   \
     | ((uint64_t)(__buffer)[1] << 8)   \
     | ((uint64_t)(__buffer)[2] << 16)   \
     | ((uint64_t)(__buffer)[3] << 24)   \
     | ((uint64_t)(__buffer)[4] << 32)   \
     | ((uint64_t)(__buffer)[5] << 40)   \
     | ((uint64_t)(__buffer)[6] << 48)   \
     | ((uint64_t)(__buffer)[7] << 56))

namespace mariadb
{
/**
 * Protocol packing and unpacking functions. The functions read or write unsigned integers from/to
 * MySQL-protocol buffers. MySQL saves integers in lsb-first format, so a conversion to host format
 * may be required.
 */

void     set_byte2(uint8_t* buffer, uint16_t val);
void     set_byte3(uint8_t* buffer, uint32_t val);
void     set_byte4(uint8_t* buffer, uint32_t val);
void     set_byte8(uint8_t* buffer, uint64_t val);
uint16_t get_byte2(const uint8_t* buffer);
uint32_t get_byte3(const uint8_t* buffer);
uint32_t get_byte4(const uint8_t* buffer);
uint64_t get_byte8(const uint8_t* buffer);
}

/** MySQL protocol constants */
enum gw_mysql_capabilities_t
{
    GW_MYSQL_CAPABILITIES_NONE = 0,
    /** This is sent by pre-10.2 clients */
    GW_MYSQL_CAPABILITIES_CLIENT_MYSQL           = (1 << 0),
    GW_MYSQL_CAPABILITIES_FOUND_ROWS             = (1 << 1),
    GW_MYSQL_CAPABILITIES_LONG_FLAG              = (1 << 2),
    GW_MYSQL_CAPABILITIES_CONNECT_WITH_DB        = (1 << 3),
    GW_MYSQL_CAPABILITIES_NO_SCHEMA              = (1 << 4),
    GW_MYSQL_CAPABILITIES_COMPRESS               = (1 << 5),
    GW_MYSQL_CAPABILITIES_ODBC                   = (1 << 6),
    GW_MYSQL_CAPABILITIES_LOCAL_FILES            = (1 << 7),
    GW_MYSQL_CAPABILITIES_IGNORE_SPACE           = (1 << 8),
    GW_MYSQL_CAPABILITIES_PROTOCOL_41            = (1 << 9),
    GW_MYSQL_CAPABILITIES_INTERACTIVE            = (1 << 10),
    GW_MYSQL_CAPABILITIES_SSL                    = (1 << 11),
    GW_MYSQL_CAPABILITIES_IGNORE_SIGPIPE         = (1 << 12),
    GW_MYSQL_CAPABILITIES_TRANSACTIONS           = (1 << 13),
    GW_MYSQL_CAPABILITIES_RESERVED               = (1 << 14),
    GW_MYSQL_CAPABILITIES_SECURE_CONNECTION      = (1 << 15),
    GW_MYSQL_CAPABILITIES_MULTI_STATEMENTS       = (1 << 16),
    GW_MYSQL_CAPABILITIES_MULTI_RESULTS          = (1 << 17),
    GW_MYSQL_CAPABILITIES_PS_MULTI_RESULTS       = (1 << 18),
    GW_MYSQL_CAPABILITIES_PLUGIN_AUTH            = (1 << 19),
    GW_MYSQL_CAPABILITIES_CONNECT_ATTRS          = (1 << 20),
    GW_MYSQL_CAPABILITIES_AUTH_LENENC_DATA       = (1 << 21),
    GW_MYSQL_CAPABILITIES_EXPIRE_PASSWORD        = (1 << 22),
    GW_MYSQL_CAPABILITIES_SESSION_TRACK          = (1 << 23),
    GW_MYSQL_CAPABILITIES_DEPRECATE_EOF          = (1 << 24),
    GW_MYSQL_CAPABILITIES_SSL_VERIFY_SERVER_CERT = (1 << 30),
    GW_MYSQL_CAPABILITIES_REMEMBER_OPTIONS       = (1 << 31),
    GW_MYSQL_CAPABILITIES_CLIENT                 = (
        GW_MYSQL_CAPABILITIES_CLIENT_MYSQL
        | GW_MYSQL_CAPABILITIES_FOUND_ROWS
        | GW_MYSQL_CAPABILITIES_LONG_FLAG
        | GW_MYSQL_CAPABILITIES_CONNECT_WITH_DB
        | GW_MYSQL_CAPABILITIES_LOCAL_FILES
        | GW_MYSQL_CAPABILITIES_PLUGIN_AUTH
        | GW_MYSQL_CAPABILITIES_CONNECT_ATTRS
        | GW_MYSQL_CAPABILITIES_TRANSACTIONS
        | GW_MYSQL_CAPABILITIES_PROTOCOL_41
        | GW_MYSQL_CAPABILITIES_MULTI_STATEMENTS
        | GW_MYSQL_CAPABILITIES_MULTI_RESULTS
        | GW_MYSQL_CAPABILITIES_PS_MULTI_RESULTS
        | GW_MYSQL_CAPABILITIES_SECURE_CONNECTION),
    GW_MYSQL_CAPABILITIES_SERVER = (
        GW_MYSQL_CAPABILITIES_CLIENT_MYSQL
        | GW_MYSQL_CAPABILITIES_FOUND_ROWS
        | GW_MYSQL_CAPABILITIES_LONG_FLAG
        | GW_MYSQL_CAPABILITIES_CONNECT_WITH_DB
        | GW_MYSQL_CAPABILITIES_NO_SCHEMA
        | GW_MYSQL_CAPABILITIES_ODBC
        | GW_MYSQL_CAPABILITIES_LOCAL_FILES
        | GW_MYSQL_CAPABILITIES_IGNORE_SPACE
        | GW_MYSQL_CAPABILITIES_PROTOCOL_41
        | GW_MYSQL_CAPABILITIES_INTERACTIVE
        | GW_MYSQL_CAPABILITIES_IGNORE_SIGPIPE
        | GW_MYSQL_CAPABILITIES_TRANSACTIONS
        | GW_MYSQL_CAPABILITIES_RESERVED
        | GW_MYSQL_CAPABILITIES_SECURE_CONNECTION
        | GW_MYSQL_CAPABILITIES_MULTI_STATEMENTS
        | GW_MYSQL_CAPABILITIES_MULTI_RESULTS
        | GW_MYSQL_CAPABILITIES_PS_MULTI_RESULTS
        | GW_MYSQL_CAPABILITIES_PLUGIN_AUTH
        | GW_MYSQL_CAPABILITIES_CONNECT_ATTRS),
};

/**
 * Capabilities supported by MariaDB 10.2 and later, stored in the last 4 bytes
 * of the 10 byte filler of the initial handshake packet.
 *
 * The actual capability bytes use by the server are left shifted by an extra 32
 * bits to get one 64 bit capability that combines the old and new capabilities.
 * Since we only use these in the non-shifted form, the definitions declared here
 * are right shifted by 32 bytes and can be directly copied into the extra capabilities.
 */
#define MXS_MARIA_CAP_PROGRESS             (1 << 0)
#define MXS_MARIA_CAP_COM_MULTI            (1 << 1)
#define MXS_MARIA_CAP_STMT_BULK_OPERATIONS (1 << 2)

// Default extended flags that MaxScale supports
constexpr const uint32_t MXS_EXTRA_CAPABILITIES_SERVER = MXS_MARIA_CAP_STMT_BULK_OPERATIONS;

enum mxs_mysql_cmd_t
{
    MXS_COM_SLEEP = 0,
    MXS_COM_QUIT,
    MXS_COM_INIT_DB,
    MXS_COM_QUERY,
    MXS_COM_FIELD_LIST,
    MXS_COM_CREATE_DB,
    MXS_COM_DROP_DB,
    MXS_COM_REFRESH,
    MXS_COM_SHUTDOWN,
    MXS_COM_STATISTICS,
    MXS_COM_PROCESS_INFO,
    MXS_COM_CONNECT,
    MXS_COM_PROCESS_KILL,
    MXS_COM_DEBUG,
    MXS_COM_PING,
    MXS_COM_TIME = 15,
    MXS_COM_DELAYED_INSERT,
    MXS_COM_CHANGE_USER,
    MXS_COM_BINLOG_DUMP,
    MXS_COM_TABLE_DUMP,
    MXS_COM_CONNECT_OUT = 20,
    MXS_COM_REGISTER_SLAVE,
    MXS_COM_STMT_PREPARE        = 22,
    MXS_COM_STMT_EXECUTE        = 23,
    MXS_COM_STMT_SEND_LONG_DATA = 24,
    MXS_COM_STMT_CLOSE          = 25,
    MXS_COM_STMT_RESET          = 26,
    MXS_COM_SET_OPTION          = 27,
    MXS_COM_STMT_FETCH          = 28,
    MXS_COM_DAEMON              = 29,
    MXS_COM_UNSUPPORTED         = 30,
    MXS_COM_RESET_CONNECTION    = 31,
    MXS_COM_STMT_BULK_EXECUTE   = 0xfa,
    MXS_COM_MULTI               = 0xfe,
    MXS_COM_END,
    MXS_COM_UNDEFINED = -1
};

/**
 * A GWBUF property with this name will contain the latest GTID in string form.
 * This information is only available in OK packets.
 */
static const char* const MXS_LAST_GTID = "last_gtid";

struct MXS_PS_RESPONSE
{
    uint32_t id;
    uint16_t columns;
    uint16_t parameters;
    uint16_t warnings;
};

static inline mxs_mysql_cmd_t MYSQL_GET_COMMAND(const uint8_t* header)
{
    return (mxs_mysql_cmd_t)header[4];
}

static inline uint8_t MYSQL_GET_PACKET_NO(const uint8_t* header)
{
    return header[3];
}

static inline uint32_t MYSQL_GET_PAYLOAD_LEN(const uint8_t* header)
{
    return gw_mysql_get_byte3(header);
}

static inline uint32_t MYSQL_GET_PACKET_LEN(const GWBUF* buffer)
{
    mxb_assert(buffer);
    return MYSQL_GET_PAYLOAD_LEN(GWBUF_DATA(buffer)) + MYSQL_HEADER_LEN;
}

static inline bool MYSQL_IS_ERROR_PACKET(const uint8_t* header)
{
    return MYSQL_GET_COMMAND(header) == MYSQL_REPLY_ERR;
}

static inline bool MYSQL_IS_COM_QUIT(const uint8_t* header)
{
    return MYSQL_GET_COMMAND(header) == MXS_COM_QUIT
           && MYSQL_GET_PAYLOAD_LEN(header) == 1;
}

static inline bool MYSQL_IS_COM_INIT_DB(const uint8_t* header)
{
    return MYSQL_GET_COMMAND(header) == MXS_COM_INIT_DB;
}

static inline bool MYSQL_IS_CHANGE_USER(const uint8_t* header)
{
    return MYSQL_GET_COMMAND(header) == MXS_COM_CHANGE_USER;
}

/* The following can be compared using memcmp to detect a null password */
extern uint8_t null_client_sha1[MYSQL_SCRAMBLE_LEN];

GWBUF* mysql_create_com_quit(GWBUF* bufparam, int sequence);
GWBUF* mysql_create_custom_error(int sequence, int affected_rows, uint16_t errnum, const char* errmsg);
GWBUF* mxs_mysql_create_ok(int sequence, uint8_t affected_rows, const char* message);

void init_response_status(GWBUF* buf, uint8_t cmd, int* npackets, size_t* nbytes);

/**
 * @brief Check if the buffer contains an OK packet
 *
 * @param buffer Buffer containing a complete MySQL packet
 * @return True if the buffer contains an OK packet
 */
bool mxs_mysql_is_ok_packet(GWBUF* buffer);

/**
 * @brief Check if the buffer contains an ERR packet
 *
 * @param buffer Buffer containing a complete MySQL packet
 * @return True if the buffer contains an ERR packet
 */
bool mxs_mysql_is_err_packet(GWBUF* buffer);

/**
 * Extract the error code from an ERR packet
 *
 * @param buffer Buffer containing the ERR packet
 *
 * @return The error code or 0 if the buffer is not an ERR packet
 */
uint16_t mxs_mysql_get_mysql_errno(GWBUF* buffer);

/**
 * @brief Check if the buffer contains a LOCAL INFILE request
 *
 * @param buffer Buffer containing a complete MySQL packet
 *
 * @return True if the buffer contains a LOCAL INFILE request
 */
bool mxs_mysql_is_local_infile(GWBUF* buffer);

/**
 * @brief Check if the buffer contains a prepared statement OK packet
 *
 * @param buffer Buffer to check
 *
 * @return True if the @c buffer contains a prepared statement OK packet
 */
bool mxs_mysql_is_prep_stmt_ok(GWBUF* buffer);

/**
 * Is this a binary protocol command
 *
 * @param cmd Command to check
 *
 * @return True if the command is a binary protocol command
 */
bool mxs_mysql_is_ps_command(uint8_t cmd);

/**
 * @brief Check if the OK packet is followed by another result
 *
 * @param buffer Buffer to check
 *
 * @return True if more results are expected
 */
bool mxs_mysql_more_results_after_ok(GWBUF* buffer);

/** Get current command for a session */
mxs_mysql_cmd_t mxs_mysql_current_command(MXS_SESSION* session);
/**
 * @brief Calculate how many packets a session command will receive
 *
 * @param buf Buffer containing the response
 * @param cmd Command that was executed
 * @param npackets Pointer where the number of packets is stored
 * @param nbytes Pointer where number of bytes is stored
 */
void mysql_num_response_packets(GWBUF* buf,
                                uint8_t cmd,
                                int* npackets,
                                size_t* nbytes);

/**
 * @brief Get the command byte
 *
 * @param buffer Buffer containing a complete MySQL packet
 *
 * @return The command byte
 */
static inline uint8_t mxs_mysql_get_command(GWBUF* buffer)
{
    mxb_assert(buffer);
    if (GWBUF_LENGTH(buffer) > MYSQL_HEADER_LEN)
    {
        return GWBUF_DATA(buffer)[4];
    }
    else
    {
        uint8_t command = 0;
        gwbuf_copy_data(buffer, MYSQL_HEADER_LEN, 1, &command);
        return command;
    }
}

/**
 * @brief Get the total size of the first packet
 *
 * The size includes the payload and the header
 *
 * @param buffer Buffer to inspect
 *
 * @return The total packet size in bytes
 */
static inline uint32_t mxs_mysql_get_packet_len(GWBUF* buffer)
{
    mxb_assert(buffer);
    // The first three bytes of the packet header contain its length
    uint8_t buf[3];
    gwbuf_copy_data(buffer, 0, 3, buf);
    return gw_mysql_get_byte3(buf) + MYSQL_HEADER_LEN;
}

/**
 * @brief Extract PS response values
 *
 * @param buffer Buffer containing a complete response to a binary protocol
 *               preparation of a prepared statement
 * @param out    Destination where the values are extracted
 *
 * @return True if values were extracted successfully
 */
bool mxs_mysql_extract_ps_response(GWBUF* buffer, MXS_PS_RESPONSE* out);

/**
 * @brief Extract the ID from a COM_STMT command
 *
 * All the COM_STMT type commands store the statement ID in the same place.
 *
 * @param buffer Buffer containing one of the COM_STMT commands (not COM_STMT_PREPARE)
 *
 * @return The statement ID
 */
uint32_t mxs_mysql_extract_ps_id(GWBUF* buffer);

/**
 * @brief Determine if a packet contains a one way message
 *
 * @param cmd Command to inspect
 *
 * @return True if a response is expected from the server
 */
bool mxs_mysql_command_will_respond(uint8_t cmd);

/**
 * Calculates the a hash from a scramble and a password
 *
 * The algorithm used is: `SHA1(scramble + SHA1(SHA1(password))) ^ SHA1(password)`
 *
 * @param scramble The 20 byte scramble sent by the server
 * @param passwd   The SHA1(password) sent by the client
 * @param output   Pointer where the resulting 20 byte hash is stored
 */
void mxs_mysql_calculate_hash(const uint8_t* scramble, const uint8_t* passwd, uint8_t* output);

int response_length(bool with_ssl, bool ssl_established, const char* user, const uint8_t* passwd,
                    const char* dbname, const char* auth_module);

uint8_t* load_hashed_password(const uint8_t* scramble, uint8_t* payload, const uint8_t* passwd);

bool read_protocol_packet(DCB* dcb, mxs::Buffer* output);
