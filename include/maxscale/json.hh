/*
 * Copyright (c) 2020 MariaDB Corporation Ab
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

#include <maxscale/jansson.hh>

/**
 * @brief Return value at provided JSON Pointer
 *
 * @param json     JSON object
 * @param json_ptr JSON Pointer to object
 *
 * @return Pointed value or NULL if no value is found
 */
json_t* mxs_json_pointer(json_t* json, const char* json_ptr);

/**
 * @brief Check if the value at the provided JSON Pointer is of a certain type
 *
 * @param json     JSON object
 * @param json_ptr JSON Pointer to object
 * @param type     JSON type that is expected
 *
 * @return False if the object was found but it was not of the expected type. True in all other cases.
 */
bool mxs_json_is_type(json_t* json, const char* json_ptr, json_type type);
