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
#include "queue.h"
#include "output.h"
#include "format.h"
#include "exec.h"
#include "ezd.h"

static LIST_HEAD(, input_s) inputs;

/* Number of non-stopped inputs */
int inputs_cnt = 0;


/**
 * Add an input.
 * This function must only be called during initial setup.
 */
input_t *input_add (input_type_t type, encoding_t enc, int flags,
		    const char *arg1, int arg2, int64_t arg3,
		    char *errstr, size_t errstr_size) {
	input_t *in;

	in = calloc(1, sizeof(*in));

	in->in_type  = type;
	in->in_enc   = enc;
	in->in_flags = flags;

	switch (in->in_type)
	{
	case INPUT_PIPE:
		in->in_pipe.cmd = strdup(arg1);
		in->in_name     = in->in_pipe.cmd;
		break;

	case INPUT_KAFKA:
		in->in_kafka.topic     = strdup(arg1);
		in->in_kafka.partition = arg2;
		in->in_kafka.offset    = arg3;
		in->in_name            = in->in_kafka.topic;
		break;
	}

	LIST_INSERT_HEAD(&inputs, in, in_link);
	atomic_add(&inputs_cnt, 1);

	return in;
}


/**
 * Indicate that input 'in' has been stopped.
 */
static void input_stopped (input_t *in) {
	atomic_sub(&inputs_cnt, 1);
}


/**
 * Pipe input: main loop
 */
static void input_pipe_main (input_t *in) {
	FILE *fp = NULL;
	pid_t pid;
	int status; /* pipe exit status */
	char errstr[512];
	char *buf;
	static int our_rotate_version = 0;

	buf = malloc(conf.input_buf_size);

	while (conf.run) {
		char *ret;

		if (unlikely(!fp)) {
			int fd;
			_DBG("Starting input pipe \"%s\"", in->in_pipe.cmd);

			status = -1;

			if ((fd = kt_popen(in->in_pipe.cmd, "r", &pid, &status,
					   errstr, sizeof(errstr))) == -1) {
				kt_log(LOG_ERR,
				       "Failed to start input pipe \"%s\": %s",
				       in->in_pipe.cmd, errstr);

				if (conf.flags & CONF_F_EXIT_ON_IO_TERM) {
					kt_log(LOG_NOTICE,
					       "Exiting on input termination");
					conf.exit_code = 2;
					conf.run = 0;
					break;
				}

				sleep(1);
				continue;
			}


			fp = fdopen(fd, "r");
		}

		while (conf.run &&
		       status == -1 &&
		       ((ret = fgets(buf, conf.input_buf_size, fp)) ||
			!ferror(fp))) {
			msgpayload_t *mp;

			_DBG("Input \"%s\" counters: rx %"PRIu64", "
			     "fmterr %"PRIu64", empty %"PRIu64,
			     in->in_name,
			     in->in_c_rx, in->in_c_fmterr, in->in_c_empty);

			if (likely(ret != NULL)) {
				int len = strlen(buf);

				/* Remove input delimiter (newline) */
				if (buf[len-1] == '\n')
					len--;

				mp = msgpayload_new(in, buf, len, NULL);
				if (likely(mp != NULL)) {
					outputs_msgpayload_enq(mp);

					/* Drop our reference */
					msgpayload_destroy(mp);
				}
			} else {
				/* At EOF: clear EOF indicator, sleep, and
				 * then try again. Unless we're not allowed
				 * to restart on eof. */
				_DBG("Input \"%s\" at EOF", in->in_name);

				if (in->in_flags & INPUT_F_STOP_EOF)
					break;

				clearerr(fp);
				usleep(100000);
			}

			if (unlikely(conf.rotate != our_rotate_version)) {
				our_rotate_version = conf.rotate;
				break;
			}
		}

		_DBG("Input \"%s\" Status=%i, EOF=%i, Error=%i", in->in_name,
		     status, feof(fp), ferror(fp));

		if (status != -1) {
			kt_log(status == 0 ? LOG_NOTICE : LOG_ERR,
			       "Input \"%s\" (pid %i) %s",
			       in->in_name, (int)pid, exec_exitstatus(status));
			if (in->in_flags & INPUT_F_EXIT_ON_EXIT) {
				if (WIFEXITED(status))
					conf.exit_code = WEXITSTATUS(status);
				else
					conf.exit_code = 126;
				conf.run = 0;
				break;
			}

		} else if (ferror(fp)) {
			kt_log(LOG_ERR,
			       "Input \"%s\" error: %s",
			       in->in_name, strerror(errno));

			if (in->in_flags & INPUT_F_STOP_ERROR)
				break;
		}

		fclose(fp);
		fp = NULL;

		/* Hold off file-reopen to avoid busy-looping. */
		sleep(1);
	}

	if (fp)
		pclose(fp);

	free(buf);
}


