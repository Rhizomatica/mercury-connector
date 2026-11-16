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

/* Session state changes (CONNECTED, DISCONNECTED, teardown, the end of
 * setup) happen on two threads: the control rx thread sees the modem's
 * lines, the control tx thread does the work that needs the data tx thread
 * parked. This lock keeps them from interleaving, e.g. a teardown clearing
 * session_active right after a new CONNECTED set it. */
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;

/* Bookkeeping for a session that is over: back off the peer, log, write
 * link_down. Does not touch the transmit ring (the caller, or the next
 * session setup, resets that with tx parked). Call with session_lock held. */
static void session_end_bookkeeping(rhizo_conn *connector, const char *how)
{
    time_t now = time(NULL);
    size_t pending = connector->msg_path_queue_size;
    int idx = find_peer(connector, connector->session_peer);

    if (idx >= 0)
        connector->peer_retry_after[idx] = now + connector->retry_backoff;

    fprintf(stderr, "Session with %s ended%s (%zu unacknowledged message(s) kept for retry).\n",
            connector->session_peer[0] ? connector->session_peer : "unknown", how, pending);
    guest_event_link(connector, false);

    connector->session_peer[0] = 0;
    connector->session_ready = false;
    connector->attempt_peer = -1;
    connector->session_active = false;
}

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
        unsigned gen = connector->rx_session_gen;

        // take one whole frame off the ring (it was written in one piece)
        read_buffer(&connector->in_buffer, (uint8_t *) &buf_size, sizeof(buf_size)); // \todo if the two parties in a connection have different endianess, we are in trouble

        size_t total = sizeof(buf_size) + (size_t) buf_size;
        buffer = (uint8_t *) malloc(total);
        if (buffer == NULL){
            // can't happen for frames we built (<= MAX_RX_MSG_SIZE); stay in step anyway
            fprintf(stderr, "Out of memory for a %u byte frame, dropping it (it will be resent).\n", buf_size);
            uint8_t scratch[4096];
            for (uint32_t left = buf_size; left > 0; ){
                uint32_t c = left < sizeof(scratch) ? left : (uint32_t) sizeof(scratch);
                read_buffer(&connector->in_buffer, scratch, c);
                left -= c;
            }
            continue;
        }
        memcpy(buffer, &buf_size, sizeof(buf_size));
        read_buffer(&connector->in_buffer, buffer + sizeof(buf_size), buf_size);

        // Hand it to the modem, but never a byte after the session is over:
        // the check and each (non-blocking) write happen under session_lock,
        // so once DISCONNECTED/CONNECTED is processed nothing more of this
        // frame goes out. A frame cut short this way was not acknowledged;
        // its file is kept and sent again next session.
        size_t off = 0;
        bool cut = false;
        while (off < total && connector->tcp_ret_ok){
            struct pollfd pfd = { .fd = connector->data_socket, .events = POLLOUT };
            if (poll(&pfd, 1, 500) < 0 && errno != EINTR){
                connector->tcp_ret_ok = false;
                break;
            }
            pthread_mutex_lock(&session_lock);
            if (!connector->connected || connector->tx_hold || connector->rx_session_gen != gen){
                pthread_mutex_unlock(&session_lock);
                cut = true;
                break;
            }
            size_t chunk = total - off < 65536 ? total - off : 65536;
            ssize_t w = send(connector->data_socket, buffer + off, chunk, MSG_DONTWAIT | MSG_NOSIGNAL);
            pthread_mutex_unlock(&session_lock);
            if (w < 0){
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;
                fprintf(stderr, "Data socket write error (%s).\n", strerror(errno));
                connector->tcp_ret_ok = false;
                break;
            }
            off += (size_t) w;
        }
        if (cut && off > 0)
            fprintf(stderr, "Session ended while sending a frame: stopped after %zu of %zu bytes "
                    "(the rest is not sent after DISCONNECTED; the file will be resent).\n", off, total);

        free(buffer);
    }

exit_local:
    return EXIT_SUCCESS;
}

/* ---------------------------------------------------------------------
 * Receive side.
 *
 * The data socket to the TNC stays open across ARQ sessions, and the modem
 * reports CONNECTED/DISCONNECTED on the control port while the session's
 * bytes come in on the data port. Nothing orders the two for us, so:
 *
 *  - A frame belongs to the session it started in, and is read to its end
 *    even if DISCONNECTED is seen first: the modem may already have
 *    acknowledged bytes we haven't read yet (the sender then gets BUFFER 0
 *    and deletes the file, so dropping them would lose the message). The
 *    frame is only given up if a NEW session starts, or no more bytes come
 *    for RX_TAIL_QUIET_MS after the link went down.
 *  - Bytes that arrive with no session up are never thrown away on a
 *    timer. We give a CONNECTED that may be in flight RX_ORPHAN_WAIT_MS to
 *    show up (then they are that session's first frame); otherwise they are
 *    a late frame of the session that just ended, and are read as such.
 *  - A frame that is too big for its session (a guest message over
 *    GUEST_MAX_MSG) is skipped by its length; the frames after it are read
 *    normally. Only a header that can't be a real frame (0, or over
 *    MAX_RX_MSG_SIZE) means the stream is out of step, and then the rest of
 *    that session's data is discarded.
 *
 * Only complete frames are handed to the output spool thread.
 * --------------------------------------------------------------------- */

