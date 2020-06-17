/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/mphal.h"

#if MICROPY_PY_BLUETOOTH && MICROPY_BLUETOOTH_BTSTACK

#include "extmod/btstack/modbluetooth_btstack.h"
#include "extmod/modbluetooth.h"

#include "lib/btstack/src/btstack.h"

#define DEBUG_EVENT_printf(...) //printf(__VA_ARGS__)

#ifndef MICROPY_PY_BLUETOOTH_DEFAULT_GAP_NAME
#define MICROPY_PY_BLUETOOTH_DEFAULT_GAP_NAME "MPY BTSTACK"
#endif

// How long to wait for a controller to init/deinit.
// Some controllers can take up to 5-6 seconds in normal operation.
STATIC const uint32_t BTSTACK_INIT_DEINIT_TIMEOUT_MS = 15000;

// We need to know the attribute handle for the GAP device name (see GAP_DEVICE_NAME_UUID)
// so it can be put into the gatts_db before registering the services, and accessed
// efficiently when requesting an attribute in att_read_callback.  Because this is the
// first characteristic of the first service, it always has a handle value of 3.
STATIC const uint16_t BTSTACK_GAP_DEVICE_NAME_HANDLE = 3;

volatile int mp_bluetooth_btstack_state = MP_BLUETOOTH_BTSTACK_STATE_OFF;

STATIC int btstack_error_to_errno(int err) {
    DEBUG_EVENT_printf("  --> btstack error: %d\n", err);
    if (err == ERROR_CODE_SUCCESS) {
        return 0;
    } else if (err == BTSTACK_ACL_BUFFERS_FULL || err == BTSTACK_MEMORY_ALLOC_FAILED) {
        return MP_ENOMEM;
    } else if (err == GATT_CLIENT_IN_WRONG_STATE) {
        return MP_EALREADY;
    } else if (err == GATT_CLIENT_BUSY) {
        return MP_EBUSY;
    } else if (err == GATT_CLIENT_NOT_CONNECTED) {
        return MP_ENOTCONN;
    } else {
        return MP_EINVAL;
    }
}

STATIC mp_obj_bluetooth_uuid_t create_mp_uuid(uint16_t uuid16, const uint8_t *uuid128) {
    mp_obj_bluetooth_uuid_t result;
    if (uuid16 != 0) {
        result.data[0] = uuid16 & 0xff;
        result.data[1] = (uuid16 >> 8) & 0xff;
        result.type = MP_BLUETOOTH_UUID_TYPE_16;
    } else {
        reverse_128(uuid128, result.data);
        result.type = MP_BLUETOOTH_UUID_TYPE_128;
    }
    return result;
}


// GATTS Notify/Indicate (att_server_notify/indicate)
// * When available, copies buffer immediately.
// * Otherwise fails with BTSTACK_ACL_BUFFERS_FULL
// * Use att_server_request_to_send_notification/indication to get callback
//   * Takes btstack_context_callback_registration_t (and takes ownership) and conn_handle.
//   * Callback is invoked with just the context member of the btstack_context_callback_registration_t

// GATTC Write without response (gatt_client_write_value_of_characteristic_without_response)
// * When available, copies buffer immediately.
// * Otherwise, fails with GATT_CLIENT_BUSY
// * Use gatt_client_request_can_write_without_response_event to get callback
//   * Takes btstack_packet_handler_t (function pointer) and conn_handle
//   * Callback is involved, use gatt_event_can_write_without_response_get_handle to get the conn_handle (no other context)

// GATTC Write with response (gatt_client_write_value_of_characteristic)
// * Always succeeds, takes ownership of buffer
// * Raises GATT_EVENT_QUERY_COMPLETE to the supplied packet handler.

// For notify/indicate/write-without-response that proceed immediately, nothing extra required.
// For all other cases, buffer needs to be copied and protected from GC.
// For notify/indicate:
//  * btstack_context_callback_registration_t:
//     * needs to be malloc'ed
//     * needs to be protected from GC
//     * context arg needs to point back to the callback registration so it can be freed and un-protected
// For write-without-response
//  * only the conn_handle is available in the callback
//  * so we need a queue of conn_handle->(value_handle, copied buffer)

// Pending operation types.
enum {
    // Queued for sending when possible.
    MP_BLUETOOTH_BTSTACK_PENDING_NOTIFY, // Waiting for context callback
    MP_BLUETOOTH_BTSTACK_PENDING_INDICATE, // Waiting for context callback
    MP_BLUETOOTH_BTSTACK_PENDING_WRITE_NO_RESPONSE, // Waiting for conn handle
    // Hold buffer pointer until complete.
    MP_BLUETOOTH_BTSTACK_PENDING_WRITE, // Waiting for write done event
};

// Pending operation:
//  - Holds a GC reference to the copied outgoing buffer.
//  - Provides enough information for the callback handler to execute the desired operation.
struct _mp_btstack_pending_op_t {
    mp_btstack_pending_op_t *next; // Must be first field to match btstack_linked_item.

    uint16_t op_type;
    uint16_t conn_handle;

    // For notify/indicate only.
    btstack_context_callback_registration_t context_registration;

    uint16_t value_handle;
    size_t len;
    uint8_t buf[];
};

STATIC void btstack_notify_indicate_ready_handler(void * context) {
    MICROPY_PY_BLUETOOTH_ENTER
    mp_btstack_pending_op_t *pending_op = (mp_btstack_pending_op_t*)context;
    DEBUG_EVENT_printf("btstack_notify_indicate_ready_handler op_type=%d conn_handle=%d value_handle=%d len=%lu\n", pending_op->op_type, pending_op->conn_handle, pending_op->value_handle, pending_op->len);
    if (pending_op->op_type == MP_BLUETOOTH_BTSTACK_PENDING_NOTIFY) {
        int err = att_server_notify(pending_op->conn_handle, pending_op->value_handle, pending_op->buf, pending_op->len);
        DEBUG_EVENT_printf("btstack_notify_indicate_ready_handler: sending notification err=%d\n", err);
        assert(err == ERROR_CODE_SUCCESS);
        (void)err;
    } else if (pending_op->op_type == MP_BLUETOOTH_BTSTACK_PENDING_INDICATE) {
        int err = att_server_indicate(pending_op->conn_handle, pending_op->value_handle, NULL, 0);
        DEBUG_EVENT_printf("btstack_notify_indicate_ready_handler: sending indication err=%d\n", err);
        assert(err == ERROR_CODE_SUCCESS);
        (void)err;
    } else {
        DEBUG_EVENT_printf("btstack_notify_indicate_ready_handler: wrong type of op\n");
        assert(0);
    }
    assert(btstack_linked_list_remove(&MP_STATE_PORT(bluetooth_btstack_root_pointers)->pending_ops, (btstack_linked_item_t*)pending_op));
    MICROPY_PY_BLUETOOTH_EXIT
}

