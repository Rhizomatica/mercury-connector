# Mercury-connector

  Mercury-connector is a file exchange solution compatible with different
  HF modems. Used especially for testing and comparisson purposes, it supports
  hamlib and HERMES's shared memory interfaces for radio's keying.

  Mercury-connector connects a modem/TNC for transmitting or receiving files,
  through an inotify-driven interface. Mercury-connector expects a file to be
  copied to a specified input directory, and automatically transmits it throught the TNC, 
  while received files are written to a specified output directory.

  Support for the following TNCs are implemented: Ardop (works
  in both normal ardop or experimental ofdm mode), VARA and Mercury.

## Compilation and Installation

On a Debian based system (e.g., Debian, Ubuntu), the dependencies can be installed with:

```
apt-get install uthash-dev libhamlib-dev
```

To compile, use:

```
make
```

To install:

```
sudo make install
```

## Usage

| Option                    | Description                                                                                  |
|---------------------------|----------------------------------------------------------------------------------------------|
| -x [ardop,vara,mercury]   | Choose modem/TNC type                                                                        |
| -i input_spool_directory  | Input spool directory (Messages to send)                                                     |
| -o output_spool_directory | Output spool directory (Received messages)                                                   |
| -c callsign               | Station Callsign (Eg: PU2HFF)                                                                |
| -d remote_callsign        | Remote Station Callsign                                                                      |
| -a tnc_ip_address         | IP address of the TNC                                                                        |
| -p tcp_base_port          | TCP base port of the TNC. Control port is tcp_base_port and tcp_base_port+1 is the data port |
| -t timeout                | Time to wait before disconnect when idling                                                   |
| -f features               | Enable/Disable features. Supported features: ofdm, noofdm.                                   |
| -m radio_model            | Sets HAMLIB radio model                                                                      |
| -r radio_address          | Sets HAMLIB radio device file or ip:port address                                             |
| -s                        | Use HERMES's shared memory interface instead of HAMLIB (Do not use -r and -m in this case)   |
| -l                        | List HAMLIB supported radio models                                                           |
| -h                        | Prints this help                                                                             |

### Ardop

Example of invocation command of Ardop connected to an ICOM IC-7100, using base port 8515:

    $ ardop1ofdm 8515 -c /dev/ttyUSB0 ARDOP ARDOP -k FEFE88E01C0001FD -u FEFE88E01C0000FD

ps: as a note, for an Yaesu FT-991 use " -k 5458313B -u 5458303B ".

With the following ALSA configuration (global-wide ALSA configuration in "/etc/asound.conf"): 

    pcm.ARDOP {type rate slave {pcm "hw:1,0" rate 48000}}

Associated mercury-connector command example:

    $ mercury-connector -x ardop -i /var/spool/outgoing_messages/ -o /var/spool/incoming_messages/ -c BB2UIT -d PP2UIT -a 127.0.0.1 -p 8515 -t 60

### Vara

Example for running mercury-connector with VARA modem, on base port 8300, and keying an ICOM IC-7100 using hamlib:

    $ mercury-connector -r vara -i l1/ -o l2/ -c BB2ITU -d UU2ITU -a 127.0.0.1 -p 8300 -t 60 -r /dev/ttyUSB0 -m 370

### Mercury

Example for running mercury-connector with Mercury modem, on base port 7002, and keying a sBitx through HERMES's shared memory interface:

    $ mercury-connector  -x mercury -i in/ -o out/ -c PY4ZIT -d PY4ABC -a 127.0.0.1 -p 7002 -s

## Author

Rafael Diniz <rafael (AT) rhizomatica (DOT) org>
