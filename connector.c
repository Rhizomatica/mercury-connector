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
 * @file connector.c
 * @author Rafael Diniz
 * @date 12 Apr 2018
 * @brief Rhizo HF Connector main file
 *
 * Rhizo HF Connector main C file.
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
#include <sys/socket.h>
#include <dirent.h>

#include <hamlib/config.h>
#include <hamlib/rig.h>

#include "rigctl_parse.h"
#include "connector.h"
#include "spool.h"
#include "vara.h"
#include "ardop.h"
#include "radio_io.h"
#include "sbitx_io.h"
#include "shm_utils.h"

// global variables for access from anywhere
rhizo_conn *tmp_conn = NULL;
controller_conn *sbitx_connector = NULL;
RIG *radio;

void finish(int s){
    fprintf(stderr, "\nExiting...\n");

    /* Do house cleaning work here */
    if (tmp_conn){
        if (tmp_conn->serial_keying)
        {
            key_off(tmp_conn->serial_fd, tmp_conn->radio_type);
            close(tmp_conn->serial_fd);
        }

        if (tmp_conn->data_socket){
            shutdown(tmp_conn->data_socket, SHUT_RDWR);
            close (tmp_conn->data_socket);
        }
        if (tmp_conn->control_socket){
            shutdown(tmp_conn->control_socket, SHUT_RDWR);
            close (tmp_conn->control_socket);
        }
    }

    exit(EXIT_SUCCESS);
}

void *modem_thread(void *conn)
{
    rhizo_conn *connector = (rhizo_conn *) conn;

#if 0
    if (!strcmp("mercury", connector->modem_type))
    {
        initialize_modem_mercury(connector);
    }
#endif

    if (!strcmp("vara", connector->modem_type))
    {
        initialize_modem_vara(connector);
    }

    if (!strcmp("ardop", connector->modem_type))
    {
        initialize_modem_ardop(connector);
    }

    return NULL;
}

bool initialize_connector(rhizo_conn *connector){

    initialize_buffer(&connector->in_buffer, 26); // 64MB
    initialize_buffer(&connector->out_buffer, 26); // 64MB
    pthread_mutex_init(&connector->msg_path_queue_mutex, NULL);

    connector->serial_path[0] = 0;
    connector->connected = false;
    connector->waiting_for_connection = false;
    connector->serial_keying = false;
    connector->radio_type = 0;
    connector->tcp_ret_ok = true;
    connector->serial_fd = -1;
    connector->msg_path_queue_size = 0;
    connector->safe_state = 0;

    connector->timeout = TIMEOUT_DEFAULT;
    connector->ofdm_mode = true;
    connector->buffer_size = 0;
    return true;
}


