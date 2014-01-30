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
#include "format.h"

#include <assert.h>


/**
 * Number of original messages (i.e., msgpayloads) currently in existence.
 */
int msgs_cnt = 0;


/**
 * Unref msgpayload, free it if refcnt reaches 0.
 */
void msgpayload_destroy (msgpayload_t *mp) {
	if (atomic_sub(&mp->mp_refcnt, 1) > 0)
		return;

	if (mp->mp_rkm)
		rd_kafka_message_destroy(mp->mp_rkm);
	else
		free(mp->mp_iov[0].iov_base);

	free(mp);

	atomic_sub(&msgs_cnt, 1);
}

/**
 * Unref message's msgpayload and free 'm'.
 */
void msg_destroy (msg_t *m) {
	msgpayload_destroy(m->m_mp);
	free(m);
}


/**
 * Create new sharable msgpayload.
 * Performs payload transformation according to configuration.
 */
msgpayload_t *msgpayload_new (input_t *in, void *payload, size_t len,
			      rd_kafka_message_t *rkm) {
	msgpayload_t *mp;
	void *final_payload;
	size_t final_len;
	int extra_delim = 0;

	atomic_add(&in->in_c_rx, 1);

	/* Perform payload transformation if necessary. */
	if (in->in_enc != conf.fconf.encoding) {
		if (unlikely(payload_transform(in->in_enc,
					       &final_payload, &final_len,
					       payload, len) == -1)) {
			atomic_add(&in->in_c_fmterr, 1);
			return NULL;
		}
	} else {
		/* No transformation */
		if (rkm) {
			/* Kafka message: dont copy, point to message payload*/
			final_payload = payload;
			final_len     = len;
			/* As we cant modify the payload for non-copied messages
			 * we add the delimiter as an extra iovec instead. */
			extra_delim   = 1;
		} else {
			/* Copy data */
			final_payload = malloc(len+conf.output_delimiter_len);
			memcpy(final_payload, payload, len);
			memcpy((char *)final_payload+len, conf.output_delimiter,
			       conf.output_delimiter_len);
			final_len     = len+conf.output_delimiter_len;
		}
	}

	if (unlikely(final_len == 0)) {
		atomic_add(&in->in_c_empty, 1);
		return NULL;
	}

	mp = malloc(sizeof(*mp));
	mp->mp_iov[0].iov_base = final_payload;
	mp->mp_iov[0].iov_len  = final_len;
	if (extra_delim) {
		mp->mp_iov[1].iov_base = conf.output_delimiter;
		mp->mp_iov[1].iov_len  = conf.output_delimiter_len;
		mp->mp_iovcnt = 2;
	} else
		mp->mp_iovcnt = 1;
	mp->mp_rkm          = rkm;
	mp->mp_refcnt       = 1;

	atomic_add(&msgs_cnt, 1);

	return mp;
}


/**
 * Create a message pointing to the provided payload
 */
msg_t *msg_new (msgpayload_t *mp) {
	msg_t *m;

	m = malloc(sizeof(*m));
	m->m_mp = mp;
	memcpy(m->m_iov, mp->mp_iov, sizeof(*mp->mp_iov) * mp->mp_iovcnt);
	m->m_iovcnt = mp->mp_iovcnt;
	atomic_add(&mp->mp_refcnt, 1);

	return m;
}


/**
 * Enqueue message on queue.
 * NOTE: msgq must be locked
 */
void msgq_enq (msgq_t *mq, msg_t *m) {
	TAILQ_INSERT_TAIL(&mq->mq_msgs, m, m_link);
	mq->mq_msgcnt++;
}

/**
 * Dequeue message from queue.
 * NOTE: msgq must be locked
 */
void msgq_deq (msgq_t *mq, msg_t *m) {
	assert(mq->mq_msgcnt > 0);
	TAILQ_REMOVE(&mq->mq_msgs, m, m_link);
	mq->mq_msgcnt--;
}


/**
 * Initialize message queue.
 */
void msgq_init (msgq_t *mq) {
	TAILQ_INIT(&mq->mq_msgs);
	mq->mq_msgcnt = 0;
}

