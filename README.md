kafkatee - Apache Kafka consumer with multiple inputs and outputs
=================================================================

Copyright (c) 2014 [Wikimedia Foundation](http://www.wikimedia.org)

Copyright (c) 2014 [Magnus Edenhill](http://www.edenhill.se/)


# Description

kafkatee consumes messages from one or more Kafka topics and
writes the messages to one or more outputs - either command pipes or files.

It provides simple transformation from JSON to arbitrary string output
controlled through configuration.

Each output has a configurable sample rate.


Features:
 * Supported input types:  Kafka consumer or piped command.
 * Supported output types: Piped command or file.
 * Configuration file syntax is backwards compatible with Wikimedia's udp2log
 * Configurable output queue size
 * Memory frugal: message payload is shared by all output queues
 * Configured with configuration file
 * Operates as a daemon (daemonization, pidfile)
 * Closes/stops and reopens/restarts all inputs and outputs on SIGHUP.


# Documentation

See `kafkatee.conf.example` for an annotated configuration file explaining
available configuration properties.


# Build

## Dependencies

 * librdkafka-dev >= 0.8.3
 * libyajl-dev (yajl1 or yajl2)

## Compile

    make

## Install

    make install
    # or:
    DESTDIR=/alternate/buildroot make install


## Run tests

    make test