STATIC mp_btstack_pending_op_t* btstack_enqueue_pending_operation(uint16_t op_type, uint16_t conn_handle, uint16_t value_handle, const uint8_t *buf, size_t len) {
    DEBUG_EVENT_printf("btstack_enqueue_pending_operation op_type=%d conn_handle=%d value_handle=%d len=%lu\n", op_type, conn_handle, value_handle, len);
    mp_btstack_pending_op_t *pending_op = m_malloc(sizeof(mp_btstack_pending_op_t) + len);
    pending_op->op_type = op_type;
    pending_op->conn_handle = conn_handle;
    pending_op->value_handle = value_handle;
    pending_op->len = len;
    memcpy(pending_op->buf, buf, len);

    if (op_type == MP_BLUETOOTH_BTSTACK_PENDING_NOTIFY || op_type == MP_BLUETOOTH_BTSTACK_PENDING_INDICATE) {
        pending_op->context_registration.callback = &btstack_notify_indicate_ready_handler;
        pending_op->context_registration.context = pending_op;
    }

    MICROPY_PY_BLUETOOTH_ENTER
    assert(btstack_linked_list_add(&MP_STATE_PORT(bluetooth_btstack_root_pointers)->pending_ops, (btstack_linked_item_t*)pending_op));
    MICROPY_PY_BLUETOOTH_EXIT

    return pending_op;
}

#if MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE
// Find a pending op of the specified type for this conn_handle (and if specified, value_handle).
// Used by MP_BLUETOOTH_BTSTACK_PENDING_WRITE and MP_BLUETOOTH_BTSTACK_PENDING_WRITE_NO_RESPONSE.
// At the moment, both will set value_handle=0xffff as the events do not know their value_handle.
// TODO: Can we make btstack give us the value_handle for regular write (with response) so that we
// know for sure that we're using the correct entry.
STATIC mp_btstack_pending_op_t* btstack_find_pending_operation(uint16_t op_type, uint16_t conn_handle, uint16_t value_handle) {
    MICROPY_PY_BLUETOOTH_ENTER
    DEBUG_EVENT_printf("btstack_find_pending_operation op_type=%d conn_handle=%d value_handle=%d\n", op_type, conn_handle, value_handle);
    btstack_linked_list_iterator_t it;
    btstack_linked_list_iterator_init(&it, &MP_STATE_PORT(bluetooth_btstack_root_pointers)->pending_ops);
    while (btstack_linked_list_iterator_has_next(&it)){
        mp_btstack_pending_op_t *pending_op = (mp_btstack_pending_op_t*)btstack_linked_list_iterator_next(&it);

        if (pending_op->op_type == op_type && pending_op->conn_handle == conn_handle && (value_handle == 0xffff || pending_op->value_handle == value_handle)) {
            DEBUG_EVENT_printf("btstack_find_pending_operation: found value_handle=%d len=%lu\n", pending_op->value_handle, pending_op->len);
            assert(btstack_linked_list_remove(&MP_STATE_PORT(bluetooth_btstack_root_pointers)->pending_ops, (btstack_linked_item_t*)pending_op));
            MICROPY_PY_BLUETOOTH_EXIT
            return pending_op;
        }
    }
    DEBUG_EVENT_printf("btstack_find_pending_operation: not found\n");
    MICROPY_PY_BLUETOOTH_EXIT
    return NULL;
}
#endif