int main (int argc, char *argv[])
{
    rhizo_conn connector;

    tmp_conn = &connector;

    initialize_connector(&connector);

    // Catch Ctrl+C
    signal (SIGINT,finish);

    // hamlib stuff
#if 0 // lets just use defaults for now...
    ptt_type_t ptt_type = RIG_PTT_NONE;
    dcd_type_t dcd_type = RIG_DCD_NONE;
#endif

    // unset this after done
    rig_set_debug(RIG_DEBUG_TRACE);


    fprintf(stderr, "Mercury Connector version 0.5\n");
    fprintf(stderr, " by Rafael Diniz - rafael (AT) rhizomatica (DOT) org\n");
    fprintf(stderr, "License: GPLv3+\n\n");

    if (argc < 2)
    {
    manual:
        fprintf(stderr, "Usage modes: \n%s -r radio_modem_type -i input_spool_directory -o output_spool_directory -c callsign -d remote_callsign -a tnc_ip_address -p tcp_base_port\n", argv[0]);
        fprintf(stderr, "%s -h\n", argv[0]);
        fprintf(stderr, "\nOptions:\n");
        fprintf(stderr, " -x [mercury,ardop,vara]           Choose modem/radio type.\n");
        fprintf(stderr, " -i input_spool_directory    Input spool directory (Messages to send).\n");
        fprintf(stderr, " -o output_spool_directory    Output spool directory (Received messages).\n");
        fprintf(stderr, " -c callsign                        Station Callsign (Eg: PU2HFF).\n");
        fprintf(stderr, " -d remote_callsign           Remote Station Callsign.\n");
        fprintf(stderr, " -a tnc_ip_address            IP address of the TNC,\n");
        fprintf(stderr, " -p tcp_base_port              TCP base port of the TNC. For VARA and ARDOP ports tcp_base_port and tcp_base_port+1 are used,\n");
        fprintf(stderr, " -t timeout                 Time to wait before disconnect when idling.\n");
        fprintf(stderr, " -f features                Enable/Disable features. Supported features: ofdm, noofdm (ARDOP ONLY).\n");
        fprintf(stderr, " -m [radio_model]           Sets HAMLIB radio model\n");
        fprintf(stderr, " -r [radio_address]         Sets HAMLIB radio device file or ip:port address\n");
        fprintf(stderr, " -s                         Use HERMES's shared memory interface instead of HAMLIB\n");
        fprintf(stderr, " -l                         List HAMLIB supported radio models\n");

        fprintf(stderr, " -h                          Prints this help.\n");
        exit(EXIT_FAILURE);
    }

    char *last;
    int opt;
    while ((opt = getopt(argc, argv, "hr:si:o:c:d:p:a:t:f:s:x:m:l")) != -1)
    {
        switch (opt)
        {
        case 'h':
            goto manual;
            break;
        case 'l':
            list_models();
            exit(EXIT_SUCCESS);
            break;
        case 'c':
            strcpy(connector.call_sign, optarg);
            break;
        case 'd':
            strcpy(connector.remote_call_sign, optarg);
            break;
        case 't':
            connector.timeout = atoi(optarg);
            break;
        case 'p':
            connector.tcp_base_port = atoi(optarg);
            break;
        case 'a':
            strcpy(connector.ip_address, optarg);
            break;
        case 'x':
            strcpy(connector.modem_type, optarg);
            break;
        case 'i':
            strcpy(connector.input_directory, optarg);
            last = &connector.input_directory[strlen(connector.input_directory)-1];
            if (last[0] != '/'){
                last[1] = '/';
                last[2] = 0;
            }
            break;
        case 'o':
            strcpy(connector.output_directory, optarg);
            last = &connector.output_directory[strlen(connector.output_directory)-1];
            if (last[0] != '/'){
                last[1] = '/';
                last[2] = 0;
            }
            break;
        case 'f':
            if(strstr(optarg, "noofdm"))
                connector.ofdm_mode = false;
            else
                connector.ofdm_mode = true;
            break;
        case 'r':
            connector.serial_keying = true;
            strcpy(connector.serial_path, optarg);
            break;
        case 'm':
            connector.radio_type = atoi(optarg);
            break;
       case 's':
            connector.radio_type = RADIO_TYPE_SHM;
            break;
        default:
            goto manual;
        }
    }

    if (connector.radio_type == RADIO_TYPE_SHM)
    {
        if (shm_is_created(SYSV_SHM_CONTROLLER_KEY_STR, sizeof(controller_conn)) == false)
        {
            fprintf(stderr, "Connector SHM not created. Is sbitx_controller running?\n");
            return EXIT_FAILURE;
        }

        sbitx_connector = (controller_conn *) shm_attach(SYSV_SHM_CONTROLLER_KEY_STR, sizeof(controller_conn));
    }
    else
    {
        radio = rig_init(connector.radio_type);
        if (!radio)
        {
            fprintf(stderr, "Unknown rig num %u, or initialization error.\n", connector.radio_type);
            fprintf(stderr, "Please check available radios with -l option.\n");
            exit(2);
        }

        if (connector.serial_path[0])
            strncpy(radio->state.rigport.pathname, connector.serial_path, HAMLIB_FILPATHLEN - 1);

        int ret;

        ret = rig_open(radio);
        if (ret != RIG_OK)
        {
            fprintf(stderr, "rig_open: error = %s %s \n", connector.serial_path, rigerror(ret));
            return EXIT_FAILURE;
        }

        if (radio->caps->rig_model == RIG_MODEL_NETRIGCTL)
        {
            /* We automatically detect if we need to be in vfo mode or not */
            int rigctld_vfo_opt = netrigctl_get_vfo_mode(radio);
            radio->state.vfo_opt = rigctld_vfo_opt;
            // rig_debug(RIG_DEBUG_TRACE, "%s vfo_opt=%d\n", __func__, vfo_opt);
        }

#if 0
    vfo_t vfo = 0;
    ret = rig_get_vfo(radio, &vfo);
    if (ret == RIG_OK)
        fprintf(stderr, "Current VFO: %d\n", vfo);
    else
        fprintf(stderr, "Error reading VFO,\n");

    freq_t freq;
    ret = rig_get_freq(radio, RIG_VFO_CURR, &freq);

    if (ret == RIG_OK )
    {
        fprintf(stderr, "rig_get_freq: freq = %f\n", freq);
    }
    else
    {
        fprintf(stderr, "rig_get_freq: error =  %s \n", rigerror(ret));
    }

    rmode_t rmode;          /* radio mode of operation */
    pbwidth_t width;
    ret = rig_get_mode(radio, RIG_VFO_CURR, &rmode, &width);

    if (ret == RIG_OK )
    {
        fprintf(stderr, "rig_get_mode: mode = %lu \n", rmode);
    }
    else
    {
        fprintf(stderr, "rig_get_mode: error =  %s \n", rigerror(ret));
    }


    ret = rig_set_mode(radio, RIG_VFO_CURR, RIG_MODE_LSB, RIG_PASSBAND_NORMAL);
    if (ret != RIG_OK )
    {
       fprintf(stderr, "rig_set_mode: error = %s \n", rigerror(ret));
    }

    ret = rig_set_ptt(radio, RIG_VFO_CURR, RIG_PTT_ON ); // should we use RIG_VFO_A ?

    if (ret != RIG_OK )
    {
        fprintf(stderr, "rig_set_ptt: error = %s \n", rigerror(ret));
    }

    ret = rig_set_ptt(radio, RIG_VFO_CURR, RIG_PTT_OFF ); /* phew ! */
    if (ret != RIG_OK )
    {
        fprintf(stderr, "rig_set_ptt: error = %s \n", rigerror(ret));
    }
#endif
    }
    pthread_t tid[3];

    pthread_create(&tid[0], NULL, spool_input_directory_thread, (void *) &connector);
    pthread_create(&tid[1], NULL, spool_output_directory_thread, (void *) &connector);

    // pthread_create(&tid[2], NULL, modem_thread, (void *) &connector);
    modem_thread((void *) &connector);

    if (connector.tcp_ret_ok == false){
        // reconnect and call modem_thread again?
        // say something to the spool threads??
        // we cant guarantee nothing about data passed to tnc... pthread_cancel? select?
        // spool needs to re-read the input directory...
    }

    if (connector.radio_type != RADIO_TYPE_SHM)
    {
        rig_close(radio);
        rig_cleanup(radio);
    }

    return EXIT_SUCCESS;
}
