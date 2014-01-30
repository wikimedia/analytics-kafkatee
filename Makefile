
NAME     =kafkatee
VER     := `git describe --abbrev=6 --tags HEAD --always`
CFLAGS  +=-DKAFKATEE_VERSION="\"$(VER)\""
DESTDIR ?=/usr/local

SRCS=	kafkatee.c config.c queue.c input.c output.c exec.c format.c ezd.c

OBJS=	$(SRCS:.c=.o)
DEPS=	${OBJS:%.o=%.d}

LIBS=	-lyajl -lrdkafka -lrt -lpthread -lz

CFLAGS+=-O2 -Wall -Werror -Wfloat-equal -Wpointer-arith -g


# Profiling
#CFLAGS+=-O0
#CFLAGS += -pg
#LDFLAGS += -pg

.PHONY:

all: $(NAME)

%.o: %.c
	$(CC) -MD -MP $(CFLAGS) -c $<

$(NAME): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $(NAME) $(LIBS)

install:
	if [ "$(DESTDIR)" != "/usr/local" ]; then \
		DESTDIR="$(DESTDIR)/usr"; \
	else \
		DESTDIR="$(DESTDIR)" ; \
	fi ; \
	install -t $$DESTDIR/bin $(NAME)

test: .PHONY
	make -C test

clean:
	rm -f $(OBJS) $(DEPS) $(NAME)

-include $(DEPS)