STATIC void btstack_packet_handler(uint8_t packet_type, uint8_t *packet, uint8_t irq) {
    DEBUG_EVENT_printf("btstack_packet_handler(packet_type=%u, packet=%p)\n", packet_type, packet);
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    uint8_t event_type = hci_event_packet_get_type(packet);
    if (event_type == ATT_EVENT_CONNECTED) {
        DEBUG_EVENT_printf("  --> att connected\n");
    } else if (event_type == ATT_EVENT_DISCONNECTED) {
        DEBUG_EVENT_printf("  --> att disconnected\n");
    } else if (event_type == HCI_EVENT_LE_META) {
        DEBUG_EVENT_printf("  --> hci le meta\n");
        if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
            uint16_t conn_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
            uint8_t addr_type = hci_subevent_le_connection_complete_get_peer_address_type(packet);
            bd_addr_t addr;
            hci_subevent_le_connection_complete_get_peer_address(packet, addr);
            uint16_t irq_event;
            if (hci_subevent_le_connection_complete_get_role(packet) == 0) {
                // Master role.
                irq_event = MP_BLUETOOTH_IRQ_PERIPHERAL_CONNECT;
            } else {
                // Slave role.
                irq_event = MP_BLUETOOTH_IRQ_CENTRAL_CONNECT;
            }
            mp_bluetooth_gap_on_connected_disconnected(irq_event, conn_handle, addr_type, addr);
        }
    } else if (event_type == BTSTACK_EVENT_STATE) {
        uint8_t state = btstack_event_state_get_state(packet);
        DEBUG_EVENT_printf("  --> btstack event state 0x%02x\n", state);
        if (state == HCI_STATE_WORKING) {
            // Signal that initialisation has completed.
            mp_bluetooth_btstack_state = MP_BLUETOOTH_BTSTACK_STATE_ACTIVE;
        } else if (state == HCI_STATE_OFF) {
            // Signal that de-initialisation has completed.
            mp_bluetooth_btstack_state = MP_BLUETOOTH_BTSTACK_STATE_OFF;
        }
    } else if (event_type == HCI_EVENT_TRANSPORT_PACKET_SENT) {
        DEBUG_EVENT_printf("  --> hci transport packet set\n");
    } else if (event_type == HCI_EVENT_COMMAND_COMPLETE) {
        DEBUG_EVENT_printf("  --> hci command complete\n");
    } else if (event_type == HCI_EVENT_COMMAND_STATUS) {
        DEBUG_EVENT_printf("  --> hci command status\n");
    } else if (event_type == HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS) {
        DEBUG_EVENT_printf("  --> hci number of completed packets\n");
    } else if (event_type == BTSTACK_EVENT_NR_CONNECTIONS_CHANGED) {
        DEBUG_EVENT_printf("  --> btstack # conns changed\n");
    } else if (event_type == HCI_EVENT_VENDOR_SPECIFIC) {
        DEBUG_EVENT_printf("  --> hci vendor specific\n");
    } else if (event_type == GAP_EVENT_ADVERTISING_REPORT) {
        DEBUG_EVENT_printf("  --> gap advertising report\n");
        bd_addr_t address;
        gap_event_advertising_report_get_address(packet, address);
        uint8_t adv_event_type = gap_event_advertising_report_get_advertising_event_type(packet);
        uint8_t address_type = gap_event_advertising_report_get_address_type(packet);
        int8_t rssi = gap_event_advertising_report_get_rssi(packet);
        uint8_t length = gap_event_advertising_report_get_data_length(packet);
        const uint8_t *data = gap_event_advertising_report_get_data(packet);
        mp_bluetooth_gap_on_scan_result(address_type, address, adv_event_type, rssi, data, length);
    } else if (event_type == HCI_EVENT_DISCONNECTION_COMPLETE) {
        DEBUG_EVENT_printf("  --> hci disconnect complete\n");
        uint16_t conn_handle = hci_event_disconnection_complete_get_connection_handle(packet);
        const hci_connection_t *conn = hci_connection_for_handle(conn_handle);
        uint16_t irq_event;
        if (conn == NULL || conn->role == 0) {
            // Master role.
            irq_event = MP_BLUETOOTH_IRQ_PERIPHERAL_DISCONNECT;
        } else {
            // Slave role.
            irq_event = MP_BLUETOOTH_IRQ_CENTRAL_DISCONNECT;
        }
        uint8_t addr[6] = {0};
        mp_bluetooth_gap_on_connected_disconnected(irq_event, conn_handle, 0xff, addr);
    #if MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE
    } else if (event_type == GATT_EVENT_QUERY_COMPLETE) {
        uint16_t conn_handle = gatt_event_query_complete_get_handle(packet);
        uint16_t status = gatt_event_query_complete_get_att_status(packet);
        DEBUG_EVENT_printf("  --> gatt query complete irq=%d conn_handle=%d status=%d\n", irq, conn_handle, status);
        if (irq == MP_BLUETOOTH_IRQ_GATTC_READ_DONE || irq == MP_BLUETOOTH_IRQ_GATTC_WRITE_DONE) {
            // TODO there is no value_handle available to pass here.
            // TODO try and get this implemented in btstack.
            mp_bluetooth_gattc_on_read_write_status(irq, conn_handle, 0xffff, status);
            // Unref the saved buffer for the write operation on this conn_handle.
            assert(btstack_find_pending_operation(MP_BLUETOOTH_BTSTACK_PENDING_WRITE, conn_handle, 0xffff));
        } else if (irq == MP_BLUETOOTH_IRQ_GATTC_SERVICE_DONE ||
                   irq == MP_BLUETOOTH_IRQ_GATTC_CHARACTERISTIC_DONE ||
                   irq == MP_BLUETOOTH_IRQ_GATTC_DESCRIPTOR_DONE) {
            mp_bluetooth_gattc_on_discover_complete(irq, conn_handle, status);
        }
    } else if (event_type == GATT_EVENT_SERVICE_QUERY_RESULT) {
        DEBUG_EVENT_printf("  --> gatt service query result\n");
        uint16_t conn_handle = gatt_event_service_query_result_get_handle(packet);
        gatt_client_service_t service;
        gatt_event_service_query_result_get_service(packet, &service);
        mp_obj_bluetooth_uuid_t service_uuid = create_mp_uuid(service.uuid16, service.uuid128);
        mp_bluetooth_gattc_on_primary_service_result(conn_handle, service.start_group_handle, service.end_group_handle, &service_uuid);
    } else if (event_type == GATT_EVENT_CHARACTERISTIC_QUERY_RESULT) {
        DEBUG_EVENT_printf("  --> gatt characteristic query result\n");
        uint16_t conn_handle = gatt_event_characteristic_query_result_get_handle(packet);
        gatt_client_characteristic_t characteristic;
        gatt_event_characteristic_query_result_get_characteristic(packet, &characteristic);
        mp_obj_bluetooth_uuid_t characteristic_uuid = create_mp_uuid(characteristic.uuid16, characteristic.uuid128);
        mp_bluetooth_gattc_on_characteristic_result(conn_handle, characteristic.start_handle, characteristic.value_handle, characteristic.properties, &characteristic_uuid);
    } else if (event_type == GATT_EVENT_CHARACTERISTIC_DESCRIPTOR_QUERY_RESULT) {
        DEBUG_EVENT_printf("  --> gatt descriptor query result\n");
        uint16_t conn_handle = gatt_event_all_characteristic_descriptors_query_result_get_handle(packet);
        gatt_client_characteristic_descriptor_t descriptor;
        gatt_event_all_characteristic_descriptors_query_result_get_characteristic_descriptor(packet, &descriptor);
        mp_obj_bluetooth_uuid_t descriptor_uuid = create_mp_uuid(descriptor.uuid16, descriptor.uuid128);
        mp_bluetooth_gattc_on_descriptor_result(conn_handle, descriptor.handle, &descriptor_uuid);
    } else if (event_type == GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT) {
        DEBUG_EVENT_printf("  --> gatt characteristic value query result\n");
        uint16_t conn_handle = gatt_event_characteristic_value_query_result_get_handle(packet);
        uint16_t value_handle = gatt_event_characteristic_value_query_result_get_value_handle(packet);
        uint16_t len = gatt_event_characteristic_value_query_result_get_value_length(packet);
        const uint8_t *data = gatt_event_characteristic_value_query_result_get_value(packet);
        mp_uint_t atomic_state;
        len = mp_bluetooth_gattc_on_data_available_start(MP_BLUETOOTH_IRQ_GATTC_READ_RESULT, conn_handle, value_handle, len, &atomic_state);
        mp_bluetooth_gattc_on_data_available_chunk(data, len);
        mp_bluetooth_gattc_on_data_available_end(atomic_state);
    } else if (event_type == GATT_EVENT_NOTIFICATION) {
        DEBUG_EVENT_printf("  --> gatt notification\n");
        uint16_t conn_handle = gatt_event_notification_get_handle(packet);
        uint16_t value_handle = gatt_event_notification_get_value_handle(packet);
        uint16_t len = gatt_event_notification_get_value_length(packet);
        const uint8_t *data = gatt_event_notification_get_value(packet);
        mp_uint_t atomic_state;
        len = mp_bluetooth_gattc_on_data_available_start(MP_BLUETOOTH_IRQ_GATTC_NOTIFY, conn_handle, value_handle, len, &atomic_state);
        mp_bluetooth_gattc_on_data_available_chunk(data, len);
        mp_bluetooth_gattc_on_data_available_end(atomic_state);
    } else if (event_type == GATT_EVENT_INDICATION) {
        DEBUG_EVENT_printf("  --> gatt indication\n");
        uint16_t conn_handle = gatt_event_indication_get_handle(packet);
        uint16_t value_handle = gatt_event_indication_get_value_handle(packet);
        uint16_t len = gatt_event_indication_get_value_length(packet);
        const uint8_t *data = gatt_event_indication_get_value(packet);
        mp_uint_t atomic_state;
        len = mp_bluetooth_gattc_on_data_available_start(MP_BLUETOOTH_IRQ_GATTC_INDICATE, conn_handle, value_handle, len, &atomic_state);
        mp_bluetooth_gattc_on_data_available_chunk(data, len);
        mp_bluetooth_gattc_on_data_available_end(atomic_state);
    } else if (event_type == GATT_EVENT_CAN_WRITE_WITHOUT_RESPONSE) {
        uint16_t conn_handle = gatt_event_can_write_without_response_get_handle(packet);
        DEBUG_EVENT_printf("  --> gatt can write without response %d\n", conn_handle);
        mp_btstack_pending_op_t *pending_op = btstack_find_pending_operation(MP_BLUETOOTH_BTSTACK_PENDING_WRITE_NO_RESPONSE, conn_handle, 0xffff);
        if (pending_op) {
            DEBUG_EVENT_printf("  --> ready for value_handle=%d len=%lu\n", pending_op->value_handle, pending_op->len);
            gatt_client_write_value_of_characteristic_without_response(pending_op->conn_handle, pending_op->value_handle, pending_op->len, (uint8_t *)pending_op->buf);
        }

    #endif // MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE
    } else {
        DEBUG_EVENT_printf("  --> hci event type: unknown (0x%02x)\n", event_type);
    }
}

