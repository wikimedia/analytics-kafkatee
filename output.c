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

#define __need_IOV_MAX

#include "kafkatee.h"
#include "queue.h"
#include "exec.h"
#include "input.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/uio.h>
#include <sys/epoll.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <sys/socket.h>
#include <signal.h>


static LIST_HEAD(, output_s) outputs; /* Sorted in sample-rate order */
static int outputs_cnt;
static int outputs_epoll_fd = -1;
static pthread_t outputs_thread;


/**
 * If output is eligible for a message (according to queue pressure and 
 * its configured sample-rate) then create a new message shadowing the 
 * msgpayload and enqueue it on the output's output queue.
 */
static int output_msgpayload_enq (output_t *o, msgpayload_t *mp) {

	msgq_lock(&o->o_outq);
	if (o->o_outq.mq_msgcnt < conf.output_queue_size) {
		if (!(o->o_c_rx++ % o->o_sample_rate))
			msgq_enq(&o->o_outq, msg_new(mp));
	} else
		o->o_c_tx_qdrop++;
	msgq_unlock(&o->o_outq);
	return 0;
}


/**
 * Attempt to enqueue msgs based on the msgpayload on all outputs.
 */
int outputs_msgpayload_enq (msgpayload_t *mp) {
	output_t *o;
	int cnt = 0;

	LIST_FOREACH(o, &outputs, o_link) {
		if (output_msgpayload_enq(o, mp) != -1)
			cnt++;
	}

	return cnt;
}


/**
 * Output comparator for sorting: sort ascendingly on sample_rate
 */
static int output_cmp (void *_a, void *_b) {
	output_t *a = _a, *b = _b;

	return a->o_sample_rate - b->o_sample_rate;
}


/**
 * Add a new output.
 * This must only be done during initial setup and is typically done
 * by the config file reader.
 */
output_t *output_add (output_type_t type, int sample_rate, const char *cmd) {
	output_t *o;

	o = calloc(1, sizeof(*o));
	o->o_id          = conf.output_id_next++;
	o->o_type        = type;
	o->o_sample_rate = sample_rate;
	o->o_fd          = -1;

	assert(*cmd);

	switch (o->o_type)
	{
	case OUTPUT_PIPE:
		o->o_pipe.cmd = strdup(cmd);
		o->o_name     = o->o_pipe.cmd;
		break;

	case OUTPUT_FILE:
		o->o_file.path = strdup(cmd);
		o->o_name      = o->o_file.path;
		break;
	}

	msgq_init(&o->o_outq);

	LIST_INSERT_SORTED(&outputs, o, o_link, output_cmp);
	outputs_cnt++;



	return o;
}



/**
 * Open/start output.
 */
int output_open (output_t *o) {
	int fd = -1;
	struct epoll_event ev = {};
	int r;
	char errstr[512];

	_DBG("Opening output \"%s\"", o->o_name);

	assert(o->o_fd == -1);

	switch (o->o_type)
	{
	case OUTPUT_PIPE:
		o->o_pipe.status = 0;
		if ((fd = kt_popen(o->o_pipe.cmd, "w",
				   &o->o_pipe.pid, &o->o_pipe.status,
				   errstr, sizeof(errstr))) == -1) {
			kt_log(LOG_ERR,
			       "Failed to start output pipe #%i \"%s\": %s",
			       o->o_id, o->o_pipe.cmd, errstr);
			goto failed;
		}
		_DBG("Started output \"%s\" pid %i",
		     o->o_name, o->o_pipe.pid);
		break;

	case OUTPUT_FILE:
		if ((fd = open(o->o_file.path,
			       O_WRONLY|O_APPEND|O_CREAT, 0644)) == -1) {
			kt_log(LOG_ERR,
			       "Failed to open output file #%i \"%s\": %s",
			       o->o_id, o->o_file.path, strerror(errno));
			goto failed;
		}
		_DBG("Output file \"%s\" opened", o->o_name);

		break;

	default:
		goto failed;
	}

	/* Set non-blocking */
	if ((r = fcntl(fd, F_GETFL)) == -1 ||
	    fcntl(fd, F_SETFL, r|O_NONBLOCK) == -1) {
		kt_log(LOG_ERR,
		       "Failed to set non-blocking output for \"%s\": %s",
		       o->o_name, strerror(errno));
		goto failed;
	}

	o->o_fd          = fd;
	o->o_t_open      = time(NULL);
	o->o_open_fails  = 0;
	o->o_t_open_fail = 0;

	/* Start polling for writability (edge-triggered) */
	ev.events        = EPOLLOUT | EPOLLET;
	ev.data.ptr      = o;
	epoll_ctl(outputs_epoll_fd, EPOLL_CTL_ADD, o->o_fd, &ev);


	return 0;

failed:
	if (fd != -1)
		close(o->o_fd);
	o->o_open_fails++;
	o->o_t_open_fail = time(NULL);
	return -1;
}



