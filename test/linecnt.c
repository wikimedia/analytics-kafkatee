/**
 * linecnt - simple output line counter for kafkatee testing
 */
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


#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>

static int run = 1;

static void do_timeout (int sig) {
	run = 0;
	fclose(stdin); /* trigger fgets() to quit */
}

int main (int argc, char **argv) {
	int cnt = 0;
	int expected;
	int sleeptime = 5;
	int endtime = 0;
	char buf[4096];

	if (argc < 3) {
		printf("Usage: %s <expected-number-of-lines-to-read> "
		       " <timeout> "
		       " [<sleep-time-after-expected-reached>]\n", argv[0]);
		exit(1);
	}

	expected = atoi(argv[1]);
	alarm(atoi(argv[2]));
	signal(SIGALRM, do_timeout);

	if (argc == 4)
		sleeptime = atoi(argv[3]);


	while (run && fgets(buf, sizeof(buf)-1, stdin)) {
		cnt++;

		if (cnt >= expected) {
			if (!endtime)
				endtime = time(NULL) + sleeptime;
			else if (time(NULL) >= endtime)
				break;
		}
	}

	printf("%i lines read, %i expected\n", cnt, expected);

	if (cnt == expected)
		exit(0);
	else
		exit(2);
}
