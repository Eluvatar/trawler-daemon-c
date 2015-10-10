trawler-daemon-c
================

Daemon to throttle requests as part of trawler protocol

### Compilation

The daemon depends on zmq, czmq, curl, and protobuf-c. I.E:

```sh
$ sudo dnf install zeromq3 zeromq3-devel czmq czmq-devel libcurl libcurl-devel protobuf-c protobuf-c-devel
```

The daemon is compiled using make.

```sh
$ make 
```

### Running

The daemon does not self-daemonize. Use a terminal multiplexer, daemonizing script, init scripts, or even systemd to run it in the background.