/**
 * Close/stop an output.
 */
void output_close (output_t *o, int level, const char *reason) {

	if (o->o_fd == -1)
		return;

	kt_log(level, "Closing output \"%s\": %s (%i messages in queue)",
	       o->o_name, reason, o->o_outq.mq_msgcnt);

	epoll_ctl(outputs_epoll_fd, EPOLL_CTL_DEL, o->o_fd, NULL);

	switch (o->o_type)
	{
	case OUTPUT_PIPE:
	case OUTPUT_FILE:
		close(o->o_fd);
		break;
	}

	o->o_fd = -1;
	o->o_t_close = time(NULL);

	if (conf.run && conf.flags & CONF_F_EXIT_ON_IO_TERM) {
		kt_log(LOG_NOTICE, "Exiting on output \"%s\" termination",
		       o->o_name);
		conf.exit_code = 2;
		conf.run = 0;
	}
}



/**
 * Attempts to write as many messages as possible from the output's out queue
 * to the output fd.
 * Returns 1 if more messages can be written, 0 if not, or -1 on error.
 */
static int output_write0 (output_t *o) {
	struct iovec iov[IOV_MAX];
	int i = 0;
	ssize_t r;
	msg_t *m, *mtmp;

	/* Collect messages into iovec */
	msgq_lock(&o->o_outq);
	TAILQ_FOREACH(m, &o->o_outq.mq_msgs, m_link) {
		memcpy(&iov[i], &m->m_iov[0], sizeof(iov[i]) * m->m_iovcnt);
		i += m->m_iovcnt;
		if (unlikely(i+2 == IOV_MAX))
			break;
	}
	msgq_unlock(&o->o_outq);

	if (i == 0)
		return 0;

	/* Write iovec */
	r = writev(o->o_fd, iov, i);
	if (r == -1) {
		if (likely(errno == EAGAIN))
			return 0;
		_DBG("Write failure on fd %i at %"PRIu64" msgs sent: %s",
		     o->o_fd, o->o_c_tx, strerror(errno));
		return -1;
	}

	/* Unlink written messages and adjust partially written messages
	 * for the next write attempt. */
	msgq_lock(&o->o_outq);
	TAILQ_FOREACH_SAFE(m, &o->o_outq.mq_msgs, m_link, mtmp) {
		if (r == 0)
			break;

		for (i = 0 ; i < m->m_iovcnt ; i++) {
			if (m->m_iov[i].iov_len <= r) {
				/* Wrote full iov element */
				r -= m->m_iov[i].iov_len;
				if (i == m->m_iovcnt - 1) {
					/* Wrote full message */
					o->o_c_tx++;
					msgq_deq(&o->o_outq, m);
					msg_destroy(m);
					break;
				}
			} else {
				/* Wrote partial iov element */
				m->m_iov[i].iov_base =
					(char *)m->m_iov[i].iov_base + r;
				m->m_iov[i].iov_len -= r;
				goto done;
			}
		}
	}
done:
	msgq_unlock(&o->o_outq);

	/* All messages written? If so we may be called again */
	return !m ? 1 : 0;
}


/**
 * Write as many messages to file descriptor from outq as possible.
 * Propogate errors to caller.
 */
static int output_write (output_t *o) {
	int r;

	if (unlikely(o->o_fd == -1))
		return -1;

	while ((r = output_write0(o)) > 0)
		;

	return r;
}



/**
 * Check if any outputs needs starting/opening.
 */
