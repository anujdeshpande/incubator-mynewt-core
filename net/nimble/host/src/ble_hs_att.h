/**
 * Copyright (c) 2015 Runtime Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef H_BLE_HS_ATT_
#define H_BLE_HS_ATT_

#include "host/uuid.h"

#define BLE_HS_ATT_MTU_DFLT         23

struct ble_hs_att_entry;

union ble_hs_att_handle_arg {
    struct {
        void *attr_data;
        int attr_len;
    } aha_read;

    struct {
        struct os_mbuf *om;
        int attr_len;
    } aha_write;
};

/**
 * Called from ble_hs_att_walk().  Called on each entry in the 
 * ble_hs_att_list.
 *
 * @param Contains the current ble_hs_att being iterated through
 * @param The user supplied argument to ble_hs_att_walk()
 *
 * @return 0 on continue, 1 on stop
 */
typedef int (*ble_hs_att_walk_func_t)(struct ble_hs_att_entry *, void *arg);

/**
 * Handles a host attribute request.
 *
 * @param entry The host attribute being requested
 * @param The request data associated with that host attribute
 */
typedef int ble_hs_att_handle_func(struct ble_hs_att_entry *entry,
                                   uint8_t op,
                                   union ble_hs_att_handle_arg *arg);

#define HA_FLAG_PERM_READ            (1 << 0)
#define HA_FLAG_PERM_WRITE           (1 << 1) 
#define HA_FLAG_PERM_RW              (1 << 2)
#define HA_FLAG_ENC_REQ              (1 << 3)
#define HA_FLAG_AUTHENTICATION_REQ   (1 << 4)
#define HA_FLAG_AUTHORIZATION_REQ    (1 << 5)

struct ble_hs_att_entry {
    STAILQ_ENTRY(ble_hs_att_entry) ha_next;

    ble_uuid_t ha_uuid;
    uint8_t ha_flags;
    uint8_t ha_pad1;
    uint16_t ha_handle_id;
    ble_hs_att_handle_func *ha_fn;
};

#define HA_OPCODE_METHOD_START (0)
#define HA_OPCODE_METHOD_END (5)
#define HA_OPCODE_COMMAND_FLAG (1 << 6) 
#define HA_OPCODE_AUTH_SIG_FLAG (1 << 7) 

int ble_hs_att_register(uint8_t *uuid, uint8_t flags, uint16_t *handle_id,
                        ble_hs_att_handle_func *fn);
struct ble_l2cap_chan *ble_hs_att_create_chan(void);
int ble_hs_att_init(void);

#endif