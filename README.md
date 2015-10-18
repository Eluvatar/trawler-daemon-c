trawler-daemon-c
================

Daemon to throttle requests as part of trawler protocol

### Dependencies

The daemon depends on zmq, czmq, curl, and protobuf-c. I.E:

#### Fedora
```sh
$ sudo dnf install zeromq3 zeromq3-devel czmq czmq-devel libcurl libcurl-devel protobuf-c protobuf-c-devel
```

#### Debian (Untested)
```sh
$ sudo apt-get install libzmq3 libzmq3-dev libczmq3 libcmq-dev libcurl3 libcurl4-dev protobuf-c-compiler libprotobuf-c-dev
```

#### Ubuntu (Does not work)
(zmq versioning issues!) (May figure out a solution or patch trawler-daemon-c to work on Ubuntu D: )
```sh
$ sudo apt-get install libcurl3 libcurl4-openssl-dev protobuf-c-compiler libprotobuf-c0-dev
$ wget http://download.zeromq.org/zeromq-3.2.5.tar.gz
$ tar -xzf zeromq-3.2.5.tar.gz
$ cd zeromq-3.2.5
$ ./autogen.sh
$ ./configure
$ make
$ sudo make install
$ sudo ldconfig
$ cd ..
$ wget http://download.zeromq.org/czmq-1.4.1.tar.gz
$ tar -xzf czmq-1.4.1.tar.gz
$ cd czmq-1.4.1
$ ./configure
$ make all
$ sudo make install
$ sudo ldconfig
```

### Compilation

Make sure to check out the protocol submodule as well as the daemon source, otherwise you will get mysterious make errors.

```sh
$ git clone https://github.com/Eluvatar/trawler-daemon-c.git
$ cd trawler-daemon-c
$ git submodule update --init
```

The daemon is compiled using make.

```sh
$ make 
```

To make the version of the daemon which runs against mock-nationstates, specify the testdaemon target.

```sh
$ make testdaemon
```

### Running

The daemon does not self-daemonize. Use a terminal multiplexer, daemonizing script, init scripts, or even systemd to run it in the background.

To run against mock-nationstates, you will want to run the testdaemon binary.

The only supported flag is `-v` which causes the daemon to turn libcurl into verbose mode, logging HTTP headers sent and received.A
