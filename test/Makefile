TESTS ?= test-*.conf

.PHONY:
.PRECIOUS: %.conf

%.conf: .PHONY
	@(echo "\033[34m######## Test $@ START #########\033[0m" ; \
	../kafkatee -c $@ -p kafkatee.test.pid -D -x ; \
	if [ $$? -eq 0 ]; then \
		echo "\033[32m######## Test $@ PASSED #########\033[0m" ; \
		true ; \
	else \
		echo "\033[31m######## Test $@ FAILED #########\033[0m" ; \
		false ; \
	fi)

all: linecnt ingen $(TESTS)

linecnt: linecnt.c
	$(CC) -O2 -Wall -Werror $^ -o $@

ingen: ingen.c
	$(CC) -O2 -Wall -Werror $^ -o $@

