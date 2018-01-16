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

#pragma once

#define __GNU_SOURCE  /* For strdup() */
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <sys/uio.h>
#include <sys/queue.h>
#include <syslog.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <assert.h>

#include <librdkafka/rdkafka.h>

#define KAFKATEE_CONF_PATH     "/etc/kafkatee.conf"
#define KAFKATEE_PID_FILE_PATH "/run/kafkatee.pid"

#define _DBG(fmt...) do {			\
	if (conf.log_level > 6) {		\
		printf(fmt);			\
		printf("\n");			\
	}					\
	} while (0)

#define atomic_add(ptr,incr) __sync_add_and_fetch(ptr,incr)
#define atomic_sub(ptr,decr) __sync_sub_and_fetch(ptr,decr)
#define atomic_set(ptr,val)  __sync_lock_test_and_set(ptr,val)

#ifndef likely
#define likely(x)   __builtin_expect((x),1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect((x),0)
#endif

typedef struct msgpayload_s {
	struct iovec        mp_iov[2];
	int                 mp_iovcnt;
	int                 mp_refcnt;
	rd_kafka_message_t *mp_rkm;
} msgpayload_t;

typedef struct msg_s {
	TAILQ_ENTRY(msg_s)  m_link;
	msgpayload_t       *m_mp;
	struct iovec        m_iov[2];
	int                 m_iovcnt;
} msg_t;

typedef struct msgq_s {
	TAILQ_HEAD(, msg_s) mq_msgs;
	int                 mq_msgcnt;
	pthread_mutex_t     mq_lock;
	pthread_cond_t      mq_cond;
	time_t              mq_t_write;   /* Time of last fd write */
} msgq_t;

#define msgq_lock(mq)    pthread_mutex_lock(&(mq)->mq_lock)
#define msgq_unlock(mq)  pthread_mutex_unlock(&(mq)->mq_lock)

typedef enum {
	INPUT_PIPE,
	INPUT_KAFKA,
}  input_type_t;


typedef enum {
	ENC_ERROR = -1,
	ENC_STRING,
	ENC_JSON,
} encoding_t;

typedef struct input_s {
	LIST_ENTRY(input_s)  in_link;

	input_type_t         in_type;
	encoding_t           in_enc;
	char                *in_name;

	int                  in_flags;
#define INPUT_F_STOP_EOF     0x1  /* Stop on EOF */
#define INPUT_F_STOP_ERROR   0x2  /* Stop on exit/error */
#define INPUT_F_EXIT_ON_EXIT 0x4  /* Exit kafkatee if this input exits */
#define INPUT_F_DEFAULTS     0

	union {
		struct {
			char *cmd;
		} pipe;
		struct {
			char             *topic;
			int               partition;
			int64_t           offset;
			rd_kafka_topic_t *rkt;
			int               terminate;
		} kafka;
	} in_u;
#define in_kafka in_u.kafka
#define in_pipe  in_u.pipe

	uint64_t    in_c_rx;     /* Read messages */
	uint64_t    in_c_fmterr; /* Messages dropped due to formatting error */
	uint64_t    in_c_empty;  /* Empty messages dropped */

	pthread_t            in_thread;
} input_t;


typedef enum {
	OUTPUT_PIPE,
	OUTPUT_FILE,
} output_type_t;

typedef struct output_s {
	LIST_ENTRY(output_s)  o_link;
	int                   o_id;
	char                 *o_name;
	output_type_t         o_type;
	int                   o_sample_rate;
	uint64_t              o_c_rx;  /* number of received messages */
	uint64_t              o_c_tx;  /* number of transmitted messages */
	uint64_t              o_c_tx_qdrop; /* number of output queue drops */

	int                   o_fd;    /* Output file descriptor */
	time_t                o_t_open;   /* Time of file creation/cmd start */
	time_t                o_t_close;  /* Time of last close/termination */
	time_t                o_t_open_fail; /* Time of last open failure */
	int                   o_open_fails; /* Number of consecutive
					     * open failures */

	msgq_t                o_outq;

	union {
		struct {
			char *cmd;
			pid_t pid;
			int   status; /* Exit status */
		} pipe;

		struct {
			char *path;
		} file;
	} o_u;

#define o_pipe o_u.pipe
#define o_file o_u.file

} output_t;



