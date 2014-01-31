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

#define _GNU_SOURCE  /* for pipe2() */

#include "kafkatee.h"
#include "exec.h"
#include "ezd.h"

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/wait.h>



/**
 * Keeps track of spawned child processes.
 */
struct child_proc {
	LIST_ENTRY(child_proc) link;
	pid_t   pid;
	char   *cmd;
	time_t  t_start;
	int    *statusp;
};

static LIST_HEAD(, child_proc) child_procs;
static pthread_mutex_t         child_proc_lock = PTHREAD_MUTEX_INITIALIZER;
static int                     child_proc_cnt = 0; /* atomic */

static int       do_child_proc_reap = 0; /* atomic */
static pthread_t exec_thread;


/**
 * Adds a child process to the list of known child processes.
 * NOTE: child_proc_lock must be held
 */
static void child_proc_add (const char *cmdstring, pid_t pid, int *statusp) {
	struct child_proc *cp;

	cp = calloc(1, sizeof(*cp));
	cp->cmd      = strdup(cmdstring);
	cp->pid      = pid;
	cp->t_start  = time(NULL);
	cp->statusp  = statusp;
	*cp->statusp = -1;

	LIST_INSERT_HEAD(&child_procs, cp, link);
	atomic_add(&child_proc_cnt, 1);
}


/**
 * Removes a child proc from the list.
 * NOTE: child_proc_lock must be held
 */
static void child_proc_del (struct child_proc *cp) {
	LIST_REMOVE(cp, link);
	free(cp->cmd);
	free(cp);
	atomic_sub(&child_proc_cnt, 1);
}

/**
 * Finds a child proc based on its pid.
 * NOTE: child_proc_lock must be held
 */
static struct child_proc *child_proc_find (pid_t pid) {
	struct child_proc *cp;

	LIST_FOREACH(cp, &child_procs, link)
		if (cp->pid == pid)
			return cp;

	return NULL;
}


/**
 * Similar to popen() but uses file descriptors instead of streams and
 * returns the child process' pid in '*pidp'.
 * 'statusp' is a pointer to where the child's exit status will be stored
 * if the child terminates. The caller should set 'statusp' to -1 prior
 * to calling kt_open, -1 indicates that pipe is still running.
 *
 * Returns the file descriptor on success or -1 on error.
 */
int kt_popen (const char *cmdstring, const char *rw,
	      pid_t *pidp, int *statusp,
	      char *errstr, size_t errstr_size) {

	int pfd[2];
	pid_t pid;

	/* Create pipe fds */
	if (pipe2(pfd, O_CLOEXEC) == -1) {
		snprintf(errstr, errstr_size,
			 "pipe failed: %s", strerror(errno));
		return -1;
	}

	/* Lock child proc list prior to fork to avoid
	 * race conditions from other threads should the child die instantly */
	pthread_mutex_lock(&child_proc_lock);

	/* Fork new process */
	if ((pid = fork()) == -1) {
		pthread_mutex_unlock(&child_proc_lock);
		snprintf(errstr, errstr_size,
			 "fork failed: %s", strerror(errno));
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}

	if (pid != 0) {
		/* Parent process */
		*pidp = pid;

		/* Add child proc to list of known child procs */
		child_proc_add(cmdstring, pid, statusp);
		pthread_mutex_unlock(&child_proc_lock);

		if (*rw == 'w') {
			close(pfd[0]);
			return pfd[1];
		} else {
			close(pfd[1]);
			return pfd[0];
		}
	}

	/* Child process */

	pthread_mutex_unlock(&child_proc_lock);

	/* Move away from parent's process group to avoid signal propagation */
	setpgid(0, getpid());

	if (*rw == 'w') { /* write */
		close(pfd[1]);
		if (pfd[0] != STDIN_FILENO) {
			dup2(pfd[0], STDIN_FILENO);
			close(pfd[0]);
		}

	} else { /* read */
		close(pfd[0]);
		if (pfd[1] != STDOUT_FILENO) {
			dup2(pfd[1], STDOUT_FILENO);
			close(pfd[1]);
		}
	}

	/* Execute command */
        execl("/bin/sh", "sh", "-c", cmdstring, NULL);
        _exit(127);
}



