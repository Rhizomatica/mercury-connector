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
 * @file spool.h
 * @author Rafael Diniz
 * @date 12 Apr 2018
 * @brief Spool directory routines
 *
 * Spool directory routines
 *
 */

#ifndef HAVE_SPOOL_H__
#define HAVE_SPOOL_H__

#include <stdint.h>
#include "connector.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BUFFER_SIZE 8192

// how many ".N" names spool_store tries before giving up on a name
#define SPOOL_MAX_NAME_SUFFIX 1000

// spool_store() results
#define SPOOL_FAILED 0
#define SPOOL_STORED 1
#define SPOOL_DUPLICATE 2

int spool_store(rhizo_conn *connector, const char *name, const uint8_t *data, size_t len,
                char *stored_name, size_t stored_name_size);

void *spool_output_directory_thread(void *conn);
void *spool_input_directory_thread(void *conn);
bool queue_msg_path(char *msg_path, rhizo_conn *connector);
bool remove_all_msg_path_queue(rhizo_conn *connector);

// multi-peer spool routines
int find_peer(rhizo_conn *connector, const char *call_sign);
bool spool_create_peer_dirs(rhizo_conn *connector);
bool spool_peer_has_mail(rhizo_conn *connector, int peer_idx);
int spool_load_peer(rhizo_conn *connector, int peer_idx);
void spool_reset_tx(rhizo_conn *connector);
int spool_load_call(rhizo_conn *connector, const char *call_sign);

#endif /* HAVE_SPOOL__ */
