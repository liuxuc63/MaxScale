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

#include <maxscale/ccdefs.hh>

#include <algorithm>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <unordered_map>

#include <maxscale/adminusers.h>
#include <maxbase/pam_utils.hh>
#include <maxscale/authenticator.hh>
#include <maxscale/event.hh>
#include <maxscale/jansson.hh>
#include <maxscale/users.h>

namespace
{

static const char STR_BASIC[] = "basic";
static const char STR_ADMIN[] = "admin";

// Generates SHA2-512 hashes
constexpr const char* ADMIN_SALT = "$6$MXS";

// Generates MD5 hashes, only used for authentication of old users
constexpr const char* OLD_ADMIN_SALT = "$1$MXS";

struct UserInfo
{
    UserInfo()
        : permissions(USER_ACCOUNT_BASIC)
    {
    }

    UserInfo(std::string pw, user_account_type perm)
        : password(pw)
        , permissions(perm)
    {
    }

    std::string       password;
    user_account_type permissions;
};


class Users
{
    Users(const Users&);
    Users& operator=(const Users&);

public:
    typedef std::unordered_map<std::string, UserInfo> UserMap;

    Users()
    {
    }

    ~Users()
    {
    }

    bool add(const std::string& user, const std::string& password, user_account_type perm)
    {
        return add_hashed(user, hash(password), perm);
    }

    bool remove(std::string user)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        bool rval = false;
        UserMap::iterator it = m_data.find(user);

        if (it != m_data.end())
        {
            m_data.erase(it);
            rval = true;
        }

        return rval;
    }

    bool get(std::string user, UserInfo* output = NULL) const
    {
        std::lock_guard<std::mutex> guard(m_lock);
        UserMap::const_iterator it = m_data.find(user);
        bool rval = false;

        if (it != m_data.end())
        {
            rval = true;

            if (output)
            {
                *output = it->second;
            }
        }

        return rval;
    }

    bool authenticate(const std::string& user, const std::string& password)
    {
        bool rval = false;
        UserInfo info;

        if (get(user, &info))
        {
            // The second character tell us which hashing function to use
            auto crypted = info.password[1] == ADMIN_SALT[1] ? hash(password) : old_hash(password);
            rval = info.password == crypted;
        }

        return rval;
    }

    int admin_count() const
    {
        return std::count_if(m_data.begin(), m_data.end(), is_admin);
    }

    bool check_permissions(const std::string& user,
                           const std::string& password,
                           user_account_type perm) const
    {
        std::lock_guard<std::mutex> guard(m_lock);
        UserMap::const_iterator it = m_data.find(user);
        bool rval = false;

        if (it != m_data.end() && it->second.permissions == perm)
        {
            rval = true;
        }

        return rval;
    }

    bool set_permissions(std::string user, user_account_type perm)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        UserMap::iterator it = m_data.find(user);
        bool rval = false;

        if (it != m_data.end())
        {
            rval = true;
            it->second.permissions = perm;
        }

        return rval;
    }

    json_t* diagnostic_json() const
    {
        std::lock_guard<std::mutex> guard(m_lock);
        json_t* rval = json_array();

        for (UserMap::const_iterator it = m_data.begin(); it != m_data.end(); it++)
        {
            json_t* obj = json_object();
            json_object_set_new(obj, CN_NAME, json_string(it->first.c_str()));
            json_object_set_new(obj, CN_ACCOUNT, json_string(account_type_to_str(it->second.permissions)));
            json_array_append_new(rval, obj);
        }

        return rval;
    }

    void diagnostic(DCB* dcb) const
    {
        std::lock_guard<std::mutex> guard(m_lock);
        if (m_data.size())
        {
            const char* sep = "";
            std::set<std::string> users;

            for (UserMap::const_iterator it = m_data.begin(); it != m_data.end(); it++)
            {
                users.insert(it->first);
            }

            for (const auto& a : users)
            {
                dcb_printf(dcb, "%s%s", sep, a.c_str());
                sep = ", ";
            }
        }
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> guard(m_lock);
        return m_data.size() > 0;
    }

    json_t* to_json() const
    {
        json_t* arr = json_array();
        std::lock_guard<std::mutex> guard(m_lock);

        for (UserMap::const_iterator it = m_data.begin(); it != m_data.end(); it++)
        {
            json_t* obj = json_object();
            json_object_set_new(obj, CN_NAME, json_string(it->first.c_str()));
            json_object_set_new(obj, CN_ACCOUNT, json_string(account_type_to_str(it->second.permissions)));
            json_object_set_new(obj, CN_PASSWORD, json_string(it->second.password.c_str()));
            json_array_append_new(arr, obj);
        }

        return arr;
    }

    static Users* from_json(json_t* json)
    {
        Users* u = reinterpret_cast<Users*>(users_alloc());
        u->load_json(json);
        return u;
    }