// Because the packet handler callbacks don't support an argument, we use a specific
// handler when we need to provide additional state to the handler (in the "irq" parameter).
// This is the generic handler for when you don't need extra state.
STATIC void btstack_packet_handler_generic(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    btstack_packet_handler(packet_type, packet, 0);
}

STATIC btstack_packet_callback_registration_t hci_event_callback_registration = {
    .callback = &btstack_packet_handler_generic
};

#if MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE
// For when the handler is being used for service discovery.
STATIC void btstack_packet_handler_discover_services(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    btstack_packet_handler(packet_type, packet, MP_BLUETOOTH_IRQ_GATTC_SERVICE_DONE);
}

// For when the handler is being used for characteristic discovery.
STATIC void btstack_packet_handler_discover_characteristics(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    btstack_packet_handler(packet_type, packet, MP_BLUETOOTH_IRQ_GATTC_CHARACTERISTIC_DONE);
}

// For when the handler is being used for descriptor discovery.
STATIC void btstack_packet_handler_discover_descriptors(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    btstack_packet_handler(packet_type, packet, MP_BLUETOOTH_IRQ_GATTC_DESCRIPTOR_DONE);
}

// For when the handler is being used for a read query.
STATIC void btstack_packet_handler_read(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    btstack_packet_handler(packet_type, packet, MP_BLUETOOTH_IRQ_GATTC_READ_DONE);
}

// For when the handler is being used for write-with-response.
STATIC void btstack_packet_handler_write_with_response(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    btstack_packet_handler(packet_type, packet, MP_BLUETOOTH_IRQ_GATTC_WRITE_DONE);
}
#endif // MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE


#define FIXED_PASSKEY 12346

