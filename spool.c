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

bool write_message_to_buffer(char *msg_path, rhizo_conn *connector){
    uint8_t buffer[BUFFER_SIZE];
    char *base_path;
    FILE *f_in;
    uint32_t msg_size;
    uint32_t msg_path_size = 0;

    f_in = fopen(msg_path,"r");
    if (f_in == NULL){
        fprintf(stderr, "write_message_to_buffer: Message %s could not be opened.\n", msg_path);
        return false;
    }

    struct stat st;
    stat(msg_path, &st);
    msg_size = (uint32_t) st.st_size;

    base_path = basename(msg_path);

    fprintf(stderr, "Loaded message %s with payload size %u.\n", base_path, msg_size);

    msg_path_size = strlen(base_path)+1;
    msg_size += msg_path_size;
    base_path[msg_path_size-1] = '\n'; // adding a \n to the 0 in the string (just to make the message clear over the air)

    // our 4 byte header which contains the size of the message
    write_buffer(&connector->in_buffer, (uint8_t *) &msg_size, sizeof(msg_size));

    // the file path basename
    write_buffer(&connector->in_buffer, (uint8_t *) base_path, msg_path_size);

    size_t read_count = 0;
    uint32_t total_read = 0;
    while ((read_count = fread(buffer, 1, sizeof(buffer), f_in)) > 0){
        total_read += read_count;

        // fprintf(stderr, "writing to buffer msg of size %u tx now: %lu\n", msg_size, read_count);
        write_buffer(&connector->in_buffer, buffer, read_count);
    }

    if (total_read + msg_path_size != msg_size){
        fprintf(stderr, "Warning: possible truncated message. FIXME! total_read = %u msg_size = %u\n", total_read + msg_path_size, msg_size);
    }

    fclose(f_in);

    base_path[msg_path_size-1] = 0; // reverting to a null terminated string
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

/* Read one COMPLETE frame (4-byte length, "filename\n", payload) from the
 * out buffer and store it in the output spool. The file is written under a
 * hidden temp directory first and then hard-linked into place, so readers
 * of the spool never see a partially written file, and an existing file is
 * never overwritten: an identical copy (a resend after a lost ack) is
 * dropped, a different one gets a ".N" suffix. */
bool read_message_from_buffer(rhizo_conn *connector){
    uint32_t frame_size = 0;
    char name[MAX_RX_NAME_LEN + 1];
    char tmp_path[1400], final_path[1400];
    static unsigned long tmp_counter = 0;

    read_buffer(&connector->out_buffer, (uint8_t *) &frame_size, sizeof(frame_size));
    if (frame_size == 0)
        return false;

    uint8_t *frame = (uint8_t *) malloc(frame_size);
    if (frame == NULL){
        // can't hold it, but must still consume it to keep the buffer framed
        uint8_t sink[4096];
        uint32_t left = frame_size;
        while (left){
            uint32_t chunk = left > sizeof(sink) ? sizeof(sink) : left;
            read_buffer(&connector->out_buffer, sink, chunk);
            left -= chunk;
        }
        fprintf(stderr, "Out of memory: dropped a received message of %u bytes.\n", frame_size);
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

    snprintf(tmp_path, sizeof(tmp_path), "%s.rx-tmp/%s.%ld.%lu", connector->output_directory, name,
             (long) getpid(), tmp_counter++);

    FILE *fp = fopen(tmp_path, "w");
    if (fp == NULL){
        fprintf(stderr, "read_message_from_buffer: %s could not be opened (%s).\n", tmp_path, strerror(errno));
        free(frame);
        return false;
    }
    bool ok = fwrite(payload, 1, payload_len, fp) == payload_len;
    ok &= fflush(fp) == 0;
    ok &= fsync(fileno(fp)) == 0;
    ok &= fclose(fp) == 0;
    if (!ok){
        fprintf(stderr, "read_message_from_buffer: error writing %s (%s).\n", tmp_path, strerror(errno));
        unlink(tmp_path);
        free(frame);
        return false;
    }

    // publish without ever overwriting: link() fails if the name exists
    bool stored = false;
    for (int suffix = 0; suffix < 1000 && !stored; suffix++){
        if (suffix == 0)
            snprintf(final_path, sizeof(final_path), "%s%s", connector->output_directory, name);
        else
            snprintf(final_path, sizeof(final_path), "%s%s.%d", connector->output_directory, name, suffix);

        if (link(tmp_path, final_path) == 0){
            stored = true;
            fprintf(stderr, "Incoming message %s with payload size %zu.\n", basename(final_path), payload_len);
        }
        else if (errno == EEXIST){
            if (file_has_content(final_path, payload, payload_len)){
                fprintf(stderr, "Incoming message %s is a duplicate of one already received, dropped.\n", name);
                break;
            }
        }
        else{
            fprintf(stderr, "read_message_from_buffer: could not store %s (%s).\n", final_path, strerror(errno));
            break;
        }
    }
    unlink(tmp_path);
    free(frame);
    return stored;
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
