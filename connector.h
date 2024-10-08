/* Rhizo-connector: A connector to different HF modems
 * Copyright (C) 2018 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 */

/**
 * @file connector.h
 * @author Rafael Diniz
 * @date 12 Apr 2018
 * @brief Rhizo HF Connector main header file
 *
 * Rhizo HF Connector main header file.
 *
 */

#ifndef HAVE_CONNECTOR_H__
#define HAVE_CONNECTOR_H__

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <hamlib/rig.h>

#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

// 90s
#define TIMEOUT_DEFAULT 90

typedef struct{
// public
    char call_sign[32];
    char remote_call_sign[32];
    int tcp_base_port;
    char ip_address[32];
    char modem_type[32];
    bool serial_keying;
    int serial_fd;
    rig_model_t radio_type;
    char serial_path[1024];
    char input_directory[1024];
    char output_directory[1024];
    bool ofdm_mode;
    int timeout;
    uint32_t buffer_size;

// state variables
// C11 atomic is used here instead of a more pedantic code with mutexes and so on... 
    atomic_bool tcp_ret_ok;
    atomic_bool connected;
    atomic_bool waiting_for_connection;
    atomic_int timeout_counter;
    atomic_int safe_state; // this means green light for changing state

// private
    buffer in_buffer;
    buffer out_buffer;
    int data_socket;
    int control_socket;
    pthread_mutex_t msg_path_queue_mutex;
    char *msg_path_queue[4096];
    size_t msg_path_queue_size;
} rhizo_conn;

#ifdef __cplusplus
};
#endif

#endif /* HAVE_CONNECTOR__ */
