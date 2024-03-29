/* Rhizo-connector: A connector to different HF modems
 * Copyright (C) 2020 Rhizomatica
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

#ifndef HAVE_SERIAL_H__
#define HAVE_SERIAL_H__

#include <stdbool.h>

#define MAX_MODEM_PATH 4096
#define MAX_BUF_SIZE 4096

#define RADIO_TYPE_SHM 0

void key_on();
void key_off();

#endif // HAVE_SERIAL_H__
