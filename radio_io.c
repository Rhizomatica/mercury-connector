/*
 * Copyright (C) 2020 Rhizomatica <rafael@rhizomatica.org>
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
 * Rhizo-HF-Connector
 *
 */

#include <sys/types.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <strings.h>
#include <asm/termbits.h>
#include <errno.h>
#include <threads.h>
#include <stdint.h>

#include "connector.h"
#include "radio_io.h"
#include "sbitx_io.h"
#include "radio_cmds.h"

extern rhizo_conn *tmp_conn;
extern controller_conn *sbitx_connector;
extern RIG *radio;

void key_on()
{

    if (tmp_conn->radio_type == RADIO_TYPE_SHM)
    {
        uint8_t srv_cmd[5];
        uint8_t response[5];

        memset(srv_cmd, 0, 5);
        memset(response, 0, 5);

        srv_cmd[4] = CMD_PTT_ON;

        radio_cmd(sbitx_connector, srv_cmd, response);
    }
    else
    {
        rig_set_ptt(radio, RIG_VFO_CURR, RIG_PTT_ON );
    }
}

void key_off()
{

    if (tmp_conn->radio_type == RADIO_TYPE_SHM)
    {
        uint8_t srv_cmd[5];
        uint8_t response[5];

        memset(srv_cmd, 0, 5);
        memset(response, 0, 5);

        srv_cmd[4] = CMD_PTT_OFF;

        radio_cmd(sbitx_connector, srv_cmd, response);
    }
    else
    {
        rig_set_ptt(radio, RIG_VFO_CURR, RIG_PTT_OFF );
    }
}
