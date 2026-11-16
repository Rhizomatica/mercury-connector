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
 * Vara support routines
 */

/**
 * @file vara.c
 * @author Rafael Diniz
 * @date 12 Apr 2018
 * @brief VARA modem support functions
 *
 * All the specific code for supporting VARA.
 *
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sched.h>

#include "common.h"
#include "net.h"
#include <strings.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>

#include "spool.h"
#include "vara.h"
#include "radio_io.h"
#include "guest.h"

void *vara_data_worker_thread_tx(void *conn)
{
    rhizo_conn *connector = (rhizo_conn *) conn;
    uint8_t *buffer;
    uint32_t buf_size;

    while(connector->tcp_ret_ok){

        // check if we are connected, otherwise, wait
        // tx_hold: session setup/teardown in progress, the buffer may be
        // reset under us, so stay parked (and say so via tx_idle).
        // Announce "busy" BEFORE re-checking tx_hold (seq_cst atomics), so the
        // control thread can never reset the buffer while we are reading it.
        connector->safe_state++;
        while (true){
            if (connector->connected && !connector->tx_hold &&
                ring_buffer_count_bytes(&connector->in_buffer.buf) > 0){
                connector->tx_idle = false;
                if (connector->connected && !connector->tx_hold)
                    break;
            }
            connector->tx_idle = true;
            if (connector->tcp_ret_ok == false){
                connector->safe_state--;
                goto exit_local;
            }
            sleep(1);
        }
        connector->safe_state--;

        connector->timeout_counter = 0;

        // read header
        read_buffer(&connector->in_buffer, (uint8_t *) &buf_size, sizeof(buf_size)); // \todo if the two parties in a connection have different endianess, we are in trouble

        buffer = (uint8_t *) malloc (buf_size);
        memset(buffer, 0, buf_size);
        read_buffer(&connector->in_buffer, buffer, buf_size);

        connector->tcp_ret_ok &= tcp_write(connector->data_socket, (uint8_t *) &buf_size, sizeof(buf_size) );
        connector->tcp_ret_ok &= tcp_write(connector->data_socket, buffer, buf_size);

        free(buffer);
    }

exit_local:
    return EXIT_SUCCESS;
}

/* ---------------------------------------------------------------------
 * Receive side.
 *
 * The data socket to the TNC stays open across ARQ sessions, so a plain
 * blocking read that is halfway through a frame when a link drops would
 * keep going with the NEXT session's bytes, merging two messages and
 * misframing everything after them. Instead, every read is bounded by
 * the session it started in (rx_session_gen): a frame is only accepted
 * if all of it arrives within one session; a partial one is discarded
 * (the sender never got BUFFER 0 for it, so it keeps the file and
 * resends it). Bytes arriving while no session is up belong to one that
 * already ended and are dropped. Only complete frames are handed to the
 * output spool thread.
 * --------------------------------------------------------------------- */

#define RX_POLL_MS 500

static void rx_set_safe(rhizo_conn *connector, bool *safe, bool want)
{
    if (want && !*safe)
    {
        connector->safe_state++;
        *safe = true;
    }
    else if (!want && *safe)
    {
        connector->safe_state--;
        *safe = false;
    }
}

/* Wait up to RX_POLL_MS and throw away whatever is readable.
 *
 * idle: we're between sessions. The modem reports CONNECTED on the control
 * port and the first data on the data port, and nothing orders the two for
 * us: the first frame of a new session can be readable here a moment before
 * the control thread has marked the session up. So before calling bytes
 * stale, give a just-starting session a short grace period, and leave the
 * bytes alone if one shows up. */
#define RX_NEW_SESSION_GRACE_MS 1000
#define RX_GRACE_STEP_MS 50

static void rx_discard_pending(rhizo_conn *connector, bool idle)
{
    uint8_t scratch[1024];
    struct pollfd pfd = { .fd = connector->data_socket, .events = POLLIN };
    unsigned gen0 = connector->rx_session_gen;

    int r = poll(&pfd, 1, RX_POLL_MS);
    if (r <= 0)
        return;

    if (idle)
    {
        for (int waited = 0; waited < RX_NEW_SESSION_GRACE_MS; waited += RX_GRACE_STEP_MS)
        {
            if (connector->connected || connector->rx_session_gen != gen0)
                return;                 // a session just started: these bytes are its
            usleep(RX_GRACE_STEP_MS * 1000);
        }
    }

    ssize_t len = recv(connector->data_socket, scratch, sizeof(scratch), 0);
    if (len == 0 || (len < 0 && errno != EINTR))
    {
        fprintf(stderr, "Data socket closed by TNC.\n");
        connector->tcp_ret_ok = false;
    }
    else if (len > 0)
    {
        fprintf(stderr, "Discarded %zd byte(s) not belonging to a live session.\n", len);
    }
}