#define RX_POLL_MS 500
#define RX_TAIL_QUIET_MS 2000     // link down: wait this long for the rest of a frame
#define RX_TAIL_SETTLE_MS 100     // link down: let a CONNECTED in flight be parsed before taking bytes
#define RX_ORPHAN_WAIT_MS 3000    // bytes with no session: wait this long for its CONNECTED
#define RX_GRACE_STEP_MS 50
#define RX_SKIP_CHUNK 4096

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

/* Read exactly n bytes belonging to session gen. While the link is up this
 * waits as long as it takes (the idle timeout ends a stalled link). Once it
 * is down, it keeps reading the tail as long as bytes keep coming. Returns
 * false if a new session starts first, the tail stops, or the socket fails. */
static bool rx_read_frame_bytes(rhizo_conn *connector, uint8_t *buf, size_t n, unsigned gen)
{
    size_t got = 0;
    int quiet_ms = 0;

    while (got < n)
    {
        if (!connector->tcp_ret_ok || connector->rx_session_gen != gen)
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
        {
            if (!connector->connected)
            {
                quiet_ms += RX_POLL_MS;
                if (quiet_ms >= RX_TAIL_QUIET_MS)
                    return false;
            }
            continue;
        }

        if (!connector->connected)
        {
            // these may be the next session's first bytes, with its
            // CONNECTED not parsed yet: give the control thread a moment
            usleep(RX_TAIL_SETTLE_MS * 1000);
            if (connector->rx_session_gen != gen)
                return false;
        }

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
        quiet_ms = 0;
        connector->timeout_counter = 0;
    }
    return true;
}

/* Skip n bytes of session gen (a frame we won't keep). */
static bool rx_skip_bytes(rhizo_conn *connector, uint32_t n, unsigned gen)
{
    uint8_t scratch[RX_SKIP_CHUNK];
    while (n > 0)
    {
        uint32_t chunk = n < sizeof(scratch) ? n : (uint32_t) sizeof(scratch);
        if (!rx_read_frame_bytes(connector, scratch, chunk, gen))
            return false;
        n -= chunk;
    }
    return true;
}

/* Throw away everything left of session gen: until a new session starts,
 * or the link is down and the data has stopped. */
static void rx_discard_session(rhizo_conn *connector, unsigned gen)
{
    uint8_t scratch[RX_SKIP_CHUNK];
    size_t total = 0;
    int quiet_ms = 0;

    while (connector->tcp_ret_ok && connector->rx_session_gen == gen)
    {
        struct pollfd pfd = { .fd = connector->data_socket, .events = POLLIN };
        int r = poll(&pfd, 1, RX_POLL_MS);
        if (r <= 0)
        {
            if (!connector->connected && (quiet_ms += RX_POLL_MS) >= RX_TAIL_QUIET_MS)
                break;
            continue;
        }
        if (!connector->connected)
        {
            usleep(RX_TAIL_SETTLE_MS * 1000);
            if (connector->rx_session_gen != gen)
                break;
        }
        ssize_t len = recv(connector->data_socket, scratch, sizeof(scratch), 0);
        if (len <= 0)
        {
            if (len < 0 && errno == EINTR)
                continue;
            fprintf(stderr, "Data socket closed by TNC.\n");
            connector->tcp_ret_ok = false;
            break;
        }
        total += (size_t) len;
        quiet_ms = 0;
    }
    if (total)
        fprintf(stderr, "Discarded %zu byte(s) of that session's data.\n", total);
}

/* Bytes are readable but no session is up. Wait for a CONNECTED that may be
 * in flight; returns true if one came (the bytes are its), false if not (the
 * bytes are a late part of the session that just ended). */
