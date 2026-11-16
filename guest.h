/* Rhizo-connector: A connector to different HF modems
 * Copyright (C) 2026 AG7JC
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * Guest mode: let any station (not only the -d peers) connect, run an
 * echo test, leave a short message, and pick up replies.
 */

#ifndef HAVE_GUEST_H__
#define HAVE_GUEST_H__

#include <stdbool.h>
#include <stdint.h>
#include "connector.h"

#ifdef __cplusplus
extern "C" {
#endif

// largest message a guest may send; anything bigger ends the session's rx
#define GUEST_MAX_MSG 4096
// messages (not counting echo pings) accepted from one guest per session
#define GUEST_MAX_MSGS_PER_SESSION 5
// echo pings answered per guest session
#define GUEST_MAX_ECHOES_PER_SESSION 3
// how much of the guest's echo text is quoted back
#define GUEST_ECHO_QUOTE 120

/* A callsign safe to use as a directory name and to show to the operator:
 * 3-10 letters/digits with at least one of each, optional -SSID (1-2 digits).
 * On success the uppercased call is written to out. */
bool guest_call_valid(const char *call, char *out, size_t out_size);

/* Control-port lines from the modem: remembers the session's last SNR and
 * bit rate for the echo reply. */
void guest_note_modem_line(rhizo_conn *connector, const char *line);

/* Called by the data rx thread with each complete frame of a guest
 * session (gen = the session it arrived in). Never writes to in_buffer. */
void guest_handle_frame(rhizo_conn *connector, const uint8_t *frame, uint32_t size, unsigned gen);

/* Called from the 1 Hz control loop once the session is ready: queues any
 * pending echo reply into the transmit buffer (the control thread is the
 * only writer of in_buffer while a session is up). */
void guest_tick(rhizo_conn *connector);

/* Reset per-session guest state (on CONNECTED). */
void guest_session_start(rhizo_conn *connector);

/* -e events file: one JSON object per line, for a status page.
 *   {"t":<unix>,"event":"link_up","who":"guest","call":"K1ABC"}
 *   {"t":<unix>,"event":"link_up","who":"peer"}      (our own stations: no call)
 *   {"t":<unix>,"event":"link_down"}
 *   {"t":<unix>,"event":"echo","call":"K1ABC","snr":6.2,"bps":1400}   (snr/bps null if unknown)
 *   {"t":<unix>,"event":"message","call":"K1ABC"}
 * Calls written here are always validated guest callsigns. */
void guest_event_link(rhizo_conn *connector, bool up);

#ifdef __cplusplus
};
#endif

#endif /* HAVE_GUEST_H__ */