/* Read exactly n bytes of the session identified by gen. Returns false if
 * that session ends (or a new one starts) first, or the socket fails. */
static bool rx_read_session(rhizo_conn *connector, uint8_t *buf, size_t n, unsigned gen)
{
    size_t got = 0;

    while (got < n)
    {
        if (!connector->tcp_ret_ok || !connector->connected || connector->rx_session_gen != gen)
            return false;

        struct pollfd pfd = { .fd = connector->data_socket, .events = POLLIN };
        int r = poll(&pfd, 1, RX_POLL_MS);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "Data socket poll error (%s).\n", strerror(errno));
            connector->tcp_ret_ok = false;
            return false;
        }
        if (r == 0)
            continue;

        // re-check: bytes must not be taken for a session that just ended
        if (!connector->connected || connector->rx_session_gen != gen)
            return false;

        ssize_t len = recv(connector->data_socket, buf + got, n - got, 0);
        if (len < 0)
        {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "Data socket read error (%s).\n", strerror(errno));
            connector->tcp_ret_ok = false;
            return false;
        }
        if (len == 0)
        {
            fprintf(stderr, "Data socket closed by TNC.\n");
            connector->tcp_ret_ok = false;
            return false;
        }
        got += (size_t) len;
        connector->timeout_counter = 0;
    }
    return true;
}

void *vara_data_worker_thread_rx(void *conn)
{
    rhizo_conn *connector = (rhizo_conn *) conn;
    bool safe = false;

    while (connector->tcp_ret_ok)
    {
        // idle (waiting for a frame to start) counts as "safe" for the
        // idle-timeout logic, exactly like the original header wait
        rx_set_safe(connector, &safe, true);

        if (!connector->connected)
        {
            rx_discard_pending(connector, true);
            continue;
        }

        unsigned gen = connector->rx_session_gen;
        uint32_t frame_size = 0;

        if (!rx_read_session(connector, (uint8_t *) &frame_size, sizeof(frame_size), gen))
            continue;

        if (frame_size == 0 || frame_size > MAX_RX_MSG_SIZE)
        {
            fprintf(stderr, "Corrupt frame header (length %u): discarding the rest of this session's data.\n",
                    frame_size);
            while (connector->tcp_ret_ok && connector->connected && connector->rx_session_gen == gen)
                rx_discard_pending(connector, false);
            continue;
        }

        int kind = connector->session_kind;
        if (kind == SESSION_REJECTED || (kind == SESSION_GUEST && frame_size > GUEST_MAX_MSG))
        {
            fprintf(stderr, kind == SESSION_REJECTED
                    ? "Data from a station that is neither a peer nor a valid callsign: discarded.\n"
                    : "Guest message too long (%u bytes, max %d): discarding the rest of this session's data.\n",
                    frame_size, GUEST_MAX_MSG);
            while (connector->tcp_ret_ok && connector->connected && connector->rx_session_gen == gen)
                rx_discard_pending(connector, false);
            continue;
        }

        rx_set_safe(connector, &safe, false);   // mid-frame: not idle

        uint8_t *frame = (uint8_t *) malloc(frame_size);
        if (frame == NULL)
        {
            fprintf(stderr, "Out of memory for a %u byte frame, discarding the rest of this session.\n", frame_size);
            rx_set_safe(connector, &safe, true);
            while (connector->tcp_ret_ok && connector->connected && connector->rx_session_gen == gen)
                rx_discard_pending(connector, false);
            continue;
        }

        if (!rx_read_session(connector, frame, frame_size, gen))
        {
            fprintf(stderr, "Link lost mid-message: partial message (%u bytes expected) discarded, "
                    "sender will retry.\n", frame_size);
            free(frame);
            continue;
        }

        if (kind == SESSION_GUEST)
        {
            // guests: echo pings and short messages, handled here; the
            // inbox file is written with our header, not theirs
            guest_handle_frame(connector, frame, frame_size, gen);
            free(frame);
            continue;
        }

        // complete frame: hand it to the output spool thread in one piece
        write_buffer(&connector->out_buffer, (uint8_t *) &frame_size, sizeof(frame_size));
        write_buffer(&connector->out_buffer, frame, frame_size);
        free(frame);
    }

    rx_set_safe(connector, &safe, false);
    return EXIT_SUCCESS;
}

