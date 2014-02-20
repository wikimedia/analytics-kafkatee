/*
 * Copyright (c) 2014 Wikimedia Foundation
 * Copyright (c) 2014 Magnus Edenhill <magnus@edenhill.se>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "kafkatee.h"

#include <signal.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>

#include "config.h"
#include "format.h"
#include "input.h"
#include "output.h"
#include "exec.h"
#include "ezd.h"

static void sighup (int sig) {
	static int rotate_version = 0;
	conf.rotate = ++rotate_version;
}

static void term (int sig) {
	kt_log(LOG_NOTICE,
	       "Received signal %i: terminating", sig);

	if (!conf.run) {
		/* Force exit on second term signal */
		kt_log(LOG_WARNING, "Forced termination");
		exit(0);
	}

	atomic_set(&conf.run, 0);
}

/**
 * Kafka error callback
 */
static void kafka_error_cb (rd_kafka_t *rk, int err,
			    const char *reason, void *opaque) {
	kt_log(LOG_ERR,
	       "Kafka error (%i): %s", err, reason);
}



/**
 * Kafka statistics callback.
 */
static int kafka_stats_cb (rd_kafka_t *rk, char *json, size_t json_len,
			void *opaque) {
	if (!conf.stats_fp)
		return 0;

	fprintf(conf.stats_fp, "{ \"kafka\": %s }\n", json);
	return 0;
}

/**
 * Output kafkatee specific stats to statsfile.
 */
static void stats_print (void) {
	/* FIXME: Currently none */
}

static void stats_close (void) {
	stats_print();
	fclose(conf.stats_fp);
	conf.stats_fp = NULL;
}

static int stats_open (void) {
	/* Already open? close and then reopen */
	if (conf.stats_fp)
		stats_close();

	if (!(conf.stats_fp = fopen(conf.stats_file, "a"))) {
		kt_log(LOG_ERR,
		"Failed to open statistics log file %s: %s",
		conf.stats_file, strerror(errno));
		return -1;
	}

	return 0;
}


static void usage (const char *argv0) {
	fprintf(stderr,
		"kafkatee version %s\n"
		"Kafka consumer with multiple inputs and outputs\n"
		"\n"
		"Usage: %s [options]\n"
		"\n"
		"Options:\n"
		"  -c <path>          Configuration file path (%s)\n"
		"  -p <path>          Pid file path (%s)\n"
		"  -d                 Enable debugging\n"
		"  -D                 Do not daemonize\n"
		"  -e                 Exit on EOF:\n"
		"                     Exit when all inputs have reached their\n"
		"                     EOF and all output queues are empty.\n"
		"  -x                 Exit on input or output failure.\n"
		"                     (exit code 2)\n"
		"\n",
		KAFKATEE_VERSION,
		argv0,
		KAFKATEE_CONF_PATH,
		KAFKATEE_PID_FILE_PATH);
	exit(1);
}