static bool rx_wait_for_session(rhizo_conn *connector, unsigned gen0)
{
    for (int waited = 0; waited < RX_ORPHAN_WAIT_MS; waited += RX_GRACE_STEP_MS)
    {
        if (connector->connected || connector->rx_session_gen != gen0 || !connector->tcp_ret_ok)
            return true;
        usleep(RX_GRACE_STEP_MS * 1000);
    }
    return false;
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

        struct pollfd pfd = { .fd = connector->data_socket, .events = POLLIN };
        int r = poll(&pfd, 1, RX_POLL_MS);
        if (r < 0 && errno != EINTR)
        {
            fprintf(stderr, "Data socket poll error (%s).\n", strerror(errno));
            connector->tcp_ret_ok = false;
            break;
        }
        if (r <= 0)
            continue;

        unsigned gen = connector->rx_session_gen;
        if (!connector->connected && !rx_wait_for_session(connector, gen))
        {
            fprintf(stderr, "Data with no session up: reading it as a late part of the last session.\n");
        }
        gen = connector->rx_session_gen;

        // who sent this frame: fixed when it starts
        int kind = connector->session_kind;
        if (gen == 0 && !connector->connected)
            // before any session at all: nobody to attribute it to
            kind = connector->guests_enabled ? SESSION_REJECTED : SESSION_PEER;

        uint32_t frame_size = 0;
        if (!rx_read_frame_bytes(connector, (uint8_t *) &frame_size, sizeof(frame_size), gen))
            continue;

        if (frame_size == 0 || frame_size > MAX_RX_MSG_SIZE)
        {
            fprintf(stderr, "Corrupt frame header (length %u): discarding the rest of this session's data.\n",
                    frame_size);
            rx_discard_session(connector, gen);
            continue;
        }

        rx_set_safe(connector, &safe, false);   // mid-frame: not idle

        if (kind == SESSION_REJECTED)
        {
            fprintf(stderr, "Data from a station that is neither a peer nor a valid callsign: discarded.\n");
            rx_discard_session(connector, gen);
            continue;
        }

        if (kind == SESSION_GUEST && frame_size > GUEST_MAX_MSG)
        {
            fprintf(stderr, "Guest message too long (%u bytes, max %d): skipped.\n", frame_size, GUEST_MAX_MSG);
            if (!rx_skip_bytes(connector, frame_size, gen))
                fprintf(stderr, "Link lost inside the skipped message.\n");
            continue;
        }

        uint8_t *frame = (uint8_t *) malloc(frame_size);
        if (frame == NULL)
        {
            fprintf(stderr, "Out of memory for a %u byte frame: skipped, sender will retry.\n", frame_size);
            rx_skip_bytes(connector, frame_size, gen);
            continue;
        }

        if (!rx_read_frame_bytes(connector, frame, frame_size, gen))
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
                pthread_mutex_lock(&session_lock);
                connector->incoming_pending = false;
                connector->tx_hold = true;        // park tx before anything else
                // rx_session_gen is NOT bumped here: a frame in progress may
                // still finish (its tail can be read after this line); only
                // the next CONNECTED ends it for good
                connector->connected = false;
                connector->waiting_for_connection = false;
                pthread_mutex_unlock(&session_lock);
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

                    pthread_mutex_lock(&session_lock);
                    connector->tx_hold = true;   // nothing goes out until this peer's mail is loaded

                    // The previous session dropped and this one came up
                    // before the 1 Hz teardown ran (or there was no
                    // DISCONNECTED at all): finish its bookkeeping now, so it
                    // still gets its link_down and back-off. Its unsent bytes
                    // are dropped by this session's setup.
                    if (connector->session_active)
                        session_end_bookkeeping(connector, " (new session started before teardown)");

                    // A late answer to a CONNECT we already gave up on (or
                    // an inbound call racing ours) is just a session with
                    // whoever is named here; attempt_peer is not involved.
                    connector->attempt_peer = -1;

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
                    pthread_mutex_unlock(&session_lock);
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
        pthread_mutex_lock(&session_lock);
        if (connector->session_active && !connector->connected && connector->tx_idle)
        {
            spool_reset_tx(connector);
            session_end_bookkeeping(connector, "");
        }
        pthread_mutex_unlock(&session_lock);

        // Session setup: load ONLY this station's mail, then release tx.
        if (connector->session_active && connector->connected &&
            !connector->session_ready && connector->tx_idle)
        {
            unsigned setup_gen = connector->rx_session_gen;
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
            else if (connector->num_peers == 0)
            {
                // no -d: the input spool goes to whoever connected (master behaviour)
                int n = spool_load_any(connector);
                fprintf(stderr, "Session with %s: %d message(s) queued.\n",
                        connector->session_peer[0] ? connector->session_peer : "unknown station", n);
            }
            else
            {
                fprintf(stderr, "Session with %s (not a configured peer): receive only.\n",
                        connector->session_peer[0] ? connector->session_peer : "unknown station");
            }
            // Release tx only if this is still the session we loaded mail
            // for; if it dropped or another started meanwhile, the next tick
            // tears down or sets up again (and resets the ring first).
            pthread_mutex_lock(&session_lock);
            if (connector->rx_session_gen == setup_gen && connector->connected)
            {
                connector->attempt_peer = -1;
                connector->session_ready = true;
                connector->tx_hold = false;
            }
            pthread_mutex_unlock(&session_lock);
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
            else if (connector->session_kind == SESSION_PEER && connector->num_peers == 0)
            {
                int n = spool_load_any(connector);
                if (n > 0)
                    fprintf(stderr, "Session: %d more message(s) queued.\n", n);
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
                if (connector->waiting_for_connection)
                {
                    // the modem may still be calling: tell it to stop, or it
                    // keeps transmitting and may connect long after we gave up
                    fprintf(stderr, "No answer to our call after %d s: aborting it.\n", CONNECT_ATTEMPT_MAX_S);
                    connector->tcp_ret_ok &= tcp_write(connector->control_socket, (uint8_t *) "ABORT\r", 6);
                }
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
