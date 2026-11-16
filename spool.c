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
 * Spool directory routines
 */

/**
 * @file spool.c
 * @author Rafael Diniz
 * @date 12 Apr 2018
 * @brief Spool directory routines
 *
 * Spool directory routines
 *
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/inotify.h>
#include <libgen.h>
#include <errno.h>
#include <strings.h>
#include <time.h>

#include "spool.h"


/** @brief Queue a message path to the list of sent message paths
 *         (waiting for an ack to be deleted)
 *
 *  @param msg_path Message path.
 *  @param connector Connector handle.
 *  @return bool Returns true in case of success.
 */
bool queue_msg_path(char *msg_path, rhizo_conn *connector){

    pthread_mutex_lock(&connector->msg_path_queue_mutex);

    if (connector->msg_path_queue_size >= MSG_PATH_QUEUE_MAX){
        pthread_mutex_unlock(&connector->msg_path_queue_mutex);
        fprintf(stderr, "Message queue full, %s not queued.\n", msg_path);
        return false;
    }

    connector->msg_path_queue[connector->msg_path_queue_size] = (char *) malloc(strlen(msg_path)+1);
    strcpy(connector->msg_path_queue[connector->msg_path_queue_size], msg_path);
    connector->msg_path_queue_size++;

    pthread_mutex_unlock(&connector->msg_path_queue_mutex);

    return true;

}

bool remove_all_msg_path_queue(rhizo_conn *connector){
    pthread_mutex_lock(&connector->msg_path_queue_mutex);

    for(int i = 0; i < connector->msg_path_queue_size; i++){
        if (unlink(connector->msg_path_queue[i]) != 0){
            fprintf(stderr, "File %s could not be deleted!\n", connector->msg_path_queue[i]);
        }
        free(connector->msg_path_queue[i]);
    }
    connector->msg_path_queue_size = 0;
    // add path to connector->msg_path_queue
    pthread_mutex_unlock(&connector->msg_path_queue_mutex);

    return true;

}

/* Load one file into the transmit buffer as a single frame.
 *
 * The whole file is read into memory first, so the frame header always
 * matches what follows it even if the file changes underneath us, and the
 * frame goes into the ring in one piece or not at all:
 *  - a file whose frame would be over MAX_RX_MSG_SIZE is never sent (the
 *    other side would reject it anyway);
 *  - if the ring has no room for the whole frame right now, nothing is
 *    written and the file is picked up again later in the session, once the
 *    modem has taken some of the backlog. Session setup loads with tx parked,
 *    so a blocking write here would wait forever for space that only the
 *    parked tx thread can free. */
bool write_message_to_buffer(char *msg_path, rhizo_conn *connector){
    char name[256];
    struct stat st;

    // every frame we send must be tracked, or it can't be deleted on BUFFER 0
    if (connector->msg_path_queue_size >= MSG_PATH_QUEUE_MAX){
        fprintf(stderr, "Message queue full, %s will be sent later.\n", msg_path);
        return false;
    }

    if (stat(msg_path, &st) != 0 || !S_ISREG(st.st_mode)){
        fprintf(stderr, "write_message_to_buffer: %s is not a readable file, skipped.\n", msg_path);
        return false;
    }

    const char *slash = strrchr(msg_path, '/');
    snprintf(name, sizeof(name), "%s", slash ? slash + 1 : msg_path);
    size_t name_len = strlen(name) + 1;                 // name + '\n'
    uint64_t frame_len = (uint64_t) name_len + (uint64_t) st.st_size;

    if (frame_len > MAX_RX_MSG_SIZE){
        fprintf(stderr, "%s is too big to send (%lld bytes, limit %u): skipped.\n",
                msg_path, (long long) st.st_size, MAX_RX_MSG_SIZE - (unsigned) name_len);
        return false;
    }

    if (ring_buffer_count_free_bytes(&connector->in_buffer.buf) < sizeof(uint32_t) + frame_len){
        // not an error: retried on the next scan, after the modem drains some
        return false;
    }

    uint8_t *frame = (uint8_t *) malloc(sizeof(uint32_t) + frame_len);
    if (frame == NULL){
        fprintf(stderr, "Out of memory loading %s, will retry.\n", msg_path);
        return false;
    }

    FILE *f_in = fopen(msg_path, "r");
    if (f_in == NULL){
        fprintf(stderr, "write_message_to_buffer: Message %s could not be opened (%s).\n",
                msg_path, strerror(errno));
        free(frame);
        return false;
    }

    uint8_t *payload = frame + sizeof(uint32_t) + name_len;
    size_t got = fread(payload, 1, (size_t) st.st_size, f_in);
    bool more = (fgetc(f_in) != EOF);
    fclose(f_in);
    if (got != (size_t) st.st_size || more){
        // changed while we read it: try again next scan
        fprintf(stderr, "%s changed while loading, will retry.\n", msg_path);
        free(frame);
        return false;
    }

    uint32_t msg_size = (uint32_t) frame_len;
    memcpy(frame, &msg_size, sizeof(msg_size));
    memcpy(frame + sizeof(uint32_t), name, name_len - 1);
    frame[sizeof(uint32_t) + name_len - 1] = '\n';   // name ends with \n, readable over the air

    fprintf(stderr, "Loaded message %s with payload size %lld.\n", name, (long long) st.st_size);

    write_buffer(&connector->in_buffer, frame, (int) (sizeof(uint32_t) + frame_len));
    free(frame);

    queue_msg_path(msg_path, connector);
    return true;
}