private:
    bool add_hashed(const std::string& user, const std::string& password, user_account_type perm)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        return m_data.insert(std::make_pair(user, UserInfo(password, perm))).second;
    }

    static bool is_admin(const UserMap::value_type& value)
    {
        return value.second.permissions == USER_ACCOUNT_ADMIN;
    }

    void load_json(json_t* json)
    {
        // This function is always called in a single-threaded context
        size_t i;
        json_t* value;

        json_array_foreach(json, i, value)
        {
            json_t* name = json_object_get(value, CN_NAME);
            json_t* type = json_object_get(value, CN_ACCOUNT);
            json_t* password = json_object_get(value, CN_PASSWORD);

            if (name && json_is_string(name)
                && type && json_is_string(type)
                && password && json_is_string(password)
                && json_to_account_type(type) != USER_ACCOUNT_UNKNOWN)
            {
                add_hashed(json_string_value(name),
                           json_string_value(password),
                           json_to_account_type(type));
            }
            else
            {
                MXS_ERROR("Corrupt JSON value in users file: %s", mxs::json_dump(value).c_str());
            }
        }
    }

    std::string hash(const std::string& password)
    {
        const int CACHE_MAX_SIZE = 1000;
        static std::unordered_map<std::string, std::string> hash_cache;
        auto it = hash_cache.find(password);

        if (it != hash_cache.end())
        {
            return it->second;
        }
        else
        {
            if (hash_cache.size() > CACHE_MAX_SIZE)
            {
                auto bucket = rand() % hash_cache.bucket_count();
                mxb_assert(bucket < hash_cache.bucket_count());
                hash_cache.erase(hash_cache.cbegin(bucket)->first);
            }

            auto new_hash = mxs::crypt(password, ADMIN_SALT);
            hash_cache.insert(std::make_pair(password, new_hash));
            return new_hash;
        }
    }

    std::string old_hash(const std::string& password)
    {
        return mxs::crypt(password, OLD_ADMIN_SALT);
    }

    mutable std::mutex m_lock;
    UserMap            m_data;
};
}

USERS* users_alloc()
{
    Users* rval = new(std::nothrow) Users();
    MXS_OOM_IFNULL(rval);
    return reinterpret_cast<USERS*>(rval);
}

void users_free(USERS* users)
{
    Users* u = reinterpret_cast<Users*>(users);
    delete u;
}

bool users_add(USERS* users, const char* user, const char* password, enum user_account_type type)
{
    Users* u = reinterpret_cast<Users*>(users);
    return u->add(user, password, type);
}

bool users_delete(USERS* users, const char* user)
{
    Users* u = reinterpret_cast<Users*>(users);
    return u->remove(user);
}

json_t* users_to_json(USERS* users)
{
    Users* u = reinterpret_cast<Users*>(users);
    return u->to_json();
}

USERS* users_from_json(json_t* json)
{
    return reinterpret_cast<USERS*>(Users::from_json(json));
}

bool users_find(USERS* users, const char* user)
{
    Users* u = reinterpret_cast<Users*>(users);
    return u->get(user);
}

bool users_change_password(USERS* users, const char* user, const char* password)
{
    Users* u = reinterpret_cast<Users*>(users);
    UserInfo info;
    return u->get(user, &info) && u->remove(user) && u->add(user, password, info.permissions);
}

bool users_auth(USERS* users, const char* user, const char* password)
{
    Users* u = reinterpret_cast<Users*>(users);
    return u->authenticate(user, password);
}

bool users_is_admin(USERS* users, const char* user, const char* password)
{
    Users* u = reinterpret_cast<Users*>(users);
    return u->check_permissions(user, password ? password : "", USER_ACCOUNT_ADMIN);
}

int users_admin_count(USERS* users)
{
    Users* u = reinterpret_cast<Users*>(users);
    return u->admin_count();
}

void users_diagnostic(DCB* dcb, USERS* users)
{
    Users* u = reinterpret_cast<Users*>(users);
    u->diagnostic(dcb);
}

json_t* users_diagnostic_json(USERS* users)
{
    Users* u = reinterpret_cast<Users*>(users);
    return u->diagnostic_json();
}

void users_default_diagnostic(DCB* dcb, Listener* port)
{
    if (port->users())
    {
        users_diagnostic(dcb, port->users());
    }
}

json_t* users_default_diagnostic_json(const Listener* port)
{
    return port->users() ? users_diagnostic_json(port->users()) : json_array();
}

int users_default_loadusers(Listener* port)
{
    users_free(port->users());
    port->set_users(users_alloc());
    return MXS_AUTH_LOADUSERS_OK;
}

const char* account_type_to_str(enum user_account_type type)
{
    switch (type)
    {
    case USER_ACCOUNT_BASIC:
        return STR_BASIC;

    case USER_ACCOUNT_ADMIN:
        return STR_ADMIN;

    default:
        return "unknown";
    }
}

enum user_account_type json_to_account_type(json_t* json)
{
    std::string str = json_string_value(json);

    if (str == STR_BASIC)
    {
        return USER_ACCOUNT_BASIC;
    }
    else if (str == STR_ADMIN)
    {
        return USER_ACCOUNT_ADMIN;
    }

    return USER_ACCOUNT_UNKNOWN;
}
