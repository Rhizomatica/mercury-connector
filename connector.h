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
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <hamlib/rig.h>

#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

// 90s
#define TIMEOUT_DEFAULT 90

// multi-peer support (VARA/Mercury modems)
#define MAX_PEERS 16
// seconds to wait before calling a peer again after a failed attempt or a finished session
#define RETRY_BACKOFF_DEFAULT 120
// give up on an outbound CONNECT the modem never answered
#define CONNECT_ATTEMPT_MAX_S 300
// ignore spool files modified more recently than this (still being written)
#define SPOOL_SETTLE_S 2
// largest frame (filename + payload) accepted from the air; anything bigger
// means the stream is corrupt
#define MAX_RX_MSG_SIZE (16u * 1024u * 1024u)
// longest filename written to the output spool
#define MAX_RX_NAME_LEN 100
// messages loaded into one session's transmit buffer (size of msg_path_queue)
#define MSG_PATH_QUEUE_MAX 4096
// stop waiting for a PENDING (incoming call) that never resolves
#define PENDING_TIMEOUT_S 30
// after CANCELPENDING, hold our own calls for MIN + rand() % JITTER seconds,
// so the other station can retry first
#define CHANNEL_BUSY_MIN_S 15
#define CHANNEL_BUSY_JITTER_S 30

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
    atomic_bool incoming_pending;   // modem reported PENDING: another station is calling us
    atomic_long pending_since;      // when that PENDING arrived (time_t)
    atomic_long channel_busy_until; // don't start our own call before this (time_t)
    atomic_int timeout_counter;
    atomic_int safe_state; // this means green light for changing state

// private
    buffer in_buffer;
    buffer out_buffer;
    int data_socket;
    int control_socket;
    pthread_mutex_t msg_path_queue_mutex;
    char *msg_path_queue[MSG_PATH_QUEUE_MAX];
    size_t msg_path_queue_size;

// multi-peer (VARA/Mercury only; ARDOP keeps the legacy single-peer path)
    char peers[MAX_PEERS][32];      // from -d CALL1,CALL2,...; peers[0] is the default
    int num_peers;
    time_t peer_retry_after[MAX_PEERS];
    int next_peer;                  // round-robin start index for outbound calls
    int retry_backoff;              // -w seconds
    int attempt_peer;               // peer index of the outstanding CONNECT, -1 if none
    time_t attempt_time;
    char session_peer[32];          // remote callsign of the current session
    atomic_bool session_active;     // CONNECTED seen, teardown not yet done
    atomic_bool session_ready;      // this session's mail has been loaded
    atomic_bool tx_hold;            // data tx thread must not send
    atomic_bool tx_idle;            // data tx thread is parked in its wait loop
    atomic_uint rx_session_gen;     // bumped on every CONNECTED/DISCONNECTED; a
                                    // received frame must start and end in one

// guest mode (-g, VARA/Mercury only; see guest.c)
    bool guests_enabled;
    atomic_int session_kind;        // SESSION_PEER, SESSION_GUEST or SESSION_REJECTED
    char guest_call[16];            // validated, uppercased callsign of a guest session
    int guest_msgs;                 // rx thread only
    int guest_echoes;               // rx thread only
    pthread_mutex_t echo_mutex;
    bool echo_pending;
    unsigned echo_gen;
    char echo_quote[128];
    atomic_int snr_x10;             // last "SN" from the modem this session, dB x10
    atomic_bool snr_valid;
    atomic_int bps;                 // last "BITRATE" from the modem this session
    char events_path[1024];         // -e: JSON lines of link/guest events ("" = off)
    pthread_mutex_t events_mutex;
} rhizo_conn;

// what kind of station the current session is with
#define SESSION_PEER 0      // a -d peer (or anyone, without -g: receive only)
#define SESSION_GUEST 1     // -g: a guest with a valid callsign
#define SESSION_REJECTED 2  // -g: not a peer and not a usable callsign; nothing kept

#ifdef __cplusplus
};
#endif

#endif /* HAVE_CONNECTOR__ */