void *vara_control_worker_thread_rx(void *conn)
{
    rhizo_conn *connector = (rhizo_conn *) conn;
    uint8_t rcv_byte;
    uint8_t buffer[1024];
    int counter = 0;
    bool new_cmd = false;

    while(connector->tcp_ret_ok){
        connector->tcp_ret_ok &= tcp_read(connector->control_socket, &rcv_byte, 1);

        if (rcv_byte == '\r')
        {
            buffer[counter] = 0;
            counter = 0;
            new_cmd = true;
        }
        else
        {
            buffer[counter] = rcv_byte;
            counter++;
            new_cmd = false;
        }

        if (new_cmd)
        {
            if (!strcmp((char *) buffer, "DISCONNECTED"))
            {
                fprintf(stderr, "TNC: %s\n", buffer);
                connector->incoming_pending = false;
                connector->tx_hold = true;        // park tx before anything else
                connector->rx_session_gen++;      // abandon any partial rx frame
                connector->connected = false;
                connector->waiting_for_connection = false;
            }
            else
            {
            // other commands here
                if (!memcmp(buffer, "CONNECTED", strlen("CONNECTED")))
                {
                    // "CONNECTED <source> <destination> <bw>": the remote
                    // station is whichever callsign is not ours (source on
                    // inbound calls, destination on our own outbound calls)
                    char src[32] = {0}, dst[32] = {0};
                    const char *peer = NULL;

                    sscanf((char *) buffer, "CONNECTED %31s %31s", src, dst);
                    if (src[0] && strcasecmp(src, connector->call_sign))
                        peer = src;
                    else if (dst[0] && strcasecmp(dst, connector->call_sign))
                        peer = dst;

                    fprintf(stderr, "TNC: %s\n", buffer);

                    connector->tx_hold = true;   // nothing goes out until this peer's mail is loaded
                    connector->session_ready = false;
                    snprintf(connector->session_peer, sizeof(connector->session_peer), "%s",
                             peer ? peer : "");
                    guest_session_start(connector);
                    if (!connector->guests_enabled || find_peer(connector, connector->session_peer) >= 0)
                        connector->session_kind = SESSION_PEER;
                    else if (guest_call_valid(connector->session_peer, connector->guest_call,
                                              sizeof(connector->guest_call)))
                        connector->session_kind = SESSION_GUEST;
                    else
                        connector->session_kind = SESSION_REJECTED;
                    guest_event_link(connector, true);
                    connector->session_active = true;
                    connector->incoming_pending = false;
                    connector->rx_session_gen++;   // frames must start in this session
                    connector->connected = true;
                    connector->waiting_for_connection = false;
                }
                else if (!strcmp((char *) buffer, "PENDING"))
                {
                    // Another station is calling us and the modem is
                    // answering. Starting our own CONNECT now would cancel
                    // that handshake and both stations would then call each
                    // other and miss -- so hold our own calls until this
                    // resolves (CONNECTED, CANCELPENDING, or PENDING_TIMEOUT_S).
                    fprintf(stderr, "%s\n", buffer);
                    connector->pending_since = (long) time(NULL);
                    connector->incoming_pending = true;
                }
                else if (!strcmp((char *) buffer, "CANCELPENDING"))
                {
                    fprintf(stderr, "%s\n", buffer);
                    connector->incoming_pending = false;
                    // the other station may still be retrying: give it a
                    // random head start before we call out ourselves
                    connector->channel_busy_until = (long) time(NULL) + CHANNEL_BUSY_MIN_S + rand() % CHANNEL_BUSY_JITTER_S;
                }
                else
                {
                    if (!memcmp(buffer, "BUFFER", strlen("BUFFER")))
                    {
                        uint32_t buf_size;
                        sscanf( (char *) buffer, "BUFFER %u", &buf_size);
                        fprintf(stderr, "BUFFER: %u\n", buf_size);

                        if (buf_size != 0)
                            connector->timeout_counter = 0;

                        // our delete messages mechanism
                        // only delete what this session actually handed to
                        // the modem: buffer drained, tx thread finished
                        // writing, and the session's mail was loaded
                        if (buf_size == 0 &&
                            ring_buffer_count_bytes(&connector->in_buffer.buf) == 0 &&
                            connector->connected == true &&
                            connector->session_ready &&
                            connector->tx_idle)
                        {

                            fprintf(stderr, "Messages successfully sent. Erasing messages...\n");
                            remove_all_msg_path_queue(connector);

                        }
                    }
                    else
                    {
                        guest_note_modem_line(connector, (const char *) buffer);
                        if (connector->serial_keying == true)
                        {
                            if (!memcmp(buffer, "PTT ON", strlen("PTT ON")))
                            {
                                key_on();
                            }
                            if (!memcmp(buffer, "PTT OFF", strlen("PTT OFF")))
                            {
                                key_off();
                            }
                        }
                        fprintf(stderr, "%s\n", buffer);
                    }
                }
            }
        }
    }

    return EXIT_SUCCESS;
}