/**
 * Formatting variable (%{VAR})
 */
struct fmtvar {
	const char *var;
	int         varlen;
	int         idx;
};

/**
 * Formatting value
 */
struct fmtval {
	char       *val;
	int         vallen;
	int         seen;
};


/**
 * Formatting from format
 */
struct fmt {
	int   idx;        /* fmt[] array index */
	int   varidx;     /* fmtvar index (non-zero if formatter is a var) */
	const char *def;  /* default string, typically "-" */
	int   deflen;     /* default string's length */
	int   flags;
#define FMT_F_ESCAPE    0x1 /* Escape the value string */
};


/**
 * Formatter configuration
 */
struct fmt_conf {
	encoding_t  encoding;

	/* Array of formatters in output order. */
	struct fmt *fmt;
	int         fmt_cnt;
	int         fmt_size;

	/* Array of fmtvars */
	struct fmtvar *fmtvar;
	int            fmtvar_cnt;
	int            fmtvar_size;

	char       *format;
};



/**
 * Formatter renderer
 */
struct render {
	const struct fmt_conf *fconf;
	struct fmtval         *fmtval; /* Variables' values */

	/* Value scratch buffer */
	char *scratch;
	int   scratch_of;
	int   scratch_size;
};



/**
 * kafkatee configuration container
 */
struct conf {
	char           *output_delimiter;     /* Output delimiter */
	int             output_delimiter_len;

	struct fmt_conf fconf;                /* Output format */

	int             output_queue_size;    /* Per-output queue size */

	int             input_queue_size_min;
	int             input_buf_size;

	int             run;
	int             output_id_next;

	int             rotate;
	int             log_level;
	int             log_rate;
	int             log_rate_period;
	int             daemonize;

	int             stats_interval;
	char           *stats_file;
	FILE           *stats_fp;

	char           *pid_file_path;

	char           *cmd_init;      /* Command run prior to starting IO */
	char           *cmd_term;      /* Command run at termination */

	int             flags;
#define CONF_F_EXIT_ON_EOF       0x1   /* Exit when all inputs have reached
					* their end-of-file and all
					* output queues are empty. */
#define CONF_F_EXIT_ON_IO_TERM   0x2   /* Exit on input/output termination. */

	int             exit_code;

	rd_kafka_t            *rk;
	rd_kafka_conf_t       *rk_conf;
	rd_kafka_topic_conf_t *rkt_conf;

};

extern struct conf conf;


#define kt_log(level,fmt...)  syslog(level, fmt)




#ifndef TAILQ_FOREACH_SAFE
/*
 * TAILQ_FOREACH_SAFE() provides a traversal where the current iterated element
 * may be freed or unlinked.
 * It does not allow freeing or modifying any other element in the list,
 * at least not the next element.
 */
#define TAILQ_FOREACH_SAFE(elm,head,field,tmpelm)			\
	for ((elm) = TAILQ_FIRST(head) ;				\
	     (elm) && ((tmpelm) = TAILQ_NEXT((elm), field), 1) ;	\
	     (elm) = (tmpelm))
#endif


#ifndef LIST_INSERT_SORTED
#define LIST_INSERT_SORTED(head, elm, field, cmpfunc) do {      \
        if(LIST_EMPTY(head)) {                                  \
           LIST_INSERT_HEAD(head, elm, field);                  \
        } else {                                                \
           typeof(elm) _tmp;                                    \
           LIST_FOREACH(_tmp,head,field) {                      \
              if(cmpfunc(elm,_tmp) <= 0) {                      \
                LIST_INSERT_BEFORE(_tmp,elm,field);             \
                break;                                          \
              }                                                 \
              if(!LIST_NEXT(_tmp,field)) {                      \
                 LIST_INSERT_AFTER(_tmp,elm,field);             \
                 break;                                         \
              }                                                 \
           }                                                    \
        }                                                       \
} while(0)
#endif
