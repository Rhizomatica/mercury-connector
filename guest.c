/* Rhizo-connector: A connector to different HF modems
 * Copyright (C) 2026 AG7JC
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
 * Guest mode (-g)
 *
 * Without -g only the -d peers exchange mail; anyone else gets a
 * receive-only session whose files land in the inbox exactly as sent.
 * With -g, every station that is not a peer is a guest:
 *
 *  - its callsign must look like a callsign (it becomes a directory
 *    name and is shown to the operator); otherwise nothing it sends is
 *    kept
 *  - a frame whose filename starts with "echo" is an echo ping: it is
 *    answered in the same session with what this station heard (SNR and
 *    bit rate reported by the modem) and a short quote of the ping
 *  - any other frame is a short text message (GUEST_MAX_MSG bytes, a few
 *    per session). It is NOT stored as sent: the connector writes the
 *    inbox file itself, with the header taken from the session (so a
 *    guest can't claim to be someone else or ask for a relay) and the
 *    guest's bytes, cleaned of control characters, as the body
 *  - replies for a guest are files in <input_directory>/<CALL>/, loaded
 *    when that guest calls (the connector never calls guests itself)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#include "guest.h"
#include "spool.h"

bool guest_call_valid(const char *call, char *out, size_t out_size)
{
    char tmp[16];
    size_t n = 0, base_len = 0;
    bool letter = false, digit = false, ssid = false;
    size_t ssid_len = 0;

    if (call == NULL)
        return false;

    for (const char *p = call; *p; p++)
    {
        char c = (char) toupper((unsigned char) *p);
        if (n + 1 >= sizeof(tmp))
            return false;
        if (c == '-')
        {
            if (ssid || base_len == 0)
                return false;
            ssid = true;
        }
        else if (isdigit((unsigned char) c))
        {
            if (ssid)
                ssid_len++;
            else
            {
                base_len++;
                digit = true;
            }
        }
        else if (c >= 'A' && c <= 'Z')
        {
            if (ssid)
                return false;
            base_len++;
            letter = true;
        }
        else
            return false;
        tmp[n++] = c;
    }
    tmp[n] = 0;

    if (base_len < 3 || base_len > 10 || !letter || !digit)
        return false;
    if (ssid && (ssid_len < 1 || ssid_len > 2))
        return false;
    if (n + 1 > out_size)
        return false;
    strcpy(out, tmp);
    return true;
}

void guest_session_start(rhizo_conn *connector)
{
    connector->snr_valid = false;
    connector->bps = 0;
    connector->guest_msgs = 0;
    connector->guest_echoes = 0;
    pthread_mutex_lock(&connector->echo_mutex);
    connector->echo_pending = false;
    pthread_mutex_unlock(&connector->echo_mutex);
}

void guest_note_modem_line(rhizo_conn *connector, const char *line)
{
    float snr;
    int n, rate;

    if (sscanf(line, "SN %f", &snr) == 1)
    {
        connector->snr_x10 = (int) (snr * 10.0f + (snr >= 0 ? 0.5f : -0.5f));
        connector->snr_valid = true;
    }
    else if (sscanf(line, "BITRATE (%d) %d BPS", &n, &rate) == 2 && rate > 0)
    {
        connector->bps = rate;
    }
}

/* printable text only: tabs/newlines kept, \r dropped, other control bytes
 * become spaces; UTF-8 (bytes >= 0x80) passes through */
static size_t clean_text(const uint8_t *in, size_t len, char *out, size_t out_size, bool one_line)
{
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < out_size; i++)
    {
        uint8_t c = in[i];
        if (c == '\r')
            continue;
        if (c == '\n' || c == '\t')
            out[n++] = one_line ? ' ' : (char) c;
        else if (c < 0x20 || c == 0x7f)
            out[n++] = ' ';
        else
            out[n++] = (char) c;
    }
    // trim
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\n' || out[n - 1] == '\t'))
        n--;
    out[n] = 0;
    size_t start = 0;
    while (out[start] == ' ' || out[start] == '\n' || out[start] == '\t')
        start++;
    if (start)
        memmove(out, out + start, n - start + 1);
    return strlen(out);
}