void *vara_control_worker_thread_tx(void *conn)
{
    rhizo_conn *connector = (rhizo_conn *) conn;
    char buffer[1024];

    // We set a call sign
    memset(buffer,0,sizeof(buffer));
    sprintf(buffer, "MYCALL %s\r", connector->call_sign);
    connector->tcp_ret_ok &= tcp_write(connector->control_socket, (uint8_t *) buffer, strlen(buffer));

    memset(buffer,0,sizeof(buffer));
    strcpy(buffer,"LISTEN ON\r");
    connector->tcp_ret_ok &= tcp_write(connector->control_socket, (uint8_t *) buffer, strlen(buffer));

    memset(buffer,0,sizeof(buffer));
    strcpy(buffer,"PUBLIC OFF\r");
    connector->tcp_ret_ok &= tcp_write(connector->control_socket, (uint8_t *) buffer, strlen(buffer));

    memset(buffer,0,sizeof(buffer));
    strcpy(buffer,"BW2300\r");
    connector->tcp_ret_ok &= tcp_write(connector->control_socket, (uint8_t *) buffer, strlen(buffer));

//    if (vara_p2p_mode)
//    {
        memset(buffer,0,sizeof(buffer));
        strcpy(buffer,"P2P SESSION\r");
        connector->tcp_ret_ok &= tcp_write(connector->control_socket, (uint8_t *) buffer, strlen(buffer));
//    }


    // 1Hz function
    while(connector->tcp_ret_ok)
    {

        time_t now = time(NULL);

        // Session teardown: the link is gone. Once the data tx thread is
        // parked, drop whatever was not handed to the modem (files are kept
        // and retried next session with the right station).
        if (connector->session_active && !connector->connected && connector->tx_idle)
        {
            size_t pending = connector->msg_path_queue_size;
            int idx = find_peer(connector, connector->session_peer);

            spool_reset_tx(connector);
            if (idx >= 0)
                connector->peer_retry_after[idx] = now + connector->retry_backoff;

            fprintf(stderr, "Session with %s ended (%zu unacknowledged message(s) kept for retry).\n",
                    connector->session_peer[0] ? connector->session_peer : "unknown", pending);
            guest_event_link(connector, false);

            connector->session_peer[0] = 0;
            connector->session_ready = false;
            connector->attempt_peer = -1;
            connector->session_active = false;
        }

        // Session setup: load ONLY this station's mail, then release tx.
        if (connector->session_active && connector->connected &&
            !connector->session_ready && connector->tx_idle)
        {
            int idx = find_peer(connector, connector->session_peer);

            spool_reset_tx(connector);   // never carry bytes over from another session
            if (idx >= 0)
            {
                int n = spool_load_peer(connector, idx);
                fprintf(stderr, "Session with %s: %d message(s) queued.\n", connector->peers[idx], n);
            }
            else if (connector->session_kind == SESSION_GUEST)
            {
                int n = spool_load_call(connector, connector->guest_call);
                fprintf(stderr, "Guest session with %s: %d message(s) queued.\n", connector->guest_call, n);
            }
            else if (connector->session_kind == SESSION_REJECTED)
            {
                fprintf(stderr, "Session with \"%s\": not a peer and not a valid callsign, nothing will be kept.\n",
                        connector->session_peer);
            }
            else
            {
                fprintf(stderr, "Session with %s (not a configured peer): receive only.\n",
                        connector->session_peer[0] ? connector->session_peer : "unknown station");
            }
            connector->attempt_peer = -1;
            connector->session_ready = true;
            connector->tx_hold = false;
        }
        // Mid-session: pick up mail for this station that arrived after setup.
        else if (connector->session_active && connector->connected && connector->session_ready)
        {
            int idx = find_peer(connector, connector->session_peer);
            if (idx >= 0)
            {
                int n = spool_load_peer(connector, idx);
                if (n > 0)
                    fprintf(stderr, "Session with %s: %d more message(s) queued.\n", connector->peers[idx], n);
            }
            else if (connector->session_kind == SESSION_GUEST)
            {
                guest_tick(connector);
                int n = spool_load_call(connector, connector->guest_call);
                if (n > 0)
                    fprintf(stderr, "Guest session with %s: %d more message(s) queued.\n", connector->guest_call, n);
            }
        }

        // Outbound attempt that never became a session (modem answered
        // DISCONNECTED, or never answered at all): back off that peer.
        if (connector->attempt_peer >= 0 && !connector->session_active)
        {
            if (!connector->waiting_for_connection ||
                now - connector->attempt_time > CONNECT_ATTEMPT_MAX_S)
            {
                // random extra wait, so two stations that keep calling each
                // other at the same moment drift apart instead of clashing again
                int wait = connector->retry_backoff + rand() % (connector->retry_backoff / 4 + 1);
                fprintf(stderr, "Call to %s failed, retrying in %d s.\n",
                        connector->peers[connector->attempt_peer], wait);
                connector->peer_retry_after[connector->attempt_peer] = now + wait;
                connector->attempt_peer = -1;
                connector->waiting_for_connection = false;
            }
        }

        // A PENDING that never resolved (no CONNECTED/CANCELPENDING): stop
        // waiting for it after PENDING_TIMEOUT_S.
        if (connector->incoming_pending && now - (time_t) connector->pending_since > PENDING_TIMEOUT_S)
            connector->incoming_pending = false;

        // Logic to start a connection: idle, nobody calling us right now,
        // and some peer (not backing off) has mail waiting. Peers take
        // turns so none is starved.
        if (connector->connected == false &&
            !connector->session_active &&
            !connector->waiting_for_connection &&
            !connector->incoming_pending &&
            now >= (time_t) connector->channel_busy_until)
        {
            for (int k = 0; k < connector->num_peers; k++)
            {
                int idx = (connector->next_peer + k) % connector->num_peers;

                if (now < connector->peer_retry_after[idx] || !spool_peer_has_mail(connector, idx))
                    continue;

                // \todo try to add some entropy in order to avoid on air clashes
                memset(buffer,0,sizeof(buffer));
                snprintf(buffer, sizeof(buffer), "CONNECT %s %s\r", connector->call_sign,
                         connector->peers[idx]);
                connector->tcp_ret_ok &= tcp_write(connector->control_socket, (uint8_t *)buffer, strlen(buffer));

                fprintf(stderr, "CONNECTING... %s\n", buffer);

                connector->attempt_peer = idx;
                connector->attempt_time = now;
                connector->next_peer = (idx + 1) % connector->num_peers;
                connector->waiting_for_connection = true;
                break;
            }
        }

        // Logic to disconnect on timeout
        if (connector->timeout_counter >= connector->timeout &&
            connector->connected == true)
        {

            connector->tx_hold = true;
            connector->rx_session_gen++;
            connector->connected = false;

            while (connector->safe_state != 2)
            { // this means we have both data threads in the safe zone
                sched_yield();
            }
            fprintf(stderr, "DISCONNECTING BY TIMEOUT...\n");

            memset(buffer,0,sizeof(buffer));
            sprintf(buffer,"DISCONNECT\r");
            connector->tcp_ret_ok &= tcp_write(connector->control_socket, (uint8_t *)buffer, strlen(buffer));

        }

        sleep(1);

    }

    return EXIT_SUCCESS;
}

