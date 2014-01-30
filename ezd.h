/**
 * ezd - eazy daemon - makes your life as a daemon conceiver easy.
 *
 *  Provides the most basic tools for daemonizing a program, such as:
 *   - configuration file parsing and reading (name=value.. pairs)
 *   - pidfile handling with locking
 *   - daemon()ization with wait-for-child-to-fully-start support to
 *     allow full initialization in the child process.
 *
 *  Simply add ezd.c and ezd.h to your project and use as you like.
 */

/*
 * Copyright (c) 2013 Magnus Edenhill <magnus@edenhill.se>
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


/**
 * Check and create/open pidfile 'path'.
 * If the pidfile lock cant be acquired, or the pidfile cant be written,
 * this function fails and returns -1 (reason written in errstr).
 * On success 0 is returned and the pidfile remains opened and locked
 * until the application exits or calls 'ezd_pidfile_close()'.
 */
int  ezd_pidfile_open (const char *path, char *errstr, size_t errstr_size);


/**
 * Unlock and close the pidfile previously opened with 'ezd_pidfile_open()'.
 * This function should be called just prior to program termination.
 */
void ezd_pidfile_close (void);


/**
 * Parses the CSV list in 'val' and allocates an array 'arrp'
 * with one malloced string in each slot.
 * Returns the number of slots in the array.
 */
int ezd_csv2array (char ***arrp, const char *val);


/**
 * Read configuration file 'path'.
 * The configuration file format is assumed to be 'key=value' based
 * but if no '=' character is found the entire line is provided in 'val' to
 * 'conf_set_cb'.
 *
 * 'conf_set_cb' is an application provided callback to apply the
 * configuration lines.
 */
int ezd_conf_file_read (const char *path,
			int (*conf_set_cb) (const char *name,
					    const char *val,
					    char *errstr,
					    size_t errstr_size,
					    void *opaque),
			char *errstr, size_t errstr_size,
			void *opaque);


/**
 * Parses the value as true or false.
 */
int ezd_str_tof (const char *val);


/**
 * Regexp matcher with iovec matching output.
 * Compiles a regexp of 'regex', matches 'str' to it and puts the
 * sub-matches into the array of iovecs in 'match' of size 'matchcnt'.
 * Returns the number of sub-matches stored in 'match' or 0 on no-match
 * and -1 on regexp compilation failure.
 */
int ezd_regmatch (const char *regex, const char *str,
		  struct iovec *match, int matchcnt);


/**
 * Provides a null-terminated string in stack memory for the string in 'iov'.
 * For use with ezd_regmatch()
 */
#define ezd_strndupa_iov(iov) ({					\
			struct iovec *_v = (iov);			\
			char *_s = alloca(_v->iov_len+1) ;		\
			memcpy(_s, _v->iov_base, _v->iov_len);		\
			_s[_v->iov_len] = '\0';			\
			_s; })


/**
 * Start daemonization.
 * Parent process will linger for timeout_sec seconds waiting for
 * child process to call ezd_daemon_started().
 * Finalize from child process with ezd_daemon_started().
 * Parent process will exit(0) if child is properly started.
 */
int ezd_daemon (int timeout_sec, char *errstr, size_t errstr_size);

/**
 * Tell parent process that child is now fully started.
 * This will finalize the ezd_daemon() call in the parent thread.
 */
void ezd_daemon_started (void);