static void btstack_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            printf("Just works requested\n");
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            printf("Confirming numeric comparison: %u\n", sm_event_numeric_comparison_request_get_passkey(packet));
            sm_numeric_comparison_confirm(sm_event_passkey_display_number_get_handle(packet));
            break;
        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            printf("Display Passkey: %u\n", sm_event_passkey_display_number_get_passkey(packet));
            break;
        case SM_EVENT_PASSKEY_INPUT_NUMBER:
            printf("Passkey Input requested\n");
            printf("Sending fixed passkey %u\n", FIXED_PASSKEY);
            sm_passkey_input(sm_event_passkey_input_number_get_handle(packet), FIXED_PASSKEY);
            break;
        case SM_EVENT_PAIRING_COMPLETE:
            switch (sm_event_pairing_complete_get_status(packet)){
                case ERROR_CODE_SUCCESS:
                    printf("Pairing complete, success\n");
                    break;
                case ERROR_CODE_CONNECTION_TIMEOUT:
                    printf("Pairing failed, timeout\n");
                    break;
                case ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION:
                    printf("Pairing faileed, disconnected\n");
                    break;
                case ERROR_CODE_AUTHENTICATION_FAILURE:
                    printf("Pairing failed, reason = %u\n", sm_event_pairing_complete_get_reason(packet));
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

STATIC btstack_packet_callback_registration_t sm_event_callback_registration = {
    .callback = &btstack_sm_packet_handler
};

STATIC btstack_timer_source_t btstack_init_deinit_timeout;

STATIC void btstack_init_deinit_timeout_handler(btstack_timer_source_t *ds) {
    (void)ds;

    // Stop waiting for initialisation.
    // This signals both the loops in mp_bluetooth_init and mp_bluetooth_deinit,
    // as well as ports that run a polling loop.
    mp_bluetooth_btstack_state = MP_BLUETOOTH_BTSTACK_STATE_TIMEOUT;
}

int mp_bluetooth_init(void) {
    DEBUG_EVENT_printf("mp_bluetooth_init\n");

    if (mp_bluetooth_btstack_state == MP_BLUETOOTH_BTSTACK_STATE_ACTIVE) {
        return 0;
    }

    // Clean up if necessary.
    mp_bluetooth_deinit();

    btstack_memory_init();

    MP_STATE_PORT(bluetooth_btstack_root_pointers) = m_new0(mp_bluetooth_btstack_root_pointers_t, 1);
    mp_bluetooth_gatts_db_create(&MP_STATE_PORT(bluetooth_btstack_root_pointers)->gatts_db);

    // Set the default GAP device name.
    const char *gap_name = MICROPY_PY_BLUETOOTH_DEFAULT_GAP_NAME;
    size_t gap_len = strlen(gap_name);
    mp_bluetooth_gatts_db_create_entry(MP_STATE_PORT(bluetooth_btstack_root_pointers)->gatts_db, BTSTACK_GAP_DEVICE_NAME_HANDLE, gap_len);
    mp_bluetooth_gap_set_device_name((const uint8_t *)gap_name, gap_len);

    mp_bluetooth_btstack_port_init();
    mp_bluetooth_btstack_state = MP_BLUETOOTH_BTSTACK_STATE_STARTING;

    l2cap_init();
    le_device_db_init();

    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);

    // TODO handle this correctly.
    sm_key_t dummy_er_key = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    sm_set_er(dummy_er_key);
    sm_key_t dummy_ir_key = {0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    sm_set_ir(dummy_ir_key);

    #if MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE
    gatt_client_init();
    #endif

    // Register for HCI events.
    hci_add_event_handler(&hci_event_callback_registration);

    // Register for Security Manager events.
    sm_add_event_handler(&sm_event_callback_registration);

    // Set a timeout for HCI initialisation.
    btstack_run_loop_set_timer(&btstack_init_deinit_timeout, BTSTACK_INIT_DEINIT_TIMEOUT_MS);
    btstack_run_loop_set_timer_handler(&btstack_init_deinit_timeout, btstack_init_deinit_timeout_handler);
    btstack_run_loop_add_timer(&btstack_init_deinit_timeout);

    // Either the HCI event will set state to ACTIVE, or the timeout will set it to TIMEOUT.
    mp_bluetooth_btstack_port_start();
    while (mp_bluetooth_btstack_state == MP_BLUETOOTH_BTSTACK_STATE_STARTING) {
        MICROPY_EVENT_POLL_HOOK
    }
    btstack_run_loop_remove_timer(&btstack_init_deinit_timeout);

    // Check for timeout.
    if (mp_bluetooth_btstack_state != MP_BLUETOOTH_BTSTACK_STATE_ACTIVE) {
        // Required to stop the polling loop.
        mp_bluetooth_btstack_state = MP_BLUETOOTH_BTSTACK_STATE_OFF;
        // Attempt a shutdown (may not do anything).
        mp_bluetooth_btstack_port_deinit();

        // Clean up.
        MP_STATE_PORT(bluetooth_btstack_root_pointers) = NULL;
        return MP_ETIMEDOUT;
    }

    #if MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE
    // Enable GATT_EVENT_NOTIFICATION/GATT_EVENT_INDICATION for all connections and handles.
    gatt_client_listen_for_characteristic_value_updates(&MP_STATE_PORT(bluetooth_btstack_root_pointers)->notification, &btstack_packet_handler_generic, GATT_CLIENT_ANY_CONNECTION, NULL);
    #endif

    return 0;
}

void mp_bluetooth_deinit(void) {
    DEBUG_EVENT_printf("mp_bluetooth_deinit\n");

    // Nothing to do if not initialised.
    if (!MP_STATE_PORT(bluetooth_btstack_root_pointers)) {
        return;
    }

    mp_bluetooth_gap_advertise_stop();

    #if MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE
    // Remove our registration for notify/indicate.
    gatt_client_stop_listening_for_characteristic_value_updates(&MP_STATE_PORT(bluetooth_btstack_root_pointers)->notification);
    #endif

    // Set a timer that will forcibly set the state to TIMEOUT, which will stop the loop below.
    btstack_run_loop_set_timer(&btstack_init_deinit_timeout, BTSTACK_INIT_DEINIT_TIMEOUT_MS);
    btstack_run_loop_add_timer(&btstack_init_deinit_timeout);

    // This should result in a clean shutdown, which will set the state to OFF.
    // On Unix this is blocking (it joins on the poll thread), on other ports the loop below will wait unil
    // either timeout or clean shutdown.
    mp_bluetooth_btstack_port_deinit();
    while (mp_bluetooth_btstack_state == MP_BLUETOOTH_BTSTACK_STATE_ACTIVE) {
        MICROPY_EVENT_POLL_HOOK
    }
    btstack_run_loop_remove_timer(&btstack_init_deinit_timeout);

    mp_bluetooth_btstack_state = MP_BLUETOOTH_BTSTACK_STATE_OFF;
    MP_STATE_PORT(bluetooth_btstack_root_pointers) = NULL;
}

bool mp_bluetooth_is_active(void) {
    return mp_bluetooth_btstack_state == MP_BLUETOOTH_BTSTACK_STATE_ACTIVE;
}

void mp_bluetooth_get_device_addr(uint8_t *addr) {
    mp_hal_get_mac(MP_HAL_MAC_BDADDR, addr);
}

size_t mp_bluetooth_gap_get_device_name(const uint8_t **buf) {
    uint8_t *value = NULL;
    size_t value_len = 0;
    mp_bluetooth_gatts_db_read(MP_STATE_PORT(bluetooth_btstack_root_pointers)->gatts_db, BTSTACK_GAP_DEVICE_NAME_HANDLE, &value, &value_len);
    *buf = value;
    return value_len;
}

int mp_bluetooth_gap_set_device_name(const uint8_t *buf, size_t len) {
    mp_bluetooth_gatts_db_write(MP_STATE_PORT(bluetooth_btstack_root_pointers)->gatts_db, BTSTACK_GAP_DEVICE_NAME_HANDLE, buf, len);
    return 0;
}

int mp_bluetooth_gap_advertise_start(bool connectable, int32_t interval_us, const uint8_t *adv_data, size_t adv_data_len, const uint8_t *sr_data, size_t sr_data_len) {
    DEBUG_EVENT_printf("mp_bluetooth_gap_advertise_start\n");
    uint16_t adv_int_min = interval_us / 625;
    uint16_t adv_int_max = interval_us / 625;
    uint8_t adv_type = connectable ? 0 : 2;
    bd_addr_t null_addr = {0};

    uint8_t direct_address_type = 0;
    uint8_t channel_map = 0x07; // Use all three broadcast channels.
    uint8_t filter_policy = 0x00; // None.

    gap_advertisements_set_params(adv_int_min, adv_int_max, adv_type, direct_address_type, null_addr, channel_map, filter_policy);

    // Copy the adv_data and sr_data into a persistent buffer (which is findable via the btstack root pointers).
    size_t total_bytes = adv_data_len + sr_data_len;
    if (total_bytes > MP_STATE_PORT(bluetooth_btstack_root_pointers)->adv_data_alloc) {
        // Resize if necessary.
        MP_STATE_PORT(bluetooth_btstack_root_pointers)->adv_data = m_new(uint8_t, total_bytes);
        MP_STATE_PORT(bluetooth_btstack_root_pointers)->adv_data_alloc = total_bytes;
    }
    uint8_t *data = MP_STATE_PORT(bluetooth_btstack_root_pointers)->adv_data;

    if (adv_data) {
        memcpy(data, (uint8_t *)adv_data, adv_data_len);
        gap_advertisements_set_data(adv_data_len, data);
        data += adv_data_len;
    }
    if (sr_data) {
        memcpy(data, (uint8_t *)sr_data, sr_data_len);
        gap_scan_response_set_data(sr_data_len, data);
    }

    gap_advertisements_enable(true);
    return 0;
}

void mp_bluetooth_gap_advertise_stop(void) {
    DEBUG_EVENT_printf("mp_bluetooth_gap_advertise_stop\n");
    gap_advertisements_enable(false);
    MP_STATE_PORT(bluetooth_btstack_root_pointers)->adv_data_alloc = 0;
    MP_STATE_PORT(bluetooth_btstack_root_pointers)->adv_data = NULL;
}

int mp_bluetooth_gatts_register_service_begin(bool append) {
    DEBUG_EVENT_printf("mp_bluetooth_gatts_register_service_begin\n");
    if (!append) {
        // This will reset the DB.
        // Becase the DB is statically allocated, there's no problem with just re-initing it.
        // Note this would be a memory leak if we enabled HAVE_MALLOC (there's no API to free the existing db).
        att_db_util_init();

        att_db_util_add_service_uuid16(GAP_SERVICE_UUID);
        uint16_t handle = att_db_util_add_characteristic_uuid16(GAP_DEVICE_NAME_UUID, ATT_PROPERTY_READ | ATT_PROPERTY_DYNAMIC, ATT_SECURITY_NONE, ATT_SECURITY_NONE, NULL, 0);
        assert(handle == BTSTACK_GAP_DEVICE_NAME_HANDLE);
        (void)handle;

        att_db_util_add_service_uuid16(0x1801);
        att_db_util_add_characteristic_uuid16(0x2a05, ATT_PROPERTY_READ, ATT_SECURITY_NONE, ATT_SECURITY_NONE, NULL, 0);
    }

    return 0;
}

STATIC uint16_t att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t *buffer, uint16_t buffer_size) {
    (void)connection_handle;
    DEBUG_EVENT_printf("btstack: att_read_callback (handle: %u, offset: %u, buffer: %p, size: %u)\n", att_handle, offset, buffer, buffer_size);
    mp_bluetooth_gatts_db_entry_t *entry = mp_bluetooth_gatts_db_lookup(MP_STATE_PORT(bluetooth_btstack_root_pointers)->gatts_db, att_handle);
    if (!entry) {
        DEBUG_EVENT_printf("btstack: att_read_callback handle not found\n");
        return 0; // TODO: Find status code for not-found.
    }

    return att_read_callback_handle_blob(entry->data, entry->data_len, offset, buffer, buffer_size);
}