static void event_line(rhizo_conn *connector, const char *json_fields)
{
    if (connector->events_path[0] == 0)
        return;
    pthread_mutex_lock(&connector->events_mutex);
    FILE *f = fopen(connector->events_path, "a");
    if (f != NULL)
    {
        fprintf(f, "{\"t\":%ld,%s}\n", (long) time(NULL), json_fields);
        fclose(f);
    }
    else
        fprintf(stderr, "Events file %s: %s\n", connector->events_path, strerror(errno));
    pthread_mutex_unlock(&connector->events_mutex);
}

void guest_event_link(rhizo_conn *connector, bool up)
{
    char fields[96];
    if (!up)
        snprintf(fields, sizeof(fields), "\"event\":\"link_down\"");
    else if (connector->session_kind == SESSION_GUEST)
        snprintf(fields, sizeof(fields), "\"event\":\"link_up\",\"who\":\"guest\",\"call\":\"%s\"",
                 connector->guest_call);
    else
        snprintf(fields, sizeof(fields), "\"event\":\"link_up\",\"who\":\"%s\"",
                 connector->session_kind == SESSION_PEER ? "peer" : "unknown");
    event_line(connector, fields);
}

static bool store_guest_message(rhizo_conn *connector, const char *body)
{
    static unsigned long counter = 0;
    char stamp[32], name[96], stored[128];
    char text[GUEST_MAX_MSG + 128];
    time_t now = time(NULL);
    struct tm tm;

    gmtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &tm);
    snprintf(name, sizeof(name), "guest_%s_%s_%lu.txt", connector->guest_call, stamp, counter++);

    // the header is ours, never the guest's
    int len = snprintf(text, sizeof(text), "type: report\ncallsign: %s\ncategory: status\nguest: yes\n\n%s\n",
                       connector->guest_call, body);
    if (len < 0 || (size_t) len >= sizeof(text))
        len = (int) strlen(text);

    // same publishing as normal mail: temp file, fsync, link, ".N" on a clash
    int r = spool_store(connector, name, (const uint8_t *) text, (size_t) len, stored, sizeof(stored));
    if (r == SPOOL_FAILED)
        return false;
    if (r == SPOOL_STORED)
    {
        char fields[64];
        fprintf(stderr, "Guest %s: message stored as %s.\n", connector->guest_call, stored);
        snprintf(fields, sizeof(fields), "\"event\":\"message\",\"call\":\"%s\"", connector->guest_call);
        event_line(connector, fields);
    }
    return true;
}

void guest_handle_frame(rhizo_conn *connector, const uint8_t *frame, uint32_t size, unsigned gen)
{
    size_t scan = size < 128 ? size : 128;
    const uint8_t *nl = memchr(frame, '\n', scan);
    const uint8_t *payload = frame;
    size_t payload_len = size;
    bool echo = false;

    if (nl != NULL)
    {
        size_t name_len = (size_t) (nl - frame);
        // last path component of the name, then "echo..."?
        size_t start = 0;
        for (size_t i = 0; i < name_len; i++)
            if (frame[i] == '/' || frame[i] == '\\')
                start = i + 1;
        echo = name_len - start >= 4 && !strncasecmp((const char *) frame + start, "echo", 4);
        payload = nl + 1;
        payload_len = size - name_len - 1;
    }

    if (echo)
    {
        if (connector->guest_echoes >= GUEST_MAX_ECHOES_PER_SESSION)
        {
            fprintf(stderr, "Guest %s: echo limit for this session reached, ping ignored.\n",
                    connector->guest_call);
            return;
        }
        connector->guest_echoes++;
        pthread_mutex_lock(&connector->echo_mutex);
        clean_text(payload, payload_len, connector->echo_quote, GUEST_ECHO_QUOTE + 1, true);
        connector->echo_gen = gen;
        connector->echo_pending = true;
        pthread_mutex_unlock(&connector->echo_mutex);
        fprintf(stderr, "Guest %s: echo ping received, replying.\n", connector->guest_call);
        return;
    }

    if (connector->guest_msgs >= GUEST_MAX_MSGS_PER_SESSION)
    {
        fprintf(stderr, "Guest %s: message limit for this session reached, message dropped.\n",
                connector->guest_call);
        return;
    }

    char body[GUEST_MAX_MSG + 1];
    if (clean_text(payload, payload_len, body, sizeof(body), false) == 0)
    {
        fprintf(stderr, "Guest %s: empty message dropped.\n", connector->guest_call);
        return;
    }
    if (store_guest_message(connector, body))
        connector->guest_msgs++;
}