/* Make a received filename safe to use in the output spool: keep only the
 * last path component, allow [A-Za-z0-9._-], no leading dots (hidden files
 * are reserved for our own temp files), bounded length. Falls back to a
 * generated name if nothing usable is left. */
static void sanitize_rx_name(const char *in, size_t in_len, char *out, size_t out_size)
{
    static unsigned long counter = 0;
    size_t start = 0, n = 0;

    for (size_t i = 0; i < in_len; i++)
        if (in[i] == '/' || in[i] == '\\')
            start = i + 1;

    for (size_t i = start; i < in_len && n < MAX_RX_NAME_LEN && n + 1 < out_size; i++)
    {
        char c = in[i];
        if (n == 0 && (c == '.' || c == '-'))
            continue;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-')
            out[n++] = c;
        else
            out[n++] = '_';
    }
    out[n] = 0;

    if (n == 0)
        snprintf(out, out_size, "rx_%ld_%lu.msg", (long) time(NULL), counter++);
}

/* true if the file at path holds exactly data[0..len) */
static bool file_has_content(const char *path, const uint8_t *data, size_t len)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || (size_t) st.st_size != len)
        return false;

    FILE *f = fopen(path, "r");
    if (f == NULL)
        return false;

    uint8_t buf[4096];
    size_t off = 0, r;
    bool same = true;
    while (same && (r = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        if (off + r > len || memcmp(buf, data + off, r) != 0)
            same = false;
        off += r;
    }
    fclose(f);
    return same && off == len;
}

/* Store data[0..len) in the output spool as <name>. The file is written
 * under a hidden temp directory first and then hard-linked into place, so
 * readers of the spool never see a partially written file, and an existing
 * file is never overwritten: an identical copy (a resend after a lost ack)
 * is dropped, a different one gets a ".N" suffix. Used for normal received
 * mail and for guest messages alike. On SPOOL_STORED, the name actually
 * used is written to stored_name (if not NULL). */