STATIC int att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size) {
    (void)offset;
    (void)transaction_mode;
    DEBUG_EVENT_printf("btstack: att_write_callback (handle: %u, mode: %u, offset: %u, buffer: %p, size: %u)\n", att_handle, transaction_mode, offset, buffer, buffer_size);
    mp_bluetooth_gatts_db_entry_t *entry = mp_bluetooth_gatts_db_lookup(MP_STATE_PORT(bluetooth_btstack_root_pointers)->gatts_db, att_handle);
    if (!entry) {
        DEBUG_EVENT_printf("btstack: att_write_callback handle not found\n");
        return 0; // TODO: Find status code for not-found.
    }

    // TODO: Use `offset` arg.
    size_t append_offset = 0;
    if (entry->append) {
        append_offset = entry->data_len;
    }
    entry->data_len = MIN(entry->data_alloc, buffer_size + append_offset);
    memcpy(entry->data + append_offset, buffer, entry->data_len - append_offset);

    mp_bluetooth_gatts_on_write(connection_handle, att_handle);

    return 0;
}

STATIC inline uint16_t get_uuid16(const mp_obj_bluetooth_uuid_t *uuid) {
    return (uuid->data[1] << 8) | uuid->data[0];
}

int mp_bluetooth_gatts_register_service(mp_obj_bluetooth_uuid_t *service_uuid, mp_obj_bluetooth_uuid_t **characteristic_uuids, uint16_t *characteristic_flags, mp_obj_bluetooth_uuid_t **descriptor_uuids, uint16_t *descriptor_flags, uint8_t *num_descriptors, uint16_t *handles, size_t num_characteristics) {
    DEBUG_EVENT_printf("mp_bluetooth_gatts_register_service\n");
    // Note: btstack expects BE UUIDs (which it immediately convertes to LE).
    // So we have to convert all our modbluetooth LE UUIDs to BE just for the att_db_util_add_* methods (using get_uuid16 above, and reverse_128 from btstackutil.h).

    // TODO: btstack's att_db_util_add_* methods have no bounds checking or validation.
    // Need some way to prevent additional services being added if we're out of space in the static buffer.

    if (service_uuid->type == MP_BLUETOOTH_UUID_TYPE_16) {
        att_db_util_add_service_uuid16(get_uuid16(service_uuid));
    } else if (service_uuid->type == MP_BLUETOOTH_UUID_TYPE_128) {
        uint8_t buffer[16];
        reverse_128(service_uuid->data, buffer);
        att_db_util_add_service_uuid128(buffer);
    } else {
        return MP_EINVAL;
    }

    size_t handle_index = 0;
    size_t descriptor_index = 0;
    static uint8_t cccb_buf[2] = {0};

    for (size_t i = 0; i < num_characteristics; ++i) {
        uint16_t props = characteristic_flags[i] | ATT_PROPERTY_DYNAMIC;
        uint16_t read_permission = (characteristic_flags[i]  & 0x0400) ? ATT_SECURITY_AUTHENTICATED : ATT_SECURITY_NONE;
        uint16_t write_permission = (characteristic_flags[i] & 0x2000) ? ATT_SECURITY_ENCRYPTED : ATT_SECURITY_NONE;
        if (characteristic_uuids[i]->type == MP_BLUETOOTH_UUID_TYPE_16) {
            handles[handle_index] = att_db_util_add_characteristic_uuid16(get_uuid16(characteristic_uuids[i]), props, read_permission, write_permission, NULL, 0);
        } else if (characteristic_uuids[i]->type == MP_BLUETOOTH_UUID_TYPE_128) {
            uint8_t buffer[16];
            reverse_128(characteristic_uuids[i]->data, buffer);
            handles[handle_index] = att_db_util_add_characteristic_uuid128(buffer, props, read_permission, write_permission, NULL, 0);
        } else {
            return MP_EINVAL;
        }
        mp_bluetooth_gatts_db_create_entry(MP_STATE_PORT(bluetooth_btstack_root_pointers)->gatts_db, handles[handle_index], MP_BLUETOOTH_DEFAULT_ATTR_LEN);
        // If a NOTIFY or INDICATE characteristic is added, then we need to manage a value for the CCCB.
        if (props & (ATT_PROPERTY_NOTIFY | ATT_PROPERTY_INDICATE)) {
            // btstack creates the CCCB as the next handle.
            mp_bluetooth_gatts_db_create_entry(MP_STATE_PORT(bluetooth_btstack_root_pointers)->gatts_db, handles[handle_index] + 1, MP_BLUETOOTH_CCCB_LEN);
            mp_bluetooth_gatts_db_write(MP_STATE_PORT(bluetooth_btstack_root_pointers)->gatts_db, handles[handle_index] + 1, cccb_buf, sizeof(cccb_buf));
        }
        DEBUG_EVENT_printf("Registered char with handle %u\n", handles[handle_index]);
        ++handle_index;

        for (size_t j = 0; j < num_descriptors[i]; ++j) {
            props = descriptor_flags[descriptor_index] | ATT_PROPERTY_DYNAMIC;
            read_permission = ATT_SECURITY_NONE;
            write_permission = ATT_SECURITY_NONE;

            if (descriptor_uuids[descriptor_index]->type == MP_BLUETOOTH_UUID_TYPE_16) {
                handles[handle_index] = att_db_util_add_descriptor_uuid16(get_uuid16(descriptor_uuids[descriptor_index]), props, read_permission, write_permission, NULL, 0);
            } else if (descriptor_uuids[descriptor_index]->type == MP_BLUETOOTH_UUID_TYPE_128) {
                uint8_t buffer[16];
                reverse_128(descriptor_uuids[descriptor_index]->data, buffer);
                handles[handle_index] = att_db_util_add_descriptor_uuid128(buffer, props, read_permission, write_permission, NULL, 0);
            } else {
                return MP_EINVAL;
            }
            mp_bluetooth_gatts_db_create_entry(MP_STATE_PORT(bluetooth_btstack_root_pointers)->gatts_db, handles[handle_index], MP_BLUETOOTH_DEFAULT_ATTR_LEN);
            DEBUG_EVENT_printf("Registered desc with handle %u\n", handles[handle_index]);
            ++descriptor_index;
            ++handle_index;
        }
    }

    return 0;
}