/**
 * Returns a human readable process exit reason based on the exit code.
 */
const char *exec_exitstatus (int status) {
	static __thread char ret[128];

	if (WIFEXITED(status)) {
		if (WEXITSTATUS(status) == 127)
			snprintf(ret, sizeof(ret),
				 "could not execute command");
		else
			snprintf(ret, sizeof(ret), "exited with status %i",
				 WEXITSTATUS(status));

	} else if (WIFSIGNALED(status)) {
#ifdef WCOREDUMP
		if (WCOREDUMP(status))
			snprintf(ret, sizeof(ret), "core dumped");
		else
#endif
			snprintf(ret, sizeof(ret), "terminated by signal %i",
				 WTERMSIG(status));
	} else
		snprintf(ret, sizeof(ret), "exited with code %i", status);

	return ret;
}


/**
 * Called from non-signal context to reap any child processes that
 * might have died lately.
 */
static void child_proc_reap (void) {
	pid_t pid;
	int st;

	atomic_set(&do_child_proc_reap, 0);

	pthread_mutex_lock(&child_proc_lock);
	while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
		struct child_proc *cp;

		cp = child_proc_find(pid);

		if (likely(cp != NULL))
			kt_log(LOG_INFO,
			       "Child process \"%s\" (%i) %s: "
			       "ran for %i seconds",
			       cp->cmd, pid, exec_exitstatus(st),
			       (int)(time(NULL) - cp->t_start));
		else
			kt_log(LOG_WARNING,
			       "Unknown child process (%i) %s",
			       pid, exec_exitstatus(st));
		if (!cp)
			continue;

		/* Store exit status for owner */
		atomic_set(cp->statusp, st);
		child_proc_del(cp);
	}
	pthread_mutex_unlock(&child_proc_lock);

}


static void sigchld (int sig) {
	/* Simply indicate to non-signal context to reap the child proc */
	atomic_set(&do_child_proc_reap, 1);
}



/**
 * Exec thread's main loop.
 * The exec thread is responsible for reaping spawned child processes.
 */
static void *exec_main (void *ignore) {
	time_t hard_timeout = 0;
	const int gracetime = 10;
	struct child_proc *cp;

	/* Install SIGCHLD handler to reap dying child processes */
        ezd_thread_sigmask(SIG_BLOCK, 0/*ALL*/, -1/*end*/);
        ezd_thread_sigmask(SIG_UNBLOCK, SIGCHLD, -1/*end*/);
	signal(SIGCHLD, sigchld);

	while (conf.run || child_proc_cnt > 0) {
		if (!conf.run) {
			/* Allow for childs to terminate, then kill them. */
			if (!hard_timeout) {
				hard_timeout = time(NULL) + gracetime;
				_DBG("Waiting %i seconds for "
				     "%i child processes to terminate",
				     gracetime, child_proc_cnt);
			} else if (hard_timeout < time(NULL))
				break;
		}

		if (do_child_proc_reap)
			child_proc_reap();
		usleep(500000);
	}

	/* Kill any remaining children */
	pthread_mutex_lock(&child_proc_lock);
	LIST_FOREACH(cp, &child_procs, link) {
		if (cp->pid <= 0)
			continue;

		kt_log(LOG_WARNING,
		       "Child process \"%s\" (%i) did not "
		       "terminate in %is: killing",
		       cp->cmd, (int)cp->pid, gracetime);
		kill(cp->pid, SIGTERM);
		sleep(1);
		kill(cp->pid, SIGKILL);
	}
	pthread_mutex_unlock(&child_proc_lock);

	child_proc_reap();

	return NULL;
}


/**
 * Terminate child process exec layer
 */
void exec_term (void) {
	void *ignore;
	pthread_join(exec_thread, &ignore);
}


/**
 * Initialize child process exec layer
 */
void exec_init (void) {
	int err;

	if ((err = pthread_create(&exec_thread, NULL, exec_main, NULL))) {
		kt_log(LOG_ERR,
		       "Failed to create exec_main thread: %s", strerror(err));
		exit(1);
	}
}