int spool_store(rhizo_conn *connector, const char *name, const uint8_t *data, size_t len,
                char *stored_name, size_t stored_name_size)
{
    static atomic_ulong tmp_counter = 0;
    char tmp_path[1400], final_path[1400];

    snprintf(tmp_path, sizeof(tmp_path), "%s.rx-tmp/%s.%ld.%lu", connector->output_directory, name,
             (long) getpid(), (unsigned long) tmp_counter++);

    FILE *fp = fopen(tmp_path, "w");
    if (fp == NULL){
        fprintf(stderr, "spool_store: %s could not be opened (%s).\n", tmp_path, strerror(errno));
        return SPOOL_FAILED;
    }
    bool ok = fwrite(data, 1, len, fp) == len;
    ok &= fflush(fp) == 0;
    ok &= fsync(fileno(fp)) == 0;
    ok &= fclose(fp) == 0;
    if (!ok){
        fprintf(stderr, "spool_store: error writing %s (%s).\n", tmp_path, strerror(errno));
        unlink(tmp_path);
        return SPOOL_FAILED;
    }

    // publish without ever overwriting: link() fails if the name exists
    int result = SPOOL_FAILED;
    for (int suffix = 0; suffix < SPOOL_MAX_NAME_SUFFIX; suffix++){
        if (suffix == 0)
            snprintf(final_path, sizeof(final_path), "%s%s", connector->output_directory, name);
        else
            snprintf(final_path, sizeof(final_path), "%s%s.%d", connector->output_directory, name, suffix);

        if (link(tmp_path, final_path) == 0){
            result = SPOOL_STORED;
            if (stored_name != NULL)
                snprintf(stored_name, stored_name_size, "%s", basename(final_path));
            break;
        }
        if (errno != EEXIST){
            fprintf(stderr, "spool_store: could not store %s (%s).\n", final_path, strerror(errno));
            break;
        }
        if (file_has_content(final_path, data, len)){
            result = SPOOL_DUPLICATE;
            break;
        }
    }
    unlink(tmp_path);
    return result;
}

/* Read one COMPLETE frame (4-byte length, "filename\n", payload) from the
 * out buffer and store it in the output spool (see spool_store). */
bool read_message_from_buffer(rhizo_conn *connector){
    uint32_t frame_size = 0;
    char name[MAX_RX_NAME_LEN + 1];
    char stored[MAX_RX_NAME_LEN + 16];

    read_buffer(&connector->out_buffer, (uint8_t *) &frame_size, sizeof(frame_size));
    if (frame_size == 0)
        return false;

    // the length comes from the remote station: never allocate what it asks
    // for beyond MAX_RX_MSG_SIZE
    uint8_t *frame = frame_size <= MAX_RX_MSG_SIZE ? (uint8_t *) malloc(frame_size) : NULL;
    if (frame == NULL){
        // too big, or can't hold it, but must still consume it to keep the buffer framed
        uint8_t sink[4096];
        uint32_t left = frame_size;
        while (left){
            uint32_t chunk = left > sizeof(sink) ? sizeof(sink) : left;
            read_buffer(&connector->out_buffer, sink, chunk);
            left -= chunk;
        }
        fprintf(stderr, "Dropped a received message of %u bytes (%s).\n", frame_size,
                frame_size > MAX_RX_MSG_SIZE ? "too big" : "out of memory");
        return false;
    }
    read_buffer(&connector->out_buffer, frame, frame_size);

    // filename is everything up to the first '\n' (searched within a bounded prefix)
    size_t scan = frame_size < 512 ? frame_size : 512;
    uint8_t *nl = memchr(frame, '\n', scan);
    const uint8_t *payload;
    size_t payload_len;

    if (nl != NULL){
        sanitize_rx_name((const char *) frame, (size_t) (nl - frame), name, sizeof(name));
        payload = nl + 1;
        payload_len = frame_size - (size_t) (nl - frame) - 1;
    }
    else{
        fprintf(stderr, "Received message has no filename line, saving it under a generated name.\n");
        sanitize_rx_name("", 0, name, sizeof(name));
        payload = frame;
        payload_len = frame_size;
    }

    int r = spool_store(connector, name, payload, payload_len, stored, sizeof(stored));
    if (r == SPOOL_STORED)
        fprintf(stderr, "Incoming message %s with payload size %zu.\n", stored, payload_len);
    else if (r == SPOOL_DUPLICATE)
        fprintf(stderr, "Incoming message %s is a duplicate of one already received, dropped.\n", name);
    free(frame);
    return r == SPOOL_STORED;
}