void guest_tick(rhizo_conn *connector)
{
    char text[512], name[] = "echo-reply.txt\n";
    char when[32], snr[24], rate[24], quote[sizeof(((rhizo_conn *) 0)->echo_quote)];
    unsigned gen;

    pthread_mutex_lock(&connector->echo_mutex);
    if (!connector->echo_pending)
    {
        pthread_mutex_unlock(&connector->echo_mutex);
        return;
    }
    connector->echo_pending = false;
    gen = connector->echo_gen;
    snprintf(quote, sizeof(quote), "%s", connector->echo_quote);
    pthread_mutex_unlock(&connector->echo_mutex);

    if (gen != connector->rx_session_gen || !connector->connected)
        return;   // that session is gone

    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%MZ", &tm);

    if (connector->snr_valid)
    {
        int v = connector->snr_x10;
        snprintf(snr, sizeof(snr), "%s%d.%d dB", v < 0 ? "-" : "+", abs(v) / 10, abs(v) % 10);
    }
    else
        snprintf(snr, sizeof(snr), "not reported");
    if (connector->bps > 0)
        snprintf(rate, sizeof(rate), "%d bps", (int) connector->bps);
    else
        snprintf(rate, sizeof(rate), "not reported");

    int len = snprintf(text, sizeof(text),
                       "ECHO %s de %s\n"
                       "Copied you at %s\n"
                       "Your signal here: SNR %s, speed %s\n"
                       "You sent: %s\n"
                       "73\n",
                       connector->guest_call, connector->call_sign, when, snr, rate,
                       quote[0] ? quote : "(empty ping)");
    if (len < 0 || (size_t) len >= sizeof(text))
        len = (int) strlen(text);

    uint32_t frame_size = (uint32_t) (sizeof(name) - 1 + (size_t) len);
    write_buffer(&connector->in_buffer, (uint8_t *) &frame_size, sizeof(frame_size));
    write_buffer(&connector->in_buffer, (uint8_t *) name, sizeof(name) - 1);
    write_buffer(&connector->in_buffer, (uint8_t *) text, len);
    fprintf(stderr, "Guest %s: echo reply queued (SNR %s, %s).\n", connector->guest_call, snr, rate);

    char fields[128], snr_json[16], bps_json[16];
    if (connector->snr_valid)
    {
        int v = connector->snr_x10;
        snprintf(snr_json, sizeof(snr_json), "%s%d.%d", v < 0 ? "-" : "", abs(v) / 10, abs(v) % 10);
    }
    else
        snprintf(snr_json, sizeof(snr_json), "null");
    if (connector->bps > 0)
        snprintf(bps_json, sizeof(bps_json), "%d", (int) connector->bps);
    else
        snprintf(bps_json, sizeof(bps_json), "null");
    snprintf(fields, sizeof(fields), "\"event\":\"echo\",\"call\":\"%s\",\"snr\":%s,\"bps\":%s",
             connector->guest_call, snr_json, bps_json);
    event_line(connector, fields);
}
