/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <string.h>
#include <errno.h>
#include "bsp/bsp.h"
#include "os/os.h"
#include "nimble/ble.h"
#include "host/ble_uuid.h"
#include "ble_hs_priv.h"

static STAILQ_HEAD(, ble_att_svr_entry) ble_att_svr_list;
static uint16_t ble_att_svr_id;

static void *ble_att_svr_entry_mem;
static struct os_mempool ble_att_svr_entry_pool;

static void *ble_att_svr_prep_entry_mem;
static struct os_mempool ble_att_svr_prep_entry_pool;

ble_att_svr_notify_fn *ble_att_svr_notify_cb;
void *ble_att_svr_notify_cb_arg;

static struct ble_att_svr_entry *
ble_att_svr_entry_alloc(void)
{
    struct ble_att_svr_entry *entry;

    entry = os_memblock_get(&ble_att_svr_entry_pool);
    if (entry != NULL) {
        memset(entry, 0, sizeof *entry);
    }

    return entry;
}

/**
 * Allocate the next handle id and return it.
 *
 * @return A new 16-bit handle ID.
 */
static uint16_t
ble_att_svr_next_id(void)
{
    /* Rollover is fatal. */
    BLE_HS_DBG_ASSERT(ble_att_svr_id != UINT16_MAX);
    return ++ble_att_svr_id;
}

/**
 * Register a host attribute with the BLE stack.
 *
 * @param ha                    A filled out ble_att structure to register
 * @param handle_id             A pointer to a 16-bit handle ID, which will be
 *                                  the handle that is allocated.
 * @param fn                    The callback function that gets executed when
 *                                  the attribute is operated on.
 *
 * @return 0 on success, non-zero error code on failure.
 */
int
ble_att_svr_register(uint8_t *uuid, uint8_t flags, uint16_t *handle_id,
                     ble_att_svr_access_fn *cb, void *cb_arg)
{
    struct ble_att_svr_entry *entry;

    entry = ble_att_svr_entry_alloc();
    if (entry == NULL) {
        return BLE_HS_ENOMEM;
    }

    memcpy(&entry->ha_uuid, uuid, sizeof entry->ha_uuid);
    entry->ha_flags = flags;
    entry->ha_handle_id = ble_att_svr_next_id();
    entry->ha_cb = cb;
    entry->ha_cb_arg = cb_arg;

    STAILQ_INSERT_TAIL(&ble_att_svr_list, entry, ha_next);

    if (handle_id != NULL) {
        *handle_id = entry->ha_handle_id;
    }

    return 0;
}