void *spool_output_directory_thread(void *conn)
{
    rhizo_conn *connector = (rhizo_conn *) conn;
    char tmp_dir[1100];

    DIR *dirp = opendir(connector->output_directory);
    if (dirp == NULL){
        fprintf(stderr, "Directory \"%s\" could not be opened.\n", connector->output_directory);
        return NULL;
    }
    closedir(dirp);

    // temp area for received files; hidden, and a directory, so spool
    // consumers that only look at regular files never pick it up
    snprintf(tmp_dir, sizeof(tmp_dir), "%s.rx-tmp", connector->output_directory);
    if (mkdir(tmp_dir, 0775) != 0 && errno != EEXIST){
        fprintf(stderr, "Could not create %s (%s).\n", tmp_dir, strerror(errno));
        return NULL;
    }

    while (true){
        read_message_from_buffer(connector);
    }

    return NULL;
}

void *spool_input_directory_thread(void *conn)
{
    rhizo_conn *connector = (rhizo_conn *) conn;
    struct dirent *dp;
    char msg_path[1024];

    DIR *dirp = opendir(connector->input_directory);
    if (dirp == NULL){
        fprintf(stderr, "Directory \"%s\" could not be opened.\n", connector->input_directory);
        return NULL;
    }
    while ((dp = readdir(dirp)) != NULL){
        if (dp->d_type == DT_REG){
            strcpy(msg_path, connector->input_directory);
            strcat(msg_path, dp->d_name);
            write_message_to_buffer(msg_path, connector);
        }
    }
    closedir(dirp);


    // now starts inotify marvelous...
#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))

    int length, i = 0;
    int fd;
    int wd;
    char buffer_inot[BUF_LEN];

    fd = inotify_init();

    if (fd < 0) {
        perror("inotify_init");
    }

    wd = inotify_add_watch(fd, connector->input_directory,
                           IN_MOVED_TO | IN_CLOSE_WRITE);

    while(true){
        i = 0;
        length = read(fd, buffer_inot, BUF_LEN);

        if (length < 0) {
            perror("read");
        }

        while (i < length) {
            struct inotify_event *event =
                (struct inotify_event *) &buffer_inot[i];
            if (event->len) {
                if ((event->mask & IN_CLOSE_WRITE) || (event->mask & IN_MOVED_TO)) {
                    strcpy(msg_path, connector->input_directory);
                    strcat(msg_path, event->name);
                    write_message_to_buffer(msg_path, connector);
                }


            }
            i += EVENT_SIZE + event->len;
        }

    }
    inotify_rm_watch(fd, wd);
    close(fd);

    fprintf(stderr, "Spool is finishing...\n");

    return NULL;
}


/* ------------------------------------------------------------------------
 * Multi-peer support (VARA/Mercury)
 *
 * Outgoing mail for each remote station lives in its own subdirectory of the
 * input spool:  <input_directory>/<CALLSIGN>/   Files placed directly in
 * <input_directory> go to the default peer (the first -d callsign), so a
 * single-peer setup behaves exactly as before.
 *
 * Mail is only loaded into the transmit buffer once a session is established
 * and the remote callsign is known, and only that station's mail is loaded.
 * When a session ends, anything not yet acknowledged is dropped from the
 * buffer WITHOUT deleting the files, so it is retried on the next session
 * with the right station.
 * ------------------------------------------------------------------------ */

int find_peer(rhizo_conn *connector, const char *call_sign)
{
    if (call_sign == NULL || call_sign[0] == 0)
        return -1;
    for (int i = 0; i < connector->num_peers; i++)
    {
        if (!strcasecmp(connector->peers[i], call_sign))
            return i;
    }
    return -1;
}

static void peer_dir(rhizo_conn *connector, int peer_idx, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s%s/", connector->input_directory, connector->peers[peer_idx]);
}

bool spool_create_peer_dirs(rhizo_conn *connector)
{
    char dir[1100];
    bool ok = true;

    for (int i = 0; i < connector->num_peers; i++)
    {
        peer_dir(connector, i, dir, sizeof(dir));
        if (mkdir(dir, 0775) != 0 && errno != EEXIST)
        {
            fprintf(stderr, "Could not create peer spool directory %s: %s\n", dir, strerror(errno));
            ok = false;
        }
        else
        {
            fprintf(stderr, "Peer %s%s: outgoing spool %s\n", connector->peers[i],
                    i == 0 ? " (default)" : "", dir);
        }
    }
    return ok;
}