int mp_bluetooth_gatts_register_service_end(void) {
    DEBUG_EVENT_printf("mp_bluetooth_gatts_register_service_end\n");
    att_server_init(att_db_util_get_address(), &att_read_callback, &att_write_callback);
    return 0;
}

int mp_bluetooth_gatts_read(uint16_t value_handle, uint8_t **value, size_t *value_len) {
    DEBUG_EVENT_printf("mp_bluetooth_gatts_read\n");
    return mp_bluetooth_gatts_db_read(MP_STATE_PORT(bluetooth_btstack_root_pointers)->gatts_db, value_handle, value, value_len);
}

int mp_bluetooth_gatts_write(uint16_t value_handle, const uint8_t *value, size_t value_len) {
    DEBUG_EVENT_printf("mp_bluetooth_gatts_write\n");
    return mp_bluetooth_gatts_db_write(MP_STATE_PORT(bluetooth_btstack_root_pointers)->gatts_db, value_handle, value, value_len);
}

int mp_bluetooth_gatts_notify(uint16_t conn_handle, uint16_t value_handle) {
    DEBUG_EVENT_printf("mp_bluetooth_gatts_notify\n");
    // Note: btstack doesn't appear to support sending a notification without a value, so include the stored value.
    uint8_t *data = NULL;
    size_t len = 0;
    mp_bluetooth_gatts_db_read(MP_STATE_PORT(bluetooth_btstack_root_pointers)->gatts_db, value_handle, &data, &len);
    return mp_bluetooth_gatts_notify_send(conn_handle, value_handle, data, &len);
}

int mp_bluetooth_gatts_notify_send(uint16_t conn_handle, uint16_t value_handle, const uint8_t *value, size_t *value_len) {
    DEBUG_EVENT_printf("mp_bluetooth_gatts_notify_send\n");

    // Attempt to send immediately, will copy buffer.
    MICROPY_PY_BLUETOOTH_ENTER
    int err = att_server_notify(conn_handle, value_handle, value, *value_len);
    MICROPY_PY_BLUETOOTH_EXIT

    if (err == BTSTACK_ACL_BUFFERS_FULL) {
        DEBUG_EVENT_printf("mp_bluetooth_gatts_notify_send: ACL buffer full, scheduling callback\n");
        // Schedule callback, making a copy of the buffer.
        mp_btstack_pending_op_t *pending_op = btstack_enqueue_pending_operation(MP_BLUETOOTH_BTSTACK_PENDING_NOTIFY, conn_handle, value_handle, value, *value_len);

        att_server_request_to_send_notification(&pending_op->context_registration, conn_handle);
        // We don't know how many bytes will be sent.
        *value_len = 0;

        return 0;
    } else {
        return btstack_error_to_errno(err);
    }
}

int mp_bluetooth_gatts_indicate(uint16_t conn_handle, uint16_t value_handle) {
    DEBUG_EVENT_printf("mp_bluetooth_gatts_indicate\n");

    // Attempt to send immediately, will copy buffer.
    MICROPY_PY_BLUETOOTH_ENTER
    int err = att_server_indicate(conn_handle, value_handle, NULL, 0);
    MICROPY_PY_BLUETOOTH_EXIT

    if (err == BTSTACK_ACL_BUFFERS_FULL) {
        DEBUG_EVENT_printf("mp_bluetooth_gatts_indicate: ACL buffer full, scheduling callback\n");
        // Schedule callback, making a copy of the buffer.
        mp_btstack_pending_op_t *pending_op = btstack_enqueue_pending_operation(MP_BLUETOOTH_BTSTACK_PENDING_INDICATE, conn_handle, value_handle, NULL, 0);
        att_server_request_to_send_indication(&pending_op->context_registration, conn_handle);
        return 0;
    } else {
        return btstack_error_to_errno(err);
    }
}

int mp_bluetooth_gatts_set_buffer(uint16_t value_handle, size_t len, bool append) {
    DEBUG_EVENT_printf("mp_bluetooth_gatts_set_buffer\n");
    return mp_bluetooth_gatts_db_resize(MP_STATE_PORT(bluetooth_btstack_root_pointers)->gatts_db, value_handle, len, append);
}

int mp_bluetooth_gap_disconnect(uint16_t conn_handle) {
    DEBUG_EVENT_printf("mp_bluetooth_gap_disconnect\n");
    gap_disconnect(conn_handle);
    return 0;
}

#if MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE
STATIC btstack_timer_source_t scan_duration_timeout;

STATIC void hci_initialization_timeout_handler(btstack_timer_source_t *ds) {
    (void)ds;
    mp_bluetooth_gap_scan_stop();
}

int mp_bluetooth_gap_scan_start(int32_t duration_ms, int32_t interval_us, int32_t window_us) {
    DEBUG_EVENT_printf("mp_bluetooth_gap_scan_start\n");

    btstack_run_loop_set_timer(&scan_duration_timeout, duration_ms);
    btstack_run_loop_set_timer_handler(&scan_duration_timeout, hci_initialization_timeout_handler);
    btstack_run_loop_add_timer(&scan_duration_timeout);

    // 0 = passive scan (we don't handle scan response).
    gap_set_scan_parameters(0, interval_us / 625, window_us / 625);
    gap_start_scan();

    return 0;
}

int mp_bluetooth_gap_scan_stop(void) {
    DEBUG_EVENT_printf("mp_bluetooth_gap_scan_stop\n");
    btstack_run_loop_remove_timer(&scan_duration_timeout);
    gap_stop_scan();
    mp_bluetooth_gap_on_scan_complete();
    return 0;
}