int
ble_att_svr_register_uuid16(uint16_t uuid16, uint8_t flags,
                            uint16_t *handle_id, ble_att_svr_access_fn *cb,
                            void *cb_arg)
{
    uint8_t uuid128[16];
    int rc;

    rc = ble_uuid_16_to_128(uuid16, uuid128);
    if (rc != 0) {
        return rc;
    }

    rc = ble_att_svr_register(uuid128, flags, handle_id, cb, cb_arg);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

uint16_t
ble_att_svr_prev_handle(void)
{
    return ble_att_svr_id;
}

/**
 * Find a host attribute by handle id.
 *
 * @param handle_id             The handle_id to search for
 * @param ha_ptr                On input: Indicates the starting point of the
 *                                  walk; null means start at the beginning of
 *                                  the list, non-null means start at the
 *                                  following entry.
 *                              On output: Indicates the last ble_att element
 *                                  processed, or NULL if the entire list has
 *                                  been processed.
 *
 * @return                      0 on success; BLE_HS_ENOENT on not found.
 */
struct ble_att_svr_entry *
ble_att_svr_find_by_handle(uint16_t handle_id)
{
    struct ble_att_svr_entry *entry;

    for (entry = STAILQ_FIRST(&ble_att_svr_list);
         entry != NULL;
         entry = STAILQ_NEXT(entry, ha_next)) {

        if (entry->ha_handle_id == handle_id) {
            return entry;
        }
    }

    return NULL;
}

/**
 * Find a host attribute by UUID.
 *
 * @param uuid                  The ble_uuid_t to search for
 * @param ha_ptr                On input: Indicates the starting point of the
 *                                  walk; null means start at the beginning of
 *                                  the list, non-null means start at the
 *                                  following entry.
 *                              On output: Indicates the last ble_att element
 *                                  processed, or NULL if the entire list has
 *                                  been processed.
 *
 * @return                      0 on success; BLE_HS_ENOENT on not found.
 */
struct ble_att_svr_entry *
ble_att_svr_find_by_uuid(struct ble_att_svr_entry *prev, uint8_t *uuid)
{
    struct ble_att_svr_entry *entry;

    if (prev == NULL) {
        entry = STAILQ_FIRST(&ble_att_svr_list);
    } else {
        entry = STAILQ_NEXT(prev, ha_next);
    }

    for (; entry != NULL; entry = STAILQ_NEXT(entry, ha_next)) {
        if (memcmp(entry->ha_uuid, uuid, sizeof entry->ha_uuid) == 0) {
            return entry;
        }
    }

    return NULL;
}

static int
ble_att_svr_pullup_req_base(struct os_mbuf **om, int base_len,
                            uint8_t *out_att_err)
{
    uint8_t att_err;
    int rc;

    rc = ble_hs_misc_pullup_base(om, base_len);
    if (rc == BLE_HS_ENOMEM) {
        att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
    } else {
        att_err = 0;
    }

    if (out_att_err != NULL) {
        *out_att_err = att_err;
    }

    return rc;
}

static int
ble_att_svr_read(uint16_t conn_handle, struct ble_att_svr_entry *entry,
                 struct ble_att_svr_access_ctxt *ctxt, uint8_t *out_att_err)
{
    uint8_t att_err;
    int rc;

    if (conn_handle != BLE_HS_CONN_HANDLE_NONE &&
        !(entry->ha_flags & HA_FLAG_PERM_READ)) {

        att_err = BLE_ATT_ERR_READ_NOT_PERMITTED;
        rc = BLE_HS_ENOTSUP;
        goto err;
    }

    if (entry->ha_cb == NULL) {
        att_err = BLE_ATT_ERR_UNLIKELY;
        rc = BLE_HS_ENOTSUP;
        goto err;
    }

    /* XXX: Check security. */

    BLE_HS_DBG_ASSERT(entry->ha_cb != NULL);
    rc = entry->ha_cb(conn_handle, entry->ha_handle_id,
                      entry->ha_uuid, BLE_ATT_ACCESS_OP_READ, ctxt,
                      entry->ha_cb_arg);
    if (rc != 0) {
        att_err = rc;
        rc = BLE_HS_EAPP;
        goto err;
    }

    return 0;

err:
    if (out_att_err != NULL) {
        *out_att_err = att_err;
    }
    return rc;
}

int
ble_att_svr_read_handle(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_att_svr_access_ctxt *ctxt,
                        uint8_t *out_att_err)
{
    struct ble_att_svr_entry *entry;
    int rc;

    entry = ble_att_svr_find_by_handle(attr_handle);
    if (entry == NULL) {
        *out_att_err = BLE_ATT_ERR_INVALID_HANDLE;
        return BLE_HS_ENOENT;
    }

    rc = ble_att_svr_read(conn_handle, entry, ctxt, out_att_err);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static int
ble_att_svr_write(uint16_t conn_handle, struct ble_att_svr_entry *entry,
                  struct ble_att_svr_access_ctxt *ctxt, uint8_t *out_att_err)
{
    uint8_t att_err;
    int rc;

    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());

    if (conn_handle != BLE_HS_CONN_HANDLE_NONE &&
        !(entry->ha_flags & HA_FLAG_PERM_WRITE)) {

        att_err = BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        rc = BLE_HS_ENOTSUP;
        goto err;
    }

    /* XXX: Check security. */

    BLE_HS_DBG_ASSERT(entry->ha_cb != NULL);
    rc = entry->ha_cb(conn_handle, entry->ha_handle_id,
                      entry->ha_uuid, BLE_ATT_ACCESS_OP_WRITE, ctxt,
                      entry->ha_cb_arg);
    if (rc != 0) {
        att_err = rc;
        rc = BLE_HS_EAPP;
        goto err;
    }

    return 0;

err:
    if (out_att_err != NULL) {
        *out_att_err = att_err;
    }
    return rc;
}

static int
ble_att_svr_write_handle(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_att_svr_access_ctxt *ctxt,
                         uint8_t *out_att_err)
{
    struct ble_att_svr_entry *entry;
    int rc;

    entry = ble_att_svr_find_by_handle(attr_handle);
    if (entry == NULL) {
        *out_att_err = BLE_ATT_ERR_INVALID_HANDLE;
        return BLE_HS_ENOENT;
    }

    rc = ble_att_svr_write(conn_handle, entry, ctxt, out_att_err);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

static int
ble_att_svr_tx_error_rsp(struct ble_hs_conn *conn, struct ble_l2cap_chan *chan,
                         uint8_t req_op, uint16_t handle, uint8_t error_code)
{
    struct ble_att_error_rsp rsp;
    struct os_mbuf *txom;
    void *dst;
    int rc;

    BLE_HS_DBG_ASSERT(error_code != 0);

    txom = ble_hs_misc_pkthdr();
    if (txom == NULL) {
        rc = BLE_HS_ENOMEM;
        goto err;
    }

    dst = os_mbuf_extend(txom, BLE_ATT_ERROR_RSP_SZ);
    if (dst == NULL) {
        rc = BLE_HS_ENOMEM;
        goto err;
    }

    rsp.baep_req_op = req_op;
    rsp.baep_handle = handle;
    rsp.baep_error_code = error_code;

    ble_att_error_rsp_write(dst, BLE_ATT_ERROR_RSP_SZ, &rsp);

    rc = ble_l2cap_tx(conn, chan, txom);
    txom = NULL;
    if (rc != 0) {
        goto err;
    }

    return 0;

err:
    os_mbuf_free_chain(txom);
    return rc;
}

/**
 * Transmits a response or error message over the specified connection.
 *
 * The specified rc value controls what gets sent as follows:
 *     o If rc == 0: tx an affirmative response.
 *     o If rc == BLE_HS_ENOTCONN: tx nothing.
 *     o Else: tx an error response.
 *
 * In addition, if transmission of an affirmative response fails, an error is
 * sent instead.
 *
 * @param conn_handle           The handle of the connection to send over.
 * @param rc                    The status indicating whether to transmit an
 *                                  affirmative response or an error.
 * @param txom                  Contains the affirmative response payload.
 * @param att_op                If an error is transmitted, this is the value
 *                                  of the error message's op field.
 * @param err_status            If an error is transmitted, this is the value
 *                                  of the error message's status field.
 * @param err_handle            If an error is transmitted, this is the value
 *                                  of the error message's attribute handle
 *                                  field.
 */
static int
ble_att_svr_tx_rsp(uint16_t conn_handle, int rc, struct os_mbuf *txom,
                   uint8_t att_op, uint8_t err_status, uint16_t err_handle)
{
    struct ble_l2cap_chan *chan;
    struct ble_hs_conn *conn;
    int do_tx;

    if (rc == BLE_HS_ENOTCONN) {
        /* No connection; tx is not possible. */
        do_tx = 0;
    } else if (rc != 0 && err_status == 0) {
        /* Processing failed, but err_status of 0 means don't send error. */
        do_tx = 0;
    } else {
        do_tx = 1;
    }

    if (do_tx) {
        ble_hs_lock();

        ble_att_conn_chan_find(conn_handle, &conn, &chan);
        if (chan == NULL) {
            rc = BLE_HS_ENOTCONN;
        } else {
            if (rc == 0) {
                BLE_HS_DBG_ASSERT(txom != NULL);
                ble_att_inc_tx_stat(txom->om_data[0]);
                rc = ble_l2cap_tx(conn, chan, txom);
                txom = NULL;
                if (rc != 0) {
                    err_status = BLE_ATT_ERR_UNLIKELY;
                }
            }

            if (rc != 0) {
                STATS_INC(ble_att_stats, error_rsp_tx);
                ble_att_svr_tx_error_rsp(conn, chan, att_op,
                                         err_handle, err_status);
            }
        }

        ble_hs_unlock();
    }

    os_mbuf_free_chain(txom);

    return rc;
}

static int
ble_att_svr_build_mtu_rsp(uint16_t conn_handle, struct os_mbuf **out_txom,
                          uint8_t *att_err)
{
    struct ble_att_mtu_cmd cmd;
    struct ble_l2cap_chan *chan;
    struct os_mbuf *txom;
    uint16_t mtu;
    void *dst;
    int rc;

    *att_err = 0; /* Silence unnecessary warning. */
    txom = NULL;

    ble_hs_lock();
    rc = ble_att_conn_chan_find(conn_handle, NULL, &chan);
    if (rc == 0) {
        mtu = chan->blc_my_mtu;
    }
    ble_hs_unlock();

    if (rc != 0) {
        goto done;
    }

    txom = ble_hs_misc_pkthdr();
    if (txom == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    dst = os_mbuf_extend(txom, BLE_ATT_MTU_CMD_SZ);
    if (dst == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    cmd.bamc_mtu = mtu;

    ble_att_mtu_rsp_write(dst, BLE_ATT_MTU_CMD_SZ, &cmd);

    rc = 0;

done:
    *out_txom = txom;
    return rc;
}

int
ble_att_svr_rx_mtu(uint16_t conn_handle, struct os_mbuf **om)
{
    struct ble_att_mtu_cmd cmd;
    struct ble_l2cap_chan *chan;
    struct ble_hs_conn *conn;
    struct os_mbuf *txom;
    uint8_t att_err;
    int rc;

    txom = NULL;

    rc = ble_att_svr_pullup_req_base(om, BLE_ATT_MTU_CMD_SZ, &att_err);
    if (rc != 0) {
        goto done;
    }

    ble_att_mtu_cmd_parse((*om)->om_data, (*om)->om_len, &cmd);

    rc = ble_att_svr_build_mtu_rsp(conn_handle, &txom, &att_err);
    if (rc != 0) {
        goto done;
    }

    rc = 0;

done:
    rc = ble_att_svr_tx_rsp(conn_handle, rc, txom, BLE_ATT_OP_FIND_INFO_REQ,
                            att_err, 0);
    if (rc == 0) {
        ble_hs_lock();
        ble_att_conn_chan_find(conn_handle, &conn, &chan);
        if (chan != NULL) {
            ble_att_set_peer_mtu(chan, cmd.bamc_mtu);
            chan->blc_flags |= BLE_L2CAP_CHAN_F_TXED_MTU;
        }
        ble_hs_unlock();
    }
    return rc;
}

/**
 * Fills the supplied mbuf with the variable length Information Data field of a
 * Find Information ATT response.
 *
 * @param req                   The Find Information request being responded
 *                                  to.
 * @param om                    The destination mbuf where the Information
 *                                  Data field gets written.
 * @param mtu                   The ATT L2CAP channel MTU.
 * @param format                On success, the format field of the response
 *                                  gets stored here.  One of:
 *                                     o BLE_ATT_FIND_INFO_RSP_FORMAT_16BIT
 *                                     o BLE_ATT_FIND_INFO_RSP_FORMAT_128BIT
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
ble_att_svr_fill_info(struct ble_att_find_info_req *req, struct os_mbuf *om,
                      uint16_t mtu, uint8_t *format)
{
    struct ble_att_svr_entry *ha;
    uint16_t uuid16;
    uint8_t *buf;
    int num_entries;
    int entry_sz;
    int rc;

    *format = 0;
    num_entries = 0;
    rc = 0;

    STAILQ_FOREACH(ha, &ble_att_svr_list, ha_next) {
        if (ha->ha_handle_id > req->bafq_end_handle) {
            rc = 0;
            goto done;
        }
        if (ha->ha_handle_id >= req->bafq_start_handle) {
            uuid16 = ble_uuid_128_to_16(ha->ha_uuid);

            if (uuid16 != 0) {
                if (*format == 0) {
                    *format = BLE_ATT_FIND_INFO_RSP_FORMAT_16BIT;
                } else if (*format != BLE_ATT_FIND_INFO_RSP_FORMAT_16BIT) {
                    rc = 0;
                    goto done;
                }

                entry_sz = 4;
            } else {
                if (*format == 0) {
                    *format = BLE_ATT_FIND_INFO_RSP_FORMAT_128BIT;
                } else if (*format != BLE_ATT_FIND_INFO_RSP_FORMAT_128BIT) {
                    rc = 0;
                    goto done;
                }
                entry_sz = 18;
            }

            if (OS_MBUF_PKTLEN(om) + entry_sz > mtu) {
                rc = 0;
                goto done;
            }

            buf = os_mbuf_extend(om, entry_sz);
            if (buf == NULL) {
                rc = BLE_HS_ENOMEM;
                goto done;
            }

            htole16(buf + 0, ha->ha_handle_id);

            switch (*format) {
            case BLE_ATT_FIND_INFO_RSP_FORMAT_16BIT:
                htole16(buf + 2, uuid16);
                break;

            case BLE_ATT_FIND_INFO_RSP_FORMAT_128BIT:
                memcpy(buf + 2, ha->ha_uuid, sizeof ha->ha_uuid);
                break;

            default:
                BLE_HS_DBG_ASSERT(0);
                break;
            }

            num_entries++;
        }
    }

done:
    if (rc == 0 && num_entries == 0) {
        return BLE_HS_ENOENT;
    } else {
        return rc;
    }
}

static int
ble_att_svr_build_find_info_rsp(uint16_t conn_handle,
                                struct ble_att_find_info_req *req,
                                struct os_mbuf **out_txom,
                                uint8_t *att_err)
{
    struct ble_att_find_info_rsp rsp;
    struct os_mbuf *txom;
    uint16_t mtu;
    void *buf;
    int rc;

    txom = NULL;

    mtu = ble_att_mtu(conn_handle);
    if (mtu == 0) {
        rc = BLE_HS_ENOTCONN;
        goto done;
    }

    txom = ble_hs_misc_pkthdr();
    if (txom == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    /* Write the response base at the start of the buffer.  The format field is
     * unknown at this point; it will be filled in later.
     */
    buf = os_mbuf_extend(txom, BLE_ATT_FIND_INFO_RSP_BASE_SZ);
    if (buf == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_att_find_info_rsp_write(buf, BLE_ATT_FIND_INFO_RSP_BASE_SZ, &rsp);

    /* Write the variable length Information Data field, populating the format
     * field as appropriate.
     */
    rc = ble_att_svr_fill_info(req, txom, mtu, txom->om_data + 1);
    if (rc != 0) {
        *att_err = BLE_ATT_ERR_ATTR_NOT_FOUND;
        rc = BLE_HS_ENOENT;
        goto done;
    }

    rc = 0;

done:
    *out_txom = txom;
    return rc;
}

int
ble_att_svr_rx_find_info(uint16_t conn_handle, struct os_mbuf **rxom)
{
#if !NIMBLE_OPT_ATT_SVR_FIND_INFO
    return BLE_HS_ENOTSUP;
#endif

    struct ble_att_find_info_req req;
    struct os_mbuf *txom;
    uint16_t err_handle;
    uint8_t att_err;
    int rc;

    /* Initialize some values in case of early error. */
    txom = NULL;
    att_err = 0;
    err_handle = 0;

    rc = ble_att_svr_pullup_req_base(rxom, BLE_ATT_FIND_INFO_REQ_SZ, &att_err);
    if (rc != 0) {
        err_handle = 0;
        goto done;
    }

    ble_att_find_info_req_parse((*rxom)->om_data, (*rxom)->om_len, &req);

    /* Tx error response if start handle is greater than end handle or is equal
     * to 0 (Vol. 3, Part F, 3.4.3.1).
     */
    if (req.bafq_start_handle > req.bafq_end_handle ||
        req.bafq_start_handle == 0) {

        att_err = BLE_ATT_ERR_INVALID_HANDLE;
        err_handle = req.bafq_start_handle;
        rc = BLE_HS_EBADDATA;
        goto done;
    }

    rc = ble_att_svr_build_find_info_rsp(conn_handle, &req, &txom, &att_err);
    if (rc != 0) {
        err_handle = req.bafq_start_handle;
        goto done;
    }

    rc = 0;

done:
    rc = ble_att_svr_tx_rsp(conn_handle, rc, txom, BLE_ATT_OP_FIND_INFO_REQ,
                            att_err, err_handle);
    return rc;
}

/**
 * Processes a single non-matching attribute entry while filling a
 * Find-By-Type-Value-Response.
 *
 * @param om                    The response mbuf.
 * @param first                 Pointer to the first matching handle ID in the
 *                                  current group of IDs.  0 if there is not a
 *                                  current group.
 * @param prev                  Pointer to the most recent matching handle ID
 *                                  in the current group of IDs.  0 if there is
 *                                  not a current group.
 * @param mtu                   The ATT L2CAP channel MTU.
 *
 * @return                      0 if the response should be sent;
 *                              BLE_HS_EAGAIN if the entry was successfully
 *                                  processed and subsequent entries can be
 *                                  inspected.
 *                              Other nonzero on error.
 */
static int
ble_att_svr_fill_type_value_no_match(struct os_mbuf *om, uint16_t *first,
                                     uint16_t *prev, int mtu,
                                     uint8_t *out_att_err)
{
    uint16_t u16;
    int rsp_sz;
    int rc;

    /* If there is no current group, then there is nothing to do. */
    if (*first == 0) {
        return BLE_HS_EAGAIN;
    }

    rsp_sz = OS_MBUF_PKTHDR(om)->omp_len + 4;
    if (rsp_sz > mtu) {
        return 0;
    }

    u16 = *first;
    htole16(&u16, u16);
    rc = os_mbuf_append(om, &u16, 2);
    if (rc != 0) {
        *out_att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        return BLE_HS_ENOMEM;
    }

    u16 = *prev;
    htole16(&u16, u16);
    rc = os_mbuf_append(om, &u16, 2);
    if (rc != 0) {
        *out_att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        return BLE_HS_ENOMEM;
    }

    *first = 0;
    *prev = 0;

    return BLE_HS_EAGAIN;
}

/**
 * Processes a single matching attribute entry while filling a
 * Find-By-Type-Value-Response.
 *
 * @param om                    The response mbuf.
 * @param first                 Pointer to the first matching handle ID in the
 *                                  current group of IDs.  0 if there is not a
 *                                  current group.
 * @param prev                  Pointer to the most recent matching handle ID
 *                                  in the current group of IDs.  0 if there is
 *                                  not a current group.
 * @param handle_id             The matching handle ID to process.
 * @param mtu                   The ATT L2CAP channel MTU.
 *
 * @return                      0 if the response should be sent;
 *                              BLE_HS_EAGAIN if the entry was successfully
 *                                  processed and subsequent entries can be
 *                                  inspected.
 *                              Other nonzero on error.
 */
static int
ble_att_svr_fill_type_value_match(struct os_mbuf *om, uint16_t *first,
                                  uint16_t *prev, uint16_t handle_id,
                                  int mtu, uint8_t *out_att_err)
{
    int rc;

    /* If this is the start of a group, record it as the first ID and keep
     * searching.
     */
    if (*first == 0) {
        *first = handle_id;
        *prev = handle_id;
        return BLE_HS_EAGAIN;
    }

    /* If this is the continuation of a group, keep searching. */
    if (handle_id == *prev + 1) {
        *prev = handle_id;
        return BLE_HS_EAGAIN;
    }

    /* Otherwise, this handle is not a part of the previous group.  Write the
     * previous group to the response, and remember this ID as the start of the
     * next group.
     */
    rc = ble_att_svr_fill_type_value_no_match(om, first, prev, mtu,
                                              out_att_err);
    *first = handle_id;
    *prev = handle_id;
    return rc;
}

/**
 * Fills the supplied mbuf with the variable length Handles-Information-List
 * field of a Find-By-Type-Value ATT response.
 *
 * @param req                   The Find-By-Type-Value-Request being responded
 *                                  to.
 * @param rxom                  The mbuf containing the received request.
 * @param txom                  The destination mbuf where the
 *                                  Handles-Information-List field gets
 *                                  written.
 * @param mtu                   The ATT L2CAP channel MTU.
 *
 * @return                      0 on success;
 *                              BLE_HS_ENOENT if attribute not found;
 *                              BLE_HS_EAPP on other error.
 */
static int
ble_att_svr_fill_type_value(uint16_t conn_handle,
                            struct ble_att_find_type_value_req *req,
                            struct os_mbuf *rxom, struct os_mbuf *txom,
                            uint16_t mtu, uint8_t *out_att_err)
{
    struct ble_att_svr_access_ctxt ctxt;
    struct ble_att_svr_entry *ha;
    uint16_t uuid16;
    uint16_t first;
    uint16_t prev;
    int any_entries;
    int match;
    int rc;

    first = 0;
    prev = 0;
    rc = 0;

    /* Iterate through the attribute list, keeping track of the current
     * matching group.  For each attribute entry, determine if data needs to be
     * written to the response.
     */
    STAILQ_FOREACH(ha, &ble_att_svr_list, ha_next) {
        match = 0;

        if (ha->ha_handle_id > req->bavq_end_handle) {
            break;
        }

        if (ha->ha_handle_id >= req->bavq_start_handle) {
            /* Compare the attribute type and value to the request fields to
             * determine if this attribute matches.
             */
            uuid16 = ble_uuid_128_to_16(ha->ha_uuid);
            if (uuid16 == req->bavq_attr_type) {
                ctxt.offset = 0;
                rc = ble_att_svr_read(conn_handle, ha, &ctxt, out_att_err);
                if (rc != 0) {
                    goto done;
                }
                rc = os_mbuf_memcmp(rxom, BLE_ATT_FIND_TYPE_VALUE_REQ_BASE_SZ,
                                    ctxt.attr_data, ctxt.data_len);
                if (rc == 0) {
                    match = 1;
                }
            }
        }

        if (match) {
            rc = ble_att_svr_fill_type_value_match(txom, &first, &prev,
                                                   ha->ha_handle_id, mtu,
                                                   out_att_err);
        } else {
            rc = ble_att_svr_fill_type_value_no_match(txom, &first, &prev,
                                                      mtu, out_att_err);
        }

        if (rc != BLE_HS_EAGAIN) {
            goto done;
        }
    }

    /* Process one last non-matching ID in case a group was in progress when
     * the end of the attribute list was reached.
     */
    rc = ble_att_svr_fill_type_value_no_match(txom, &first, &prev, mtu,
                                              out_att_err);
    if (rc == BLE_HS_EAGAIN) {
        rc = 0;
    }

done:
    any_entries = OS_MBUF_PKTHDR(txom)->omp_len >
                  BLE_ATT_FIND_TYPE_VALUE_RSP_BASE_SZ;
    if (rc == 0 && !any_entries) {
        *out_att_err = BLE_ATT_ERR_ATTR_NOT_FOUND;
        return BLE_HS_ENOENT;
    } else {
        return rc;
    }
}

static int
ble_att_svr_build_find_type_value_rsp(uint16_t conn_handle,
                                      struct ble_att_find_type_value_req *req,
                                      struct os_mbuf *rxom,
                                      struct os_mbuf **out_txom,
                                      uint8_t *out_att_err)
{
    struct os_mbuf *txom;
    uint16_t mtu;
    uint8_t *buf;
    int rc;

    txom = ble_hs_misc_pkthdr();
    if (txom == NULL) {
        *out_att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    /* Write the response base at the start of the buffer. */
    buf = os_mbuf_extend(txom, BLE_ATT_FIND_TYPE_VALUE_RSP_BASE_SZ);
    if (buf == NULL) {
        *out_att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }
    buf[0] = BLE_ATT_OP_FIND_TYPE_VALUE_RSP;

    /* Write the variable length Information Data field. */
    mtu = ble_att_mtu(conn_handle);
    if (mtu == 0) {
        rc = BLE_HS_ENOTCONN;
        goto done;
    }

    rc = ble_att_svr_fill_type_value(conn_handle, req, rxom, txom, mtu,
                                     out_att_err);
    if (rc != 0) {
        goto done;
    }

    rc = 0;

done:
    *out_txom = txom;
    return rc;
}

int
ble_att_svr_rx_find_type_value(uint16_t conn_handle, struct os_mbuf **rxom)
{
#if !NIMBLE_OPT_ATT_SVR_FIND_TYPE
    return BLE_HS_ENOTSUP;
#endif

    struct ble_att_find_type_value_req req;
    struct os_mbuf *txom;
    uint16_t err_handle;
    uint8_t att_err;
    int rc;

    /* Initialize some values in case of early error. */
    txom = NULL;
    att_err = 0;
    err_handle = 0;

    rc = ble_att_svr_pullup_req_base(rxom, BLE_ATT_FIND_TYPE_VALUE_REQ_BASE_SZ,
                                     &att_err);
    if (rc != 0) {
        err_handle = 0;
        goto done;
    }

    ble_att_find_type_value_req_parse((*rxom)->om_data, (*rxom)->om_len, &req);

    /* Tx error response if start handle is greater than end handle or is equal
     * to 0 (Vol. 3, Part F, 3.4.3.3).
     */
    if (req.bavq_start_handle > req.bavq_end_handle ||
        req.bavq_start_handle == 0) {

        att_err = BLE_ATT_ERR_INVALID_HANDLE;
        err_handle = req.bavq_start_handle;
        rc = BLE_HS_EBADDATA;
        goto done;
    }

    rc = ble_att_svr_build_find_type_value_rsp(conn_handle, &req, *rxom,
                                               &txom, &att_err);
    if (rc != 0) {
        err_handle = req.bavq_start_handle;
        goto done;
    }

    rc = 0;

done:
    rc = ble_att_svr_tx_rsp(conn_handle, rc, txom,
                            BLE_ATT_OP_FIND_TYPE_VALUE_REQ, att_err,
                            err_handle);
    return rc;
}

static int
ble_att_svr_build_read_type_rsp(uint16_t conn_handle,
                                struct ble_att_read_type_req *req,
                                uint8_t *uuid128,
                                struct os_mbuf **out_txom,
                                uint8_t *att_err,
                                uint16_t *err_handle)
{
    struct ble_att_read_type_rsp rsp;
    struct ble_att_svr_access_ctxt ctxt;
    struct ble_att_svr_entry *entry;
    struct os_mbuf *txom;
    uint16_t mtu;
    uint8_t *dptr;
    int entry_written;
    int txomlen;
    int prev_attr_len;
    int attr_len;
    int rc;

    *att_err = 0;    /* Silence unnecessary warning. */

    *err_handle = req->batq_start_handle;
    entry_written = 0;
    prev_attr_len = 0;

    mtu = ble_att_mtu(conn_handle);
    if (mtu == 0) {
        return BLE_HS_ENOTCONN;
    }

    txom = ble_hs_misc_pkthdr();
    if (txom == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        *err_handle = 0;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    /* Allocate space for the respose base, but don't fill in the fields.  They
     * get filled in at the end, when we know the value of the length field.
     */
    dptr = os_mbuf_extend(txom, BLE_ATT_READ_TYPE_RSP_BASE_SZ);
    if (dptr == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        *err_handle = 0;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    /* Find all matching attributes, writing a record for each. */
    entry = NULL;
    while (1) {
        entry = ble_att_svr_find_by_uuid(entry, uuid128);
        if (entry == NULL) {
            rc = BLE_HS_ENOENT;
            break;
        }

        if (entry->ha_handle_id > req->batq_end_handle) {
            break;
        }

        if (entry->ha_handle_id >= req->batq_start_handle) {
            ctxt.offset = 0;
            rc = ble_att_svr_read(conn_handle, entry, &ctxt, att_err);
            if (rc != 0) {
                *err_handle = entry->ha_handle_id;
                goto done;
            }

            if (ctxt.data_len > mtu - 4) {
                attr_len = mtu - 4;
            } else {
                attr_len = ctxt.data_len;
            }

            if (prev_attr_len == 0) {
                prev_attr_len = attr_len;
            } else if (prev_attr_len != attr_len) {
                break;
            }

            txomlen = OS_MBUF_PKTHDR(txom)->omp_len + 2 + attr_len;
            if (txomlen > mtu) {
                break;
            }

            dptr = os_mbuf_extend(txom, 2 + attr_len);
            if (dptr == NULL) {
                *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
                *err_handle = entry->ha_handle_id;
                rc = BLE_HS_ENOMEM;
                goto done;
            }

            htole16(dptr + 0, entry->ha_handle_id);
            memcpy(dptr + 2, ctxt.attr_data, attr_len);
            entry_written = 1;
        }
    }

done:
    if (!entry_written) {
        /* No matching attributes. */
        if (*att_err == 0) {
            *att_err = BLE_ATT_ERR_ATTR_NOT_FOUND;
        }
        if (rc == 0) {
            rc = BLE_HS_ENOENT;
        }
    } else {
        /* Send what we can, even if an error was encountered. */
        rc = 0;
        *att_err = 0;

        /* Fill the response base. */
        rsp.batp_length = BLE_ATT_READ_TYPE_ADATA_BASE_SZ + prev_attr_len;
        ble_att_read_type_rsp_write(txom->om_data, txom->om_len, &rsp);
    }

    *out_txom = txom;

    return rc;
}

int
ble_att_svr_rx_read_type(uint16_t conn_handle, struct os_mbuf **rxom)
{
#if !NIMBLE_OPT_ATT_SVR_READ_TYPE
    return BLE_HS_ENOTSUP;
#endif

    struct ble_att_read_type_req req;
    struct os_mbuf *txom;
    uint16_t err_handle;
    uint16_t uuid16;
    uint16_t pktlen;
    uint8_t uuid128[16];
    uint8_t att_err;
    int rc;

    /* Initialize some values in case of early error. */
    txom = NULL;

    pktlen = OS_MBUF_PKTLEN(*rxom);
    if (pktlen != BLE_ATT_READ_TYPE_REQ_SZ_16 &&
        pktlen != BLE_ATT_READ_TYPE_REQ_SZ_128) {

        /* Malformed packet; discard. */
        return BLE_HS_EBADDATA;
    }

    rc = ble_att_svr_pullup_req_base(rxom, pktlen, &att_err);
    if (rc != 0) {
        err_handle = 0;
        goto done;
    }

    ble_att_read_type_req_parse((*rxom)->om_data, (*rxom)->om_len, &req);

    if (req.batq_start_handle > req.batq_end_handle ||
        req.batq_start_handle == 0) {

        att_err = BLE_ATT_ERR_INVALID_HANDLE;
        err_handle = req.batq_start_handle;
        rc = BLE_HS_EBADDATA;
        goto done;
    }

    switch ((*rxom)->om_len) {
    case BLE_ATT_READ_TYPE_REQ_SZ_16:
        uuid16 = le16toh((*rxom)->om_data + 5);
        rc = ble_uuid_16_to_128(uuid16, uuid128);
        if (rc != 0) {
            att_err = BLE_ATT_ERR_ATTR_NOT_FOUND;
            err_handle = 0;
            rc = BLE_HS_EBADDATA;
            goto done;
        }
        break;

    case BLE_ATT_READ_TYPE_REQ_SZ_128:
        memcpy(uuid128, (*rxom)->om_data + 5, 16);
        break;

    default:
        att_err = BLE_ATT_ERR_INVALID_PDU;
        err_handle = 0;
        rc = BLE_HS_EMSGSIZE;
        goto done;
    }

    rc = ble_att_svr_build_read_type_rsp(conn_handle, &req, uuid128,
                                         &txom, &att_err, &err_handle);
    if (rc != 0) {
        goto done;
    }

    rc = 0;

done:
    rc = ble_att_svr_tx_rsp(conn_handle, rc, txom, BLE_ATT_OP_READ_TYPE_REQ,
                            att_err, err_handle);
    return rc;
}

/**
 * @return                      0 on success; nonzero on failure.
 */
static int
ble_att_svr_build_read_rsp(uint16_t conn_handle, void *attr_data, int attr_len,
                           struct os_mbuf **out_txom, uint8_t *att_err)
{
    struct os_mbuf *txom;
    uint16_t data_len;
    uint16_t mtu;
    uint8_t op;
    int rc;

    txom = NULL;

    mtu = ble_att_mtu(conn_handle);
    if (mtu == 0) {
        rc = BLE_HS_ENOTCONN;
        goto done;
    }

    txom = ble_hs_misc_pkthdr();
    if (txom == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    op = BLE_ATT_OP_READ_RSP;
    rc = os_mbuf_append(txom, &op, 1);
    if (rc != 0) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    /* Vol. 3, part F, 3.2.9; don't send more than ATT_MTU-1 bytes of data. */
    if (attr_len > mtu - 1) {
        data_len = mtu - 1;
    } else {
        data_len = attr_len;
    }

    rc = os_mbuf_append(txom, attr_data, data_len);
    if (rc != 0) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    rc = 0;

done:
    *out_txom = txom;
    return rc;
}

int
ble_att_svr_rx_read(uint16_t conn_handle, struct os_mbuf **rxom)
{
#if !NIMBLE_OPT_ATT_SVR_READ
    return BLE_HS_ENOTSUP;
#endif

    struct ble_att_svr_access_ctxt ctxt;
    struct ble_att_read_req req;
    struct os_mbuf *txom;
    uint16_t err_handle;
    uint8_t att_err;
    int rc;

    /* Initialize some values in case of early error. */
    txom = NULL;
    att_err = 0;
    err_handle = 0;

    rc = ble_att_svr_pullup_req_base(rxom, BLE_ATT_READ_REQ_SZ, &att_err);
    if (rc != 0) {
        err_handle = 0;
        goto done;
    }

    ble_att_read_req_parse((*rxom)->om_data, (*rxom)->om_len, &req);

    ctxt.offset = 0;
    rc = ble_att_svr_read_handle(conn_handle, req.barq_handle, &ctxt,
                                 &att_err);
    if (rc != 0) {
        err_handle = req.barq_handle;
        goto done;
    }

    rc = ble_att_svr_build_read_rsp(conn_handle, ctxt.attr_data, ctxt.data_len,
                                    &txom, &att_err);
    if (rc != 0) {
        err_handle = req.barq_handle;
        goto done;
    }

    rc = 0;

done:
    rc = ble_att_svr_tx_rsp(conn_handle, rc, txom, BLE_ATT_OP_READ_REQ,
                            att_err, err_handle);
    return rc;
}

/**
 * @return                      0 on success; nonzero on failure.
 */
static int
ble_att_svr_build_read_blob_rsp(void *attr_data, int attr_len, uint16_t mtu,
                                struct os_mbuf **out_txom, uint8_t *att_err)
{
    struct os_mbuf *txom;
    uint16_t data_len;
    uint8_t op;
    int rc;

    txom = ble_hs_misc_pkthdr();
    if (txom == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    op = BLE_ATT_OP_READ_BLOB_RSP;
    rc = os_mbuf_append(txom, &op, 1);
    if (rc != 0) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    /* Vol. 3, part F, 3.2.9; don't send more than ATT_MTU-1 bytes of data. */
    if (attr_len > mtu - 1) {
        data_len = mtu - 1;
    } else {
        data_len = attr_len;
    }

    rc = os_mbuf_append(txom, attr_data, data_len);
    if (rc != 0) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    rc = 0;

done:
    *out_txom = txom;
    return rc;
}

int
ble_att_svr_rx_read_blob(uint16_t conn_handle, struct os_mbuf **rxom)
{
#if !NIMBLE_OPT_ATT_SVR_READ_BLOB
    return BLE_HS_ENOTSUP;
#endif

    struct ble_att_svr_access_ctxt ctxt;
    struct ble_att_read_blob_req req;
    struct os_mbuf *txom;
    uint16_t err_handle;
    uint16_t mtu;
    uint8_t att_err;
    int rc;

    /* Initialize some values in case of early error. */
    txom = NULL;
    att_err = 0;
    err_handle = 0;

    mtu = ble_att_mtu(conn_handle);
    if (mtu == 0) {
        rc = BLE_HS_ENOTCONN;
        goto done;
    }

    rc = ble_att_svr_pullup_req_base(rxom, BLE_ATT_READ_BLOB_REQ_SZ, &att_err);
    if (rc != 0) {
        err_handle = 0;
        goto done;
    }

    ble_att_read_blob_req_parse((*rxom)->om_data, (*rxom)->om_len, &req);

    ctxt.offset = req.babq_offset;
    rc = ble_att_svr_read_handle(conn_handle, req.babq_handle, &ctxt,
                                 &att_err);
    if (rc != 0) {
        err_handle = req.babq_handle;
        goto done;
    }

    if (ctxt.offset + ctxt.data_len <= mtu - 3) {
        att_err = BLE_ATT_ERR_ATTR_NOT_LONG;
        err_handle = req.babq_handle;
        rc = BLE_HS_ENOTSUP;
        goto done;
    }

    rc = ble_att_svr_build_read_blob_rsp(ctxt.attr_data, ctxt.data_len, mtu,
                                         &txom, &att_err);
    if (rc != 0) {
        err_handle = req.babq_handle;
        goto done;
    }

    rc = 0;

done:
    rc = ble_att_svr_tx_rsp(conn_handle, rc, txom, BLE_ATT_OP_READ_BLOB_REQ,
                            att_err, err_handle);
    return rc;
}

static int
ble_att_svr_build_read_mult_rsp(uint16_t conn_handle,
                                struct os_mbuf **rxom,
                                struct os_mbuf **out_txom,
                                uint8_t *att_err,
                                uint16_t *err_handle)
{
    struct ble_att_svr_access_ctxt ctxt;
    struct os_mbuf *txom;
    uint16_t chunk_sz;
    uint16_t tx_space;
    uint16_t handle;
    uint16_t mtu;
    uint8_t *dptr;
    int rc;

    txom = NULL;

    mtu = ble_att_mtu(conn_handle);
    if (mtu == 0) {
        rc = BLE_HS_ENOTCONN;
        goto done;
    }

    txom = ble_hs_misc_pkthdr();
    if (txom == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        *err_handle = 0;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    dptr = os_mbuf_extend(txom, BLE_ATT_READ_MULT_RSP_BASE_SZ);
    if (dptr == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        *err_handle = 0;
        rc = BLE_HS_ENOMEM;
        goto done;
    }
    ble_att_read_mult_rsp_write(dptr, BLE_ATT_READ_MULT_RSP_BASE_SZ);

    tx_space = mtu - OS_MBUF_PKTLEN(txom);

    /* Iterate through requested handles, reading the corresponding attribute
     * for each.  Stop when there are no more handles to process, or the
     * response is full.
     */
    while (OS_MBUF_PKTLEN(*rxom) >= 2 && tx_space > 0) {
        /* Ensure the full 16-bit handle is contiguous at the start of the
         * mbuf.
         */
        rc = ble_att_svr_pullup_req_base(rxom, 2, att_err);
        if (rc != 0) {
            *err_handle = 0;
            goto done;
        }

        /* Extract the 16-bit handle and strip it from the front of the
         * mbuf.
         */
        handle = le16toh((*rxom)->om_data);
        os_mbuf_adj(*rxom, 2);

        ctxt.offset = 0;
        rc = ble_att_svr_read_handle(conn_handle, handle, &ctxt, att_err);
        if (rc != 0) {
            *err_handle = handle;
            goto done;
        }

        if (ctxt.data_len > tx_space) {
            chunk_sz = tx_space;
        } else {
            chunk_sz = ctxt.data_len;
        }

        rc = os_mbuf_append(txom, ctxt.attr_data, chunk_sz);
        if (rc != 0) {
            *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
            *err_handle = handle;
            rc = BLE_HS_ENOMEM;
            goto done;
        }

        tx_space -= chunk_sz;
    }

    rc = 0;

done:
    *out_txom = txom;
    return rc;
}

int
ble_att_svr_rx_read_mult(uint16_t conn_handle, struct os_mbuf **rxom)
{
#if !NIMBLE_OPT_ATT_SVR_READ_MULT
    return BLE_HS_ENOTSUP;
#endif

    struct os_mbuf *txom;
    uint16_t err_handle;
    uint8_t att_err;
    int rc;

    /* Initialize some values in case of early error. */
    txom = NULL;
    err_handle = 0;
    att_err = 0;

    rc = ble_att_svr_pullup_req_base(rxom, BLE_ATT_READ_MULT_REQ_BASE_SZ,
                                     &att_err);
    if (rc != 0) {
        err_handle = 0;
        goto done;
    }

    ble_att_read_mult_req_parse((*rxom)->om_data, (*rxom)->om_len);

    /* Strip opcode from request. */
    os_mbuf_adj(*rxom, BLE_ATT_READ_MULT_REQ_BASE_SZ);

    rc = ble_att_svr_build_read_mult_rsp(conn_handle, rxom, &txom, &att_err,
                                         &err_handle);
    if (rc != 0) {
        goto done;
    }

    rc = 0;

done:
    rc = ble_att_svr_tx_rsp(conn_handle, rc, txom, BLE_ATT_OP_READ_MULT_REQ,
                            att_err, err_handle);
    return rc;
}

static int
ble_att_svr_is_valid_group_type(uint8_t *uuid128)
{
    uint16_t uuid16;

    uuid16 = ble_uuid_128_to_16(uuid128);

    return uuid16 == BLE_ATT_UUID_PRIMARY_SERVICE ||
           uuid16 == BLE_ATT_UUID_SECONDARY_SERVICE;
}

static int
ble_att_svr_service_uuid(struct ble_att_svr_entry *entry, uint16_t *uuid16,
                         uint8_t *uuid128)
{
    struct ble_att_svr_access_ctxt ctxt;
    int rc;

    ctxt.offset = 0;
    rc = ble_att_svr_read(BLE_HS_CONN_HANDLE_NONE, entry, &ctxt, NULL);
    if (rc != 0) {
        return rc;
    }

    switch (ctxt.data_len) {
    case 16:
        *uuid16 = 0;
        memcpy(uuid128, ctxt.attr_data, 16);
        return 0;

    case 2:
        *uuid16 = le16toh(ctxt.attr_data);
        if (*uuid16 == 0) {
            return BLE_HS_EINVAL;
        }
        return 0;

    default:
        return BLE_HS_EINVAL;
    }
}

static int
ble_att_svr_read_group_type_entry_write(struct os_mbuf *om, uint16_t mtu,
                                        uint16_t start_group_handle,
                                        uint16_t end_group_handle,
                                        uint16_t service_uuid16,
                                        uint8_t *service_uuid128)
{
    uint8_t *buf;
    int len;

    if (service_uuid16 != 0) {
        len = BLE_ATT_READ_GROUP_TYPE_ADATA_SZ_16;
    } else {
        len = BLE_ATT_READ_GROUP_TYPE_ADATA_SZ_128;
    }
    if (OS_MBUF_PKTLEN(om) + len > mtu) {
        return BLE_HS_EMSGSIZE;
    }

    buf = os_mbuf_extend(om, len);
    if (buf == NULL) {
        return BLE_HS_ENOMEM;
    }

    htole16(buf + 0, start_group_handle);
    htole16(buf + 2, end_group_handle);
    if (service_uuid16 != 0) {
        htole16(buf + 4, service_uuid16);
    } else {
        memcpy(buf + 4, service_uuid128, 16);
    }

    return 0;
}

/**
 * @return                      0 on success; BLE_HS error code on failure.
 */
static int
ble_att_svr_build_read_group_type_rsp(uint16_t conn_handle,
                                      struct ble_att_read_group_type_req *req,
                                      uint8_t *group_uuid128,
                                      struct os_mbuf **out_txom,
                                      uint8_t *att_err,
                                      uint16_t *err_handle)
{
    struct ble_att_read_group_type_rsp rsp;
    struct ble_att_svr_entry *entry;
    struct os_mbuf *txom;
    uint16_t start_group_handle;
    uint16_t end_group_handle;
    uint16_t service_uuid16;
    uint16_t mtu;
    uint8_t service_uuid128[16];
    void *rsp_buf;
    int rc;

    /* Silence warnings. */
    rsp_buf = NULL;
    service_uuid16 = 0;
    end_group_handle = 0;

    *att_err = 0;
    *err_handle = req->bagq_start_handle;

    txom = NULL;

    mtu = ble_att_mtu(conn_handle);
    if (mtu == 0) {
        rc = BLE_HS_ENOTCONN;
        goto done;
    }

    txom = ble_hs_misc_pkthdr();
    if (txom == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    /* Reserve space for the response base. */
    rsp_buf = os_mbuf_extend(txom, BLE_ATT_READ_GROUP_TYPE_RSP_BASE_SZ);
    if (rsp_buf == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    start_group_handle = 0;
    rsp.bagp_length = 0;
    STAILQ_FOREACH(entry, &ble_att_svr_list, ha_next) {
        if (entry->ha_handle_id < req->bagq_start_handle) {
            continue;
        }
        if (entry->ha_handle_id > req->bagq_end_handle) {
            /* The full input range has been searched. */
            rc = 0;
            goto done;
        }

        if (start_group_handle != 0) {
            /* We have already found the start of a group. */
            if (!ble_att_svr_is_valid_group_type(entry->ha_uuid)) {
                /* This attribute is part of the current group. */
                end_group_handle = entry->ha_handle_id;
            } else {
                /* This attribute marks the end of the group.  Write an entry
                 * representing the group to the response.
                 */
                rc = ble_att_svr_read_group_type_entry_write(
                    txom, mtu, start_group_handle, end_group_handle,
                    service_uuid16, service_uuid128);
                start_group_handle = 0;
                end_group_handle = 0;
                if (rc != 0) {
                    *err_handle = entry->ha_handle_id;
                    if (rc == BLE_HS_ENOMEM) {
                        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
                    } else {
                        BLE_HS_DBG_ASSERT(rc == BLE_HS_EMSGSIZE);
                    }
                    goto done;
                }
            }
        }

        if (start_group_handle == 0) {
            /* We are looking for the start of a group. */
            if (memcmp(entry->ha_uuid, group_uuid128, 16) == 0) {
                /* Found a group start.  Read the group UUID. */
                rc = ble_att_svr_service_uuid(entry, &service_uuid16,
                                              service_uuid128);
                if (rc != 0) {
                    *err_handle = entry->ha_handle_id;
                    *att_err = BLE_ATT_ERR_UNLIKELY;
                    rc = BLE_HS_ENOTSUP;
                    goto done;
                }

                /* Make sure the group UUID lengths are consistent.  If this
                 * group has a different length UUID, then cut the response
                 * short.
                 */
                switch (rsp.bagp_length) {
                case 0:
                    if (service_uuid16 != 0) {
                        rsp.bagp_length = BLE_ATT_READ_GROUP_TYPE_ADATA_SZ_16;
                    } else {
                        rsp.bagp_length = BLE_ATT_READ_GROUP_TYPE_ADATA_SZ_128;
                    }
                    break;

                case BLE_ATT_READ_GROUP_TYPE_ADATA_SZ_16:
                    if (service_uuid16 == 0) {
                        rc = 0;
                        goto done;
                    }
                    break;

                case BLE_ATT_READ_GROUP_TYPE_ADATA_SZ_128:
                    if (service_uuid16 != 0) {
                        rc = 0;
                        goto done;
                    }
                    break;

                default:
                    BLE_HS_DBG_ASSERT(0);
                    goto done;
                }

                start_group_handle = entry->ha_handle_id;
                end_group_handle = entry->ha_handle_id;
            }
        }
    }

    rc = 0;

done:
    if (rc == 0) {
        if (start_group_handle != 0) {
            /* A group was being processed.  Add its corresponding entry to the
             * response.
             */

            if (entry == NULL) {
                /* We have reached the end of the attribute list.  Indicate an
                 * end handle of 0xffff so that the client knows there are no
                 * more attributes without needing to send a follow-up request.
                 */
                end_group_handle = 0xffff;
            }

            rc = ble_att_svr_read_group_type_entry_write(
                txom, mtu, start_group_handle, end_group_handle,
                service_uuid16, service_uuid128);
            if (rc == BLE_HS_ENOMEM) {
                *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
            }
        }

        if (OS_MBUF_PKTLEN(txom) <= BLE_ATT_READ_GROUP_TYPE_RSP_BASE_SZ) {
            *att_err = BLE_ATT_ERR_ATTR_NOT_FOUND;
            rc = BLE_HS_ENOENT;
        }
    }

    if (rc == 0 || rc == BLE_HS_EMSGSIZE) {
        ble_att_read_group_type_rsp_write(rsp_buf,
                                          BLE_ATT_READ_GROUP_TYPE_RSP_BASE_SZ,
                                          &rsp);
    }

    *out_txom = txom;
    return rc;
}

int
ble_att_svr_rx_read_group_type(uint16_t conn_handle, struct os_mbuf **rxom)
{
#if !NIMBLE_OPT_ATT_SVR_READ_GROUP_TYPE
    return BLE_HS_ENOTSUP;
#endif

    struct ble_att_read_group_type_req req;
    struct os_mbuf *txom;
    uint8_t uuid128[16];
    uint16_t err_handle;
    uint16_t pktlen;
    uint8_t att_err;
    int rc;

    /* Initialize some values in case of early error. */
    txom = NULL;

    pktlen = OS_MBUF_PKTLEN(*rxom);
    if (pktlen != BLE_ATT_READ_GROUP_TYPE_REQ_SZ_16 &&
        pktlen != BLE_ATT_READ_GROUP_TYPE_REQ_SZ_128) {

        /* Malformed request; discard. */
        return BLE_HS_EBADDATA;
    }

    rc = ble_att_svr_pullup_req_base(rxom, pktlen, &att_err);
    if (rc != 0) {
        err_handle = 0;
        goto done;
    }

    ble_att_read_group_type_req_parse((*rxom)->om_data, (*rxom)->om_len, &req);

    if (req.bagq_start_handle > req.bagq_end_handle ||
        req.bagq_start_handle == 0) {

        att_err = BLE_ATT_ERR_INVALID_HANDLE;
        err_handle = req.bagq_start_handle;
        rc = BLE_HS_EBADDATA;
        goto done;
    }

    rc = ble_uuid_extract(*rxom, BLE_ATT_READ_GROUP_TYPE_REQ_BASE_SZ,
                          uuid128);
    if (rc != 0) {
        att_err = BLE_ATT_ERR_INVALID_PDU;
        err_handle = req.bagq_start_handle;
        rc = BLE_HS_EBADDATA;
        goto done;
    }

    if (!ble_att_svr_is_valid_group_type(uuid128)) {
        att_err = BLE_ATT_ERR_UNSUPPORTED_GROUP;
        err_handle = req.bagq_start_handle;
        rc = BLE_HS_ENOTSUP;
        goto done;
    }

    rc = ble_att_svr_build_read_group_type_rsp(conn_handle, &req, uuid128,
                                               &txom, &att_err, &err_handle);
    if (rc != 0) {
        goto done;
    }

    rc = 0;

done:
    rc = ble_att_svr_tx_rsp(conn_handle, rc, txom,
                            BLE_ATT_OP_READ_GROUP_TYPE_REQ, att_err,
                            err_handle);
    return rc;
}

static int
ble_att_svr_build_write_rsp(struct os_mbuf **out_txom, uint8_t *att_err)
{
    struct os_mbuf *txom;
    uint8_t *dst;
    int rc;

    txom = ble_hs_misc_pkthdr();
    if (txom == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    dst = os_mbuf_extend(txom, BLE_ATT_WRITE_RSP_SZ);
    if (dst == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    *dst = BLE_ATT_OP_WRITE_RSP;

    rc = 0;

done:
    *out_txom = txom;
    return rc;
}

int
ble_att_svr_rx_write(uint16_t conn_handle, struct os_mbuf **rxom)
{
#if !NIMBLE_OPT_ATT_SVR_WRITE
    return BLE_HS_ENOTSUP;
#endif

    struct ble_att_svr_access_ctxt ctxt;
    struct ble_att_write_req req;
    struct os_mbuf *txom;
    uint16_t err_handle;
    uint8_t att_err;
    int rc;

    /* Initialize some values in case of early error. */
    txom = NULL;
    att_err = 0;
    err_handle = 0;

    rc = ble_att_svr_pullup_req_base(rxom, BLE_ATT_WRITE_REQ_BASE_SZ,
                                     &att_err);
    if (rc != 0) {
        err_handle = 0;
        goto done;
    }

    ble_att_write_req_parse((*rxom)->om_data, (*rxom)->om_len, &req);

    /* Strip the request base from the front of the mbuf. */
    os_mbuf_adj(*rxom, BLE_ATT_WRITE_REQ_BASE_SZ);

    ctxt.attr_data = ble_att_get_flat_buf();
    ctxt.data_len = OS_MBUF_PKTLEN(*rxom);
    os_mbuf_copydata(*rxom, 0, ctxt.data_len, ctxt.attr_data);
    rc = ble_att_svr_write_handle(conn_handle, req.bawq_handle, &ctxt,
                                  &att_err);
    if (rc != 0) {
        err_handle = req.bawq_handle;
        goto done;
    }

    rc = ble_att_svr_build_write_rsp(&txom, &att_err);
    if (rc != 0) {
        err_handle = req.bawq_handle;
        goto done;
    }

    rc = 0;

done:
    rc = ble_att_svr_tx_rsp(conn_handle, rc, txom, BLE_ATT_OP_WRITE_REQ,
                            att_err, err_handle);
    return rc;
}

int
ble_att_svr_rx_write_no_rsp(uint16_t conn_handle, struct os_mbuf **rxom)
{
#if !NIMBLE_OPT_ATT_SVR_WRITE_NO_RSP
    return BLE_HS_ENOTSUP;
#endif

    struct ble_att_svr_access_ctxt ctxt;
    struct ble_att_write_req req;
    uint8_t att_err;
    int rc;

    rc = ble_att_svr_pullup_req_base(rxom, BLE_ATT_WRITE_REQ_BASE_SZ,
                                     &att_err);
    if (rc != 0) {
        return rc;
    }

    ble_att_write_cmd_parse((*rxom)->om_data, (*rxom)->om_len, &req);

    /* Strip the request base from the front of the mbuf. */
    os_mbuf_adj(*rxom, BLE_ATT_WRITE_REQ_BASE_SZ);

    ctxt.attr_data = ble_att_get_flat_buf();
    ctxt.data_len = OS_MBUF_PKTLEN(*rxom);
    os_mbuf_copydata(*rxom, 0, ctxt.data_len, ctxt.attr_data);
    rc = ble_att_svr_write_handle(conn_handle, req.bawq_handle, &ctxt,
                                  &att_err);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

int
ble_att_svr_write_local(uint16_t attr_handle, void *data, uint16_t data_len)
{
    struct ble_att_svr_access_ctxt ctxt;
    int rc;

    ctxt.attr_data = data;
    ctxt.data_len = data_len;
    ctxt.offset = 0;

    rc = ble_att_svr_write_handle(BLE_HS_CONN_HANDLE_NONE, attr_handle, &ctxt,
                                  NULL);

    return rc;
}

static void
ble_att_svr_prep_free(struct ble_att_prep_entry *entry)
{
    os_mbuf_free_chain(entry->bape_value);
    os_memblock_put(&ble_att_svr_prep_entry_pool, entry);
}

static struct ble_att_prep_entry *
ble_att_svr_prep_alloc(void)
{
    struct ble_att_prep_entry *entry;

    entry = os_memblock_get(&ble_att_svr_prep_entry_pool);
    if (entry == NULL) {
        return NULL;
    }

    memset(entry, 0, sizeof *entry);
    entry->bape_value = ble_hs_misc_pkthdr();
    if (entry->bape_value == NULL) {
        ble_att_svr_prep_free(entry);
        return NULL;
    }

    return entry;
}

static struct ble_att_prep_entry *
ble_att_svr_prep_find_prev(struct ble_att_svr_conn *basc, uint16_t handle,
                           uint16_t offset)
{
    struct ble_att_prep_entry *entry;
    struct ble_att_prep_entry *prev;

    prev = NULL;
    SLIST_FOREACH(entry, &basc->basc_prep_list, bape_next) {
        if (entry->bape_handle > handle) {
            break;
        }

        if (entry->bape_handle == handle && entry->bape_offset > offset) {
            break;
        }

        prev = entry;
    }

    return prev;
}

void
ble_att_svr_prep_clear(struct ble_att_svr_conn *basc)
{
    struct ble_att_prep_entry *entry;

    while ((entry = SLIST_FIRST(&basc->basc_prep_list)) != NULL) {
        SLIST_REMOVE_HEAD(&basc->basc_prep_list, bape_next);
        ble_att_svr_prep_free(entry);
    }
}

/**
 * @return                      0 on success; ATT error code on failure.
 */
static int
ble_att_svr_prep_validate(struct ble_att_svr_conn *basc, uint16_t *err_handle)
{
    struct ble_att_prep_entry *entry;
    struct ble_att_prep_entry *prev;
    int cur_len;

    prev = NULL;
    SLIST_FOREACH(entry, &basc->basc_prep_list, bape_next) {
        if (prev == NULL || prev->bape_handle != entry->bape_handle) {
            /* Ensure attribute write starts at offset 0. */
            if (entry->bape_offset != 0) {
                *err_handle = entry->bape_handle;
                return BLE_ATT_ERR_INVALID_OFFSET;
            }
        } else {
            /* Ensure entry continues where previous left off. */
            if (prev->bape_offset + OS_MBUF_PKTLEN(prev->bape_value) !=
                entry->bape_offset) {

                *err_handle = entry->bape_handle;
                return BLE_ATT_ERR_INVALID_OFFSET;
            }
        }

        cur_len = entry->bape_offset + OS_MBUF_PKTLEN(entry->bape_value);
        if (cur_len > BLE_ATT_ATTR_MAX_LEN) {
            *err_handle = entry->bape_handle;
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        prev = entry;
    }

    return 0;
}

/**
 * @return                      0 on success; ATT error code on failure.
 */
static int
ble_att_svr_prep_write(struct ble_hs_conn *conn, uint16_t *err_handle)
{
    struct ble_att_svr_access_ctxt ctxt;
    struct ble_att_prep_entry *entry;
    struct ble_att_prep_entry *next;
    struct ble_att_svr_entry *attr;
    uint8_t *flat_buf;
    uint8_t att_err;
    int buf_off;
    int rc;

    *err_handle = 0; /* Silence unnecessary warning. */

    /* First, validate the contents of the prepare queue. */
    rc = ble_att_svr_prep_validate(&conn->bhc_att_svr, err_handle);
    if (rc != 0) {
        return rc;
    }

    flat_buf = ble_att_get_flat_buf();

    /* Contents are valid; perform the writes. */
    buf_off = 0;
    entry = SLIST_FIRST(&conn->bhc_att_svr.basc_prep_list);
    while (entry != NULL) {
        next = SLIST_NEXT(entry, bape_next);

        rc = os_mbuf_copydata(entry->bape_value, 0,
                              OS_MBUF_PKTLEN(entry->bape_value),
                              flat_buf + buf_off);
        BLE_HS_DBG_ASSERT_EVAL(rc == 0);
        buf_off += OS_MBUF_PKTLEN(entry->bape_value);

        /* If this is the last entry for this attribute, perform the write. */
        if (next == NULL || entry->bape_handle != next->bape_handle) {
            attr = ble_att_svr_find_by_handle(entry->bape_handle);
            if (attr == NULL) {
                *err_handle = entry->bape_handle;
                return BLE_ATT_ERR_INVALID_HANDLE;
            }

            ctxt.attr_data = flat_buf;
            ctxt.data_len = buf_off;
            rc = ble_att_svr_write(conn->bhc_handle, attr, &ctxt, &att_err);
            if (rc != 0) {
                *err_handle = entry->bape_handle;
                return att_err;
            }

            buf_off = 0;
        }

        entry = next;
    }

    return 0;
}

int
ble_att_svr_rx_prep_write(uint16_t conn_handle, struct os_mbuf **rxom)
{
#if !NIMBLE_OPT_ATT_SVR_PREP_WRITE
    return BLE_HS_ENOTSUP;
#endif

    struct ble_att_prep_write_cmd req;
    struct ble_att_prep_entry *prep_entry;
    struct ble_att_prep_entry *prep_prev;
    struct ble_att_svr_entry *attr_entry;
    struct ble_hs_conn *conn;
    struct os_mbuf *srcom;
    struct os_mbuf *txom;
    uint16_t err_handle;
    uint8_t att_err;
    int rc;

    /* Initialize some values in case of early error. */
    prep_entry = NULL;
    txom = NULL;
    att_err = 0;
    err_handle = 0;

    rc = ble_att_svr_pullup_req_base(rxom, BLE_ATT_PREP_WRITE_CMD_BASE_SZ,
                                     &att_err);
    if (rc != 0) {
        err_handle = 0;
        goto done;
    }

    ble_att_prep_write_req_parse((*rxom)->om_data, (*rxom)->om_len, &req);

    /* Strip the request base from the front of the mbuf. */
    os_mbuf_adj(*rxom, BLE_ATT_PREP_WRITE_CMD_BASE_SZ);

    attr_entry = ble_att_svr_find_by_handle(req.bapc_handle);
    if (attr_entry == NULL) {
        rc = BLE_HS_ENOENT;
        att_err = BLE_ATT_ERR_INVALID_HANDLE;
        err_handle = req.bapc_handle;
        goto done;
    }

    prep_entry = ble_att_svr_prep_alloc();
    if (prep_entry == NULL) {
        att_err = BLE_ATT_ERR_PREPARE_QUEUE_FULL;
        err_handle = req.bapc_handle;
        rc = BLE_HS_ENOMEM;
        goto done;
    }
    prep_entry->bape_handle = req.bapc_handle;
    prep_entry->bape_offset = req.bapc_offset;

    ble_hs_lock();

    conn = ble_hs_conn_find(conn_handle);
    if (conn == NULL) {
        rc = BLE_HS_ENOTCONN;
    } else {
        prep_prev = ble_att_svr_prep_find_prev(&conn->bhc_att_svr,
                                               req.bapc_handle,
                                               req.bapc_offset);
        if (prep_prev == NULL) {
            SLIST_INSERT_HEAD(&conn->bhc_att_svr.basc_prep_list, prep_entry,
                              bape_next);
        } else {
            SLIST_INSERT_AFTER(prep_prev, prep_entry, bape_next);
        }

        /* Append attribute value from request onto prep mbuf. */
        for (srcom = *rxom;
             srcom != NULL;
             srcom = SLIST_NEXT(srcom, om_next)) {

            rc = os_mbuf_append(prep_entry->bape_value, srcom->om_data,
                                srcom->om_len);
            if (rc != 0) {
                att_err = BLE_ATT_ERR_PREPARE_QUEUE_FULL;
                err_handle = req.bapc_handle;
                break;
            }
        }
    }

    ble_hs_unlock();

    if (rc != 0) {
        goto done;
    }

    /* The receive buffer now contains the attribute value.  Repurpose this
     * buffer for the response.  Prepend a response header.
     */
    *rxom = os_mbuf_prepend(*rxom, BLE_ATT_PREP_WRITE_CMD_BASE_SZ);
    if (*rxom == NULL) {
        att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        err_handle = req.bapc_handle;
        rc = BLE_HS_ENOMEM;
        goto done;
    }
    txom = *rxom;

    ble_att_prep_write_rsp_write(txom->om_data, BLE_ATT_PREP_WRITE_CMD_BASE_SZ,
                                 &req);

    rc = 0;

done:
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
        ble_hs_lock();

        conn = ble_hs_conn_find(conn_handle);
        if (conn == NULL) {
            rc = BLE_HS_ENOTCONN;
        } else {
            if (prep_entry != NULL) {
                if (prep_prev == NULL) {
                    SLIST_REMOVE_HEAD(&conn->bhc_att_svr.basc_prep_list,
                                      bape_next);
                } else {
                    SLIST_NEXT(prep_prev, bape_next) =
                        SLIST_NEXT(prep_entry, bape_next);
                }

                ble_att_svr_prep_free(prep_entry);
            }
        }

        ble_hs_unlock();
    }

    rc = ble_att_svr_tx_rsp(conn_handle, rc, txom, BLE_ATT_OP_PREP_WRITE_REQ,
                            att_err, err_handle);

    /* Make sure the receive buffer doesn't get freed since we are using it for
     * the response.
     */
    *rxom = NULL;
    return rc;
}

/**
 * @return                      0 on success; nonzero on failure.
 */
static int
ble_att_svr_build_exec_write_rsp(struct os_mbuf **out_txom, uint8_t *att_err)
{
    struct os_mbuf *txom;
    uint8_t *dst;
    int rc;

    txom = ble_hs_misc_pkthdr();
    if (txom == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    dst = os_mbuf_extend(txom, BLE_ATT_EXEC_WRITE_RSP_SZ);
    if (dst == NULL) {
        *att_err = BLE_ATT_ERR_INSUFFICIENT_RES;
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_att_exec_write_rsp_write(dst, BLE_ATT_EXEC_WRITE_RSP_SZ);

    rc = 0;

done:
    *out_txom = txom;
    return rc;
}

int
ble_att_svr_rx_exec_write(uint16_t conn_handle, struct os_mbuf **rxom)
{
#if !NIMBLE_OPT_ATT_SVR_EXEC_WRITE
    return BLE_HS_ENOTSUP;
#endif

    struct ble_att_exec_write_req req;
    struct ble_hs_conn *conn;
    struct os_mbuf *txom;
    uint16_t err_handle;
    uint8_t att_err;
    int rc;

    /* Initialize some values in case of early error. */
    txom = NULL;

    rc = ble_att_svr_pullup_req_base(rxom, BLE_ATT_EXEC_WRITE_REQ_SZ,
                                     &att_err);
    if (rc != 0) {
        err_handle = 0;
        goto done;
    }

    ble_att_exec_write_req_parse((*rxom)->om_data, (*rxom)->om_len, &req);

    rc = ble_att_svr_build_exec_write_rsp(&txom, &att_err);
    if (rc != 0) {
        err_handle = 0;
        goto done;
    }

    rc = 0;

done:
    if (rc == 0) {
        ble_hs_lock();
        conn = ble_hs_conn_find(conn_handle);
        if (conn == NULL) {
            rc = BLE_HS_ENOTCONN;
        } else {
            if (req.baeq_flags & BLE_ATT_EXEC_WRITE_F_CONFIRM) {
                /* Perform attribute writes. */
                att_err = ble_att_svr_prep_write(conn, &err_handle);
                if (att_err != 0) {
                    rc = BLE_HS_EAPP;
                }
            }

            /* Erase all prep entries. */
            ble_att_svr_prep_clear(&conn->bhc_att_svr);
        }
        ble_hs_unlock();
    }

    rc = ble_att_svr_tx_rsp(conn_handle, rc, txom, BLE_ATT_OP_EXEC_WRITE_REQ,
                            att_err, err_handle);
    return rc;
}

int
ble_att_svr_rx_notify(uint16_t conn_handle, struct os_mbuf **rxom)
{
#if !NIMBLE_OPT_ATT_SVR_NOTIFY
    return BLE_HS_ENOTSUP;
#endif

    struct ble_att_notify_req req;
    uint16_t attr_len;
    void *attr_data;
    int rc;

    if (OS_MBUF_PKTLEN(*rxom) < BLE_ATT_NOTIFY_REQ_BASE_SZ) {
        return BLE_HS_EBADDATA;
    }

    rc = ble_att_svr_pullup_req_base(rxom, BLE_ATT_NOTIFY_REQ_BASE_SZ, NULL);
    if (rc != 0) {
        return BLE_HS_ENOMEM;
    }

    ble_att_notify_req_parse((*rxom)->om_data, (*rxom)->om_len, &req);

    if (req.banq_handle == 0) {
        return BLE_HS_EBADDATA;
    }

    /* Strip the request base from the front of the mbuf. */
    os_mbuf_adj(*rxom, BLE_ATT_NOTIFY_REQ_BASE_SZ);

    attr_data = ble_att_get_flat_buf();
    attr_len = OS_MBUF_PKTLEN(*rxom);
    os_mbuf_copydata(*rxom, 0, attr_len, attr_data);

    if (ble_att_svr_notify_cb != NULL) {
        rc = ble_att_svr_notify_cb(conn_handle, req.banq_handle,
                                   attr_data, attr_len,
                                   ble_att_svr_notify_cb_arg);
        if (rc != 0) {
            return BLE_HS_EAPP;
        }
    }

    return 0;
}

/**
 * @return                      0 on success; nonzero on failure.
 */
static int
ble_att_svr_build_indicate_rsp(struct os_mbuf **out_txom)
{
    struct os_mbuf *txom;
    uint8_t *dst;
    int rc;

    txom = ble_hs_misc_pkthdr();
    if (txom == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    dst = os_mbuf_extend(txom, BLE_ATT_INDICATE_RSP_SZ);
    if (dst == NULL) {
        rc = BLE_HS_ENOMEM;
        goto done;
    }

    ble_att_indicate_rsp_write(dst, BLE_ATT_INDICATE_RSP_SZ);

    rc = 0;

done:
    *out_txom = txom;
    return rc;
}

int
ble_att_svr_rx_indicate(uint16_t conn_handle, struct os_mbuf **rxom)
{
#if !NIMBLE_OPT_ATT_SVR_INDICATE
    return BLE_HS_ENOTSUP;
#endif

    struct ble_att_indicate_req req;
    struct os_mbuf *txom;
    uint16_t attr_len;
    void *attr_data;
    int rc;

    /* Initialize some values in case of early error. */
    txom = NULL;

    if (OS_MBUF_PKTLEN(*rxom) < BLE_ATT_INDICATE_REQ_BASE_SZ) {
        rc = BLE_HS_EBADDATA;
        goto done;
    }

    rc = ble_att_svr_pullup_req_base(rxom, BLE_ATT_INDICATE_REQ_BASE_SZ, NULL);
    if (rc != 0) {
        goto done;
    }

    ble_att_indicate_req_parse((*rxom)->om_data, (*rxom)->om_len, &req);

    if (req.baiq_handle == 0) {
        rc = BLE_HS_EBADDATA;
        goto done;
    }

    /* Strip the request base from the front of the mbuf. */
    os_mbuf_adj(*rxom, BLE_ATT_INDICATE_REQ_BASE_SZ);

    attr_data = ble_att_get_flat_buf();
    attr_len = OS_MBUF_PKTLEN(*rxom);
    os_mbuf_copydata(*rxom, 0, attr_len, attr_data);

    if (ble_att_svr_notify_cb != NULL) {
        rc = ble_att_svr_notify_cb(conn_handle, req.baiq_handle,
                                   attr_data, attr_len,
                                   ble_att_svr_notify_cb_arg);
        if (rc != 0) {
            rc = BLE_HS_EAPP;
            goto done;
        }
    }

    rc = ble_att_svr_build_indicate_rsp(&txom);
    if (rc != 0) {
        goto done;
    }

    rc = 0;

done:
    rc = ble_att_svr_tx_rsp(conn_handle, rc, txom, BLE_ATT_OP_INDICATE_REQ,
                            0, 0);
    return rc;
}

static void
ble_att_svr_free_mem(void)
{
    free(ble_att_svr_entry_mem);
    ble_att_svr_entry_mem = NULL;
}

int
ble_att_svr_init(void)
{
    int rc;

    ble_att_svr_free_mem();

    if (ble_hs_cfg.max_attrs > 0) {
        ble_att_svr_entry_mem = malloc(
            OS_MEMPOOL_BYTES(ble_hs_cfg.max_attrs,
                             sizeof (struct ble_att_svr_entry)));
        if (ble_att_svr_entry_mem == NULL) {
            rc = BLE_HS_ENOMEM;
            goto err;
        }

        rc = os_mempool_init(&ble_att_svr_entry_pool, ble_hs_cfg.max_attrs,
                             sizeof (struct ble_att_svr_entry),
                             ble_att_svr_entry_mem, "ble_att_svr_entry_pool");
        if (rc != 0) {
            rc = BLE_HS_EOS;
            goto err;
        }
    }

    if (ble_hs_cfg.max_prep_entries > 0) {
        ble_att_svr_prep_entry_mem = malloc(
            OS_MEMPOOL_BYTES(ble_hs_cfg.max_prep_entries,
                             sizeof (struct ble_att_prep_entry)));
        if (ble_att_svr_prep_entry_mem == NULL) {
            rc = BLE_HS_ENOMEM;
            goto err;
        }

        rc = os_mempool_init(&ble_att_svr_prep_entry_pool,
                             ble_hs_cfg.max_prep_entries,
                             sizeof (struct ble_att_prep_entry),
                             ble_att_svr_prep_entry_mem,
                             "ble_att_svr_prep_entry_pool");
        if (rc != 0) {
            rc = BLE_HS_EOS;
            goto err;
        }
    }

    STAILQ_INIT(&ble_att_svr_list);

    ble_att_svr_id = 0;
    ble_att_svr_notify_cb = NULL;

    return 0;

err:
    ble_att_svr_free_mem();
    return rc;
}