static bool path_is_queued(rhizo_conn *connector, const char *path)
{
    bool found = false;
    pthread_mutex_lock(&connector->msg_path_queue_mutex);
    for (size_t i = 0; i < connector->msg_path_queue_size; i++)
    {
        if (!strcmp(connector->msg_path_queue[i], path))
        {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&connector->msg_path_queue_mutex);
    return found;
}

/* Walk one directory. If load is false, return 1 as soon as one eligible file
 * is found. If load is true, load every eligible file and return the count. */
static int scan_dir(rhizo_conn *connector, const char *dir, bool load)
{
    char path[2048];
    struct stat st;
    struct dirent *dp;
    int count = 0;
    time_t now = time(NULL);

    DIR *dirp = opendir(dir);
    if (dirp == NULL)
        return 0;

    while ((dp = readdir(dirp)) != NULL)
    {
        if (dp->d_name[0] == '.')            // hidden / temporary files
            continue;

        snprintf(path, sizeof(path), "%s%s", dir, dp->d_name);

        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (now - st.st_mtime < SPOOL_SETTLE_S) // probably still being written
            continue;
        if (path_is_queued(connector, path))  // already in the buffer this session
            continue;

        if (!load)
        {
            count = 1;
            break;
        }

        if (connector->msg_path_queue_size >= MSG_PATH_QUEUE_MAX)
        {
            fprintf(stderr, "Message queue full, remaining files will be sent next session.\n");
            break;
        }

        if (write_message_to_buffer(path, connector))
            count++;
    }
    closedir(dirp);
    return count;
}

bool spool_peer_has_mail(rhizo_conn *connector, int peer_idx)
{
    char dir[1100];

    if (peer_idx < 0 || peer_idx >= connector->num_peers)
        return false;

    peer_dir(connector, peer_idx, dir, sizeof(dir));
    if (scan_dir(connector, dir, false))
        return true;

    if (peer_idx == 0 && scan_dir(connector, connector->input_directory, false))
        return true;

    return false;
}

int spool_load_peer(rhizo_conn *connector, int peer_idx)
{
    char dir[1100];
    int count = 0;

    if (peer_idx < 0 || peer_idx >= connector->num_peers)
        return 0;

    peer_dir(connector, peer_idx, dir, sizeof(dir));
    count += scan_dir(connector, dir, true);

    if (peer_idx == 0)
        count += scan_dir(connector, connector->input_directory, true);

    return count;
}

/* No -d at all (num_peers == 0): like master, the input spool itself goes
 * to whichever station connects. */
bool spool_any_has_mail(rhizo_conn *connector)
{
    return scan_dir(connector, connector->input_directory, false) > 0;
}

int spool_load_any(rhizo_conn *connector)
{
    return scan_dir(connector, connector->input_directory, true);
}

/* Guests: load <input_directory>/<CALL>/ (call already validated). */
int spool_load_call(rhizo_conn *connector, const char *call_sign)
{
    char dir[1100];
    snprintf(dir, sizeof(dir), "%s%s/", connector->input_directory, call_sign);
    return scan_dir(connector, dir, true);
}

/* Drop everything not yet handed to the modem and forget the queued paths
 * WITHOUT deleting the files. Caller must make sure the data tx thread is
 * parked (tx_hold && tx_idle). */
void spool_reset_tx(rhizo_conn *connector)
{
    pthread_mutex_lock(&connector->in_buffer.mutex);
    ring_buffer_clear(&connector->in_buffer.buf);
    pthread_mutex_unlock(&connector->in_buffer.mutex);

    pthread_mutex_lock(&connector->msg_path_queue_mutex);
    for (size_t i = 0; i < connector->msg_path_queue_size; i++)
        free(connector->msg_path_queue[i]);
    connector->msg_path_queue_size = 0;
    pthread_mutex_unlock(&connector->msg_path_queue_mutex);
}