int mp_bluetooth_gap_peripheral_connect(uint8_t addr_type, const uint8_t *addr, int32_t duration_ms) {
    DEBUG_EVENT_printf("mp_bluetooth_gap_peripheral_connect\n");

    uint16_t conn_scan_interval = 60000 / 625;
    uint16_t conn_scan_window = 30000 / 625;
    uint16_t conn_interval_min = 10000 / 1250;
    uint16_t conn_interval_max = 30000 / 1250;
    uint16_t conn_latency = 4;
    uint16_t supervision_timeout = duration_ms / 10; // default = 720
    uint16_t min_ce_length = 10000 / 625;
    uint16_t max_ce_length = 30000 / 625;

    gap_set_connection_parameters(conn_scan_interval, conn_scan_window, conn_interval_min, conn_interval_max, conn_latency, supervision_timeout, min_ce_length, max_ce_length);

    bd_addr_t btstack_addr;
    memcpy(btstack_addr, addr, BD_ADDR_LEN);
    return btstack_error_to_errno(gap_connect(btstack_addr, addr_type));
}

int mp_bluetooth_gattc_discover_primary_services(uint16_t conn_handle, const mp_obj_bluetooth_uuid_t *uuid) {
    DEBUG_EVENT_printf("mp_bluetooth_gattc_discover_primary_services\n");
    uint8_t err;
    if (uuid) {
        if (uuid->type == MP_BLUETOOTH_UUID_TYPE_16) {
            err = gatt_client_discover_primary_services_by_uuid16(&btstack_packet_handler_discover_services, conn_handle, get_uuid16(uuid));
        } else if (uuid->type == MP_BLUETOOTH_UUID_TYPE_128) {
            uint8_t buffer[16];
            reverse_128(uuid->data, buffer);
            err = gatt_client_discover_primary_services_by_uuid128(&btstack_packet_handler_discover_services, conn_handle, buffer);
        } else {
            DEBUG_EVENT_printf("  --> unknown UUID size\n");
            return MP_EINVAL;
        }
    } else {
        err = gatt_client_discover_primary_services(&btstack_packet_handler_discover_services, conn_handle);
    }
    return btstack_error_to_errno(err);
}

int mp_bluetooth_gattc_discover_characteristics(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle, const mp_obj_bluetooth_uuid_t *uuid) {
    DEBUG_EVENT_printf("mp_bluetooth_gattc_discover_characteristics\n");
    gatt_client_service_t service = {
        // Only start/end handles needed for gatt_client_discover_characteristics_for_service.
        .start_group_handle = start_handle,
        .end_group_handle = end_handle,
        .uuid16 = 0,
        .uuid128 = {0},
    };
    uint8_t err;
    if (uuid) {
        if (uuid->type == MP_BLUETOOTH_UUID_TYPE_16) {
            err = gatt_client_discover_characteristics_for_service_by_uuid16(&btstack_packet_handler_discover_characteristics, conn_handle, &service, get_uuid16(uuid));
        } else if (uuid->type == MP_BLUETOOTH_UUID_TYPE_128) {
            uint8_t buffer[16];
            reverse_128(uuid->data, buffer);
            err = gatt_client_discover_characteristics_for_service_by_uuid128(&btstack_packet_handler_discover_characteristics, conn_handle, &service, buffer);
        } else {
            DEBUG_EVENT_printf("  --> unknown UUID size\n");
            return MP_EINVAL;
        }
    } else {
        err = gatt_client_discover_characteristics_for_service(&btstack_packet_handler_discover_characteristics, conn_handle, &service);
    }
    return btstack_error_to_errno(err);
}

int mp_bluetooth_gattc_discover_descriptors(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle) {
    DEBUG_EVENT_printf("mp_bluetooth_gattc_discover_descriptors\n");
    gatt_client_characteristic_t characteristic = {
        // Only start/end handles needed for gatt_client_discover_characteristic_descriptors.
        .start_handle = start_handle,
        .value_handle = 0,
        .end_handle = end_handle,
        .properties = 0,
        .uuid16 = 0,
        .uuid128 = {0},
    };
    return btstack_error_to_errno(gatt_client_discover_characteristic_descriptors(&btstack_packet_handler_discover_descriptors, conn_handle, &characteristic));
}

int mp_bluetooth_gattc_read(uint16_t conn_handle, uint16_t value_handle) {
    DEBUG_EVENT_printf("mp_bluetooth_gattc_read\n");
    return btstack_error_to_errno(gatt_client_read_value_of_characteristic_using_value_handle(&btstack_packet_handler_read, conn_handle, value_handle));
}

int mp_bluetooth_gattc_write(uint16_t conn_handle, uint16_t value_handle, const uint8_t *value, size_t *value_len, unsigned int mode) {
    DEBUG_EVENT_printf("mp_bluetooth_gattc_write\n");

    // We should be distinguishing between gatt_client_write_value_of_characteristic vs
    // gatt_client_write_characteristic_descriptor_using_descriptor_handle.
    // However both are implemented using send_gatt_write_attribute_value_request under the hood,
    // and we get the exact same event to the packet handler.
    // Same story for the "without response" version.

    if (mode == MP_BLUETOOTH_WRITE_MODE_NO_RESPONSE) {
        // If possible, this will send immediately, copying the buffer directly to the ACL buffer.
        int err = gatt_client_write_value_of_characteristic_without_response(conn_handle, value_handle, *value_len, (uint8_t *)value);
        if (err == GATT_CLIENT_BUSY) {
            // Can't send right now, need to take a copy of the buffer and add it to the queue.
            btstack_enqueue_pending_operation(MP_BLUETOOTH_BTSTACK_PENDING_WRITE_NO_RESPONSE, conn_handle, value_handle, value, *value_len);
            // Notify when this conn_handle can write.
            gatt_client_request_can_write_without_response_event(&btstack_packet_handler_generic, conn_handle);
        } else {
            return btstack_error_to_errno(err);
        }
    } else if (mode == MP_BLUETOOTH_WRITE_MODE_WITH_RESPONSE) {
        // Pending operation copies the value buffer and keeps a GC reference until the response comes back.
        // TODO: Is there always a response?
        btstack_enqueue_pending_operation(MP_BLUETOOTH_BTSTACK_PENDING_WRITE, conn_handle, value_handle, value, *value_len);
        return btstack_error_to_errno(gatt_client_write_value_of_characteristic(&btstack_packet_handler_write_with_response, conn_handle, value_handle, *value_len, (uint8_t *)value));
    }

    return MP_EINVAL;
}
#endif // MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE

#endif // MICROPY_PY_BLUETOOTH && MICROPY_BLUETOOTH_BTSTACK