static void outputs_check (void) {
	output_t *o;
	time_t now = time(NULL);
	int totqcnt = 0;

	LIST_FOREACH(o, &outputs, o_link) {
		/* FIXME: remove */
		if (0)
		_DBG("\"%s\" queue pressure: %i  (rx %"PRIu64", tx %"PRIu64
		     ", tx qdrop %"PRIu64", fd %i)",
		     o->o_name, o->o_outq.mq_msgcnt,
		     o->o_c_rx, o->o_c_tx, o->o_c_tx_qdrop, o->o_fd);

		msgq_lock(&o->o_outq);
		totqcnt = o->o_outq.mq_msgcnt;
		msgq_unlock(&o->o_outq);

		/* Already open */
		if (o->o_fd != -1) {
			if (o->o_pipe.status != -1)
				output_close(o, LOG_WARNING,
					     exec_exitstatus(o->o_pipe.status));
			continue;
		}

		/* Last open failed: back off */
		if (o->o_t_open_fail + (2 * (o->o_open_fails % 10)) > now)
			continue;

		/* Open output */
		output_open(o);
	}

	/* exit_on_eof check, terminate application if:
	 *  - all inputs are stopped
	 *  - all output queues are drained. */
	if (unlikely(inputs_cnt == 0 &&
		     (conf.flags & CONF_F_EXIT_ON_EOF) &&
		     totqcnt == 0)) {
		kt_log(LOG_NOTICE, "Exiting on EOF");
		conf.run = 0;
	}
}



/**
 * Output rotation:
 * Close all outputs and open them again.
 */
static void outputs_rotate (void) {
	output_t *o;

	kt_log(LOG_INFO, "Rotating %i outputs", outputs_cnt);

	LIST_FOREACH(o, &outputs, o_link) {
		/* Close output */
		if (o->o_fd != -1)
			output_close(o, LOG_DEBUG, "rotate");

		/* Reset open_fail to force a quick retry */
		o->o_t_open_fail = 0;
	}

	/* Checker will re-open outputs */
	outputs_check();
}


/**
 * The outputs thread main loop.
 */
static void *outputs_main (void *ignore) {
	struct epoll_event *ev;
	time_t t_last_check = 0;
	output_t *o;

	ev = malloc(sizeof(*ev) * outputs_cnt);

	while (conf.run) {
		int r;
		time_t now;
		int i;

		if (unlikely(conf.rotate)) {
			/* Outputs rotation */
			conf.rotate = 0; /* FIXME: versioning */
			outputs_rotate();
		} else {
			/* Periodic outputs checker */
			now = time(NULL);
			if (unlikely(t_last_check + 1 < now)) {
				t_last_check = now;
				outputs_check();
			}
		}


		/* Poll output fds for writeability */
		r = epoll_wait(outputs_epoll_fd, ev, outputs_cnt, 100);

		/* Handle events */
		for (i = 0 ; i < r ; i++) {
			o = ev[i].data.ptr;

			if (unlikely(ev[i].events & EPOLLHUP))
				output_close(o, LOG_WARNING, "hung up");
		}

		/* Try to write as many messages as possible to all outputs
		 * regardless of poll status or not, the poll status is
		 * probably outdated by the time we come here anyway. */
		LIST_FOREACH(o, &outputs, o_link)
			if (unlikely(output_write(o) == -1))
				output_close(o, LOG_WARNING, "write failure");
	}

	free(ev);

	/* Close all outputs */
	LIST_FOREACH(o, &outputs, o_link)
		output_close(o, LOG_INFO, "terminating");

	return NULL;
}


/**
 * Terminate outputs sub-system
 */
void outputs_term (void) {
	void *ignore;
	pthread_join(outputs_thread, &ignore);
}

/**
 * Initialize outputs sub-system
 */
void outputs_start (void) {

	/* Create epoll handle */
	outputs_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (outputs_epoll_fd == -1) {
		kt_log(LOG_CRIT, "Failed to create epoll fd: %s",
		       strerror(errno));
		exit(1);
	}

	/* Start outputs thread */
	if (pthread_create(&outputs_thread, NULL, outputs_main, NULL) == -1) {
		kt_log(LOG_CRIT, "Failed to create outputs thread: %s",
		       strerror(errno));
		exit(1);
	}
}