int main (int argc, char **argv) {
	const char *conf_file_path = KAFKATEE_CONF_PATH;
	char errstr[512];
	char c;
	int r;
	static int our_rotate_version = 0;

	/*
	 * Default configuration
	 */
	conf.pid_file_path    = strdup(KAFKATEE_PID_FILE_PATH);
	conf.run              = 1;
	conf.exit_code        = 0;
	conf.log_level        = 6;
	conf.daemonize        = 1;
	conf.stats_interval   = 60;
	conf.stats_file       = strdup("/tmp/kafkatee.stats.json");
	conf.input_buf_size   = 1024 * 10;
	conf.output_delimiter = strdup("\n");
	conf.output_delimiter_len = strlen(conf.output_delimiter);
	conf.output_queue_size = 100000;

	/* Kafka main configuration */
	conf.rk_conf          = rd_kafka_conf_new();
	rd_kafka_conf_set(conf.rk_conf, "client.id", "kafkatee", NULL, 0);
	rd_kafka_conf_set_error_cb(conf.rk_conf, kafka_error_cb);

	/* Kafka topic configuration template */
	conf.rkt_conf = rd_kafka_topic_conf_new();

	/* Parse command line arguments */
	while ((c = getopt(argc, argv, "hc:p:dDex")) != -1) {
		switch (c) {
		case 'h':
			usage(argv[0]);
			break;
		case 'c':
			conf_file_path = optarg;
			break;
		case 'p':
			conf.pid_file_path = strdup(optarg);
			break;
		case 'd':
			conf.log_level = 7;
			break;
		case 'D':
			conf.daemonize = 0;
			break;
		case 'e':
			conf.flags |= CONF_F_EXIT_ON_EOF;
			break;
		case 'x':
			conf.flags |= CONF_F_EXIT_ON_IO_TERM;
			break;
		default:
			usage(argv[0]);
			break;
		}
	}

	openlog("kafkatee", LOG_PID|LOG_PERROR, LOG_DAEMON);

	/* Read config file */
	if (ezd_conf_file_read(conf_file_path, conf_set,
			       errstr, sizeof(errstr), NULL) == -1) {
		kt_log(LOG_ERR, "%s", errstr);
		exit(1);
	}

	/* Daemonize if desired */
	if (conf.daemonize) {
		if (ezd_daemon(10, errstr, sizeof(errstr)) == -1) {
			kt_log(LOG_ERR, "%s", errstr);
			exit(1);
		}
	}

	if (ezd_pidfile_open(conf.pid_file_path,
			     errstr, sizeof(errstr)) == -1) {
		kt_log(LOG_ERR, "%s", errstr);
		exit(1);
	}


	/* Parse the format string */
	if (conf.fconf.format) {
		if (conf.fconf.encoding != ENC_STRING) {
			kt_log(LOG_ERR, "Output formatting only supported for "
				"output.encoding = string");
			ezd_pidfile_close();
			exit(1);
		}

		if (format_parse(&conf.fconf, conf.fconf.format,
				 errstr, sizeof(errstr)) == -1) {
			kt_log(LOG_ERR,
				"Failed to parse format string: %s\n%s",
				conf.fconf.format, errstr);
			ezd_pidfile_close();
			exit(1);
		}
	}

	/* Set up statistics gathering in librdkafka, if enabled. */
	if (conf.stats_interval) {
		char tmp[30];

		if (stats_open() == -1) {
			kt_log(LOG_ERR,
				"Failed to open statistics log file %s: %s",
				conf.stats_file, strerror(errno));
			ezd_pidfile_close();
			exit(1);
		}

		snprintf(tmp, sizeof(tmp), "%i", conf.stats_interval*1000);
		rd_kafka_conf_set_stats_cb(conf.rk_conf, kafka_stats_cb);
		rd_kafka_conf_set(conf.rk_conf, "statistics.interval.ms", tmp,
				  NULL, 0);
	}


	/* Create Kafka handle */
	if (!(conf.rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf.rk_conf,
				     errstr, sizeof(errstr)))) {
		kt_log(LOG_ERR,
		       "Failed to create kafka handle: %s", errstr);
		ezd_pidfile_close();
		exit(1);
	}

	rd_kafka_set_log_level(conf.rk, conf.log_level);


	/* Initialize subsystems */
	exec_init();

	/* Finalize daemonization */
	if (conf.daemonize)
		ezd_daemon_started();

	/* Run init command, if any. */
	if (conf.cmd_init) {
		if ((r = system(conf.cmd_init) != 0))
			kt_log(LOG_ERR,
			       "\"command.init\" execution of \"%s\" failed "
			       "with exit code %i", conf.cmd_init, r);
	}

	/* Block all signals in the main thread so new threads get the same
	* procmask. */
	ezd_thread_sigmask(SIG_BLOCK, 0/*ALL*/, -1/*end*/);

	/* Start IO */
	outputs_start();
	inputs_start();


	/* Set main thread sigmask */
	ezd_thread_sigmask(SIG_UNBLOCK, SIGHUP, SIGINT, SIGTERM, -1);
	signal(SIGHUP, sighup);
	signal(SIGINT, term);
	signal(SIGTERM, term);

	kt_log(LOG_INFO, "kafkatee starting");
	/* Main loop */
	while (conf.run) {
		rd_kafka_poll(conf.rk, 1000);
		if (unlikely(conf.rotate != our_rotate_version)) {
			our_rotate_version = conf.rotate;
			if (conf.stats_interval)
				stats_open();
		}
	}

	inputs_term();
	outputs_term();
	exec_term();

	rd_kafka_destroy(conf.rk);
	rd_kafka_wait_destroyed(5000);

	/* if stats_fp is set (i.e. open), close it. */
	if (conf.stats_fp)
		stats_close();
	free(conf.stats_file);

	/* Run termination command, if any. */
	if (conf.cmd_term) {
		if ((r = system(conf.cmd_term) != 0))
			kt_log(LOG_ERR,
			       "\"command.term\" execution of \"%s\" failed "
			       "with exit code %i", conf.cmd_term, r);
	}

	ezd_pidfile_close();

	kt_log(LOG_INFO, "kafkatee exiting");
	exit(conf.exit_code);
}