bool initialize_modem_vara(rhizo_conn *connector)
{
    connector->tcp_ret_ok &= tcp_connect(connector->ip_address, connector->tcp_base_port, &connector->control_socket);
    connector->tcp_ret_ok &= tcp_connect(connector->ip_address, connector->tcp_base_port+1, &connector->data_socket);

    if (connector->tcp_ret_ok == false)
    {
        fprintf(stderr, "Connection to TNC failure.\n");
        return false;
    }

    // we start our control thread
    pthread_t tid1;
    pthread_create(&tid1, NULL, vara_control_worker_thread_rx, (void *) connector);

    // we start our control tx thread
    pthread_t tid2;
    pthread_create(&tid2, NULL, vara_control_worker_thread_tx, (void *) connector);

    pthread_t tid3;
    pthread_create(&tid3, NULL, vara_data_worker_thread_tx, (void *) connector);

    pthread_t tid4;
    pthread_create(&tid4, NULL, vara_data_worker_thread_rx, (void *) connector);

    pthread_t tid5;
    pthread_create(&tid5, NULL, connection_timeout_thread, (void *) connector);

    pthread_join(tid1, NULL);
    pthread_join(tid2, NULL);
    pthread_join(tid3, NULL);
    pthread_join(tid4, NULL);
    pthread_join(tid5, NULL);

    return true;
}