/**
 * Kafka consumer message callback.
 */
static void input_kafka_msg_cb (rd_kafka_message_t *rkmessage, void *opaque) {
	input_t *in = opaque;
	msgpayload_t *mp;

	if (rkmessage->err) {
		_DBG("%s: %s[%"PRId32"]: %s",
		     rkmessage->err != RD_KAFKA_RESP_ERR__PARTITION_EOF ?
		     "err" : "EOF",
		     rd_kafka_topic_name(rkmessage->rkt),
		     rkmessage->partition,
		     rd_kafka_message_errstr(rkmessage));

		if (rkmessage->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
			if (in->in_flags & INPUT_F_STOP_EOF)
				in->in_kafka.terminate = 1;

		} else {
			if (in->in_flags & INPUT_F_STOP_ERROR)
				in->in_kafka.terminate = 1;
		}

		return;
	}

	/* Create payload handle.
	 * This will also perform any configured transformations. */
	mp = msgpayload_new(in,
			    rkmessage->payload, rkmessage->len, NULL);
	if (unlikely(!mp))
		return;

	/* Enqueue payload on output queue(s) */
	outputs_msgpayload_enq(mp);

	/* Drop our reference */
	msgpayload_destroy(mp);
}


/**
 * Kafka input: main loop
 */
static void input_kafka_main (input_t *in) {
	rd_kafka_topic_t *rkt;

	rkt = rd_kafka_topic_new(conf.rk, in->in_kafka.topic,
				 rd_kafka_topic_conf_dup(conf.rkt_conf));

	_DBG("Starting consumer for kafka topic %s partition %i",
	     rd_kafka_topic_name(rkt), in->in_kafka.partition);

	if (rd_kafka_consume_start(rkt, in->in_kafka.partition,
				   in->in_kafka.offset) == -1) {
		kt_log(LOG_ERR,
		       "Failed to start Kafka consumer for topic %s "
		       "partition %i (input \"%s\"): %s",
		       rd_kafka_topic_name(rkt), in->in_kafka.partition,
		       in->in_name, strerror(errno));
		rd_kafka_topic_destroy(rkt);
		return;
	}

	while (conf.run && !in->in_kafka.terminate) {
		rd_kafka_consume_callback(rkt, in->in_kafka.partition, 1000,
					  input_kafka_msg_cb, in);
	}

	rd_kafka_consume_stop(rkt, in->in_kafka.partition);

	rd_kafka_topic_destroy(rkt);
}


/**
 * Per-input thread's main loop trampoline.
 */
static void *input_main (void *arg) {
	input_t *in = arg;

	ezd_thread_sigmask(SIG_BLOCK, 0/*ALL*/, -1/*end*/);

	switch (in->in_type)
	{
	case INPUT_PIPE:
		input_pipe_main(in);
		break;

	case INPUT_KAFKA:
		input_kafka_main(in);
		break;
	}

	/* Destroy thread-specific renderer */
	render_destroy();

	input_stopped(in);

	return NULL;
}


/**
 * Wait for input 'in' to stop
 */
void input_wait_stopped (input_t *in) {
	void *ignore;
	pthread_join(in->in_thread, &ignore);
}

/**
 * Start input.
 */
int input_start (input_t *in) {

	if (pthread_create(&in->in_thread, NULL, input_main, in) == 1) {
		kt_log(LOG_ERR,
		       "Failed to start input thread: %s", strerror(errno));
		input_stopped(in);
		return -1;
	}

	return 0;
}


/**
 * Stop all inputs.
 */
void inputs_term (void) {
	input_t *in;

	LIST_FOREACH(in, &inputs, in_link)
		input_wait_stopped(in);
}


/**
 * Start all inputs.
 * Called after initial setup.
 */
void inputs_start (void) {
	input_t *in;

	if (LIST_EMPTY(&inputs)) {
		kt_log(LOG_ERR,
		       "No inputs configured: terminating");
		exit(1);
	}

	LIST_FOREACH(in, &inputs, in_link) {
		if (input_start(in) == -1)
			exit(1);
	}

}
