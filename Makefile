CC      ?= cc
CFLAGS  ?= -Wall -std=c11 -pedantic -O2 -flto
LDFLAGS ?=
LIBS    ?= $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl)

PREFIX ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin
MANDIR ?= $(PREFIX)/share/man

# The NTP server (-S) is built in by default; set WITH_NTP=0 to build without
WITH_NTP ?= 1

# Prometheus metrics (-M) are built in by default; set WITH_METRICS=0 to build without
WITH_METRICS ?= 1

DEPFLAGS = -MMD -MP

OBJS = httpdate.o log.o utils.o
NTPCFLAGS =
ifeq ($(WITH_NTP),1)
OBJS += ntpd.o
NTPCFLAGS = -DWITH_NTP
endif

METRICSCFLAGS =
ifeq ($(WITH_METRICS),1)
METRICSCFLAGS = -DWITH_METRICS
endif

all: httpdate

httpdate: $(OBJS)
	$(CC) $(CFLAGS) $(NTPCFLAGS) $(METRICSCFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(NTPCFLAGS) $(METRICSCFLAGS) $(DEPFLAGS) -c $< -o $@

test: httpdate
	./httpdate example.com example.net https://example.com

install: all
	install -d $(DESTDIR)$(SBINDIR) $(DESTDIR)$(MANDIR)/man8
	install -m 755 httpdate $(DESTDIR)$(SBINDIR)/httpdate
	install -m 644 httpdate.8 $(DESTDIR)$(MANDIR)/man8/httpdate.8

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/httpdate
	rm -f $(DESTDIR)$(MANDIR)/man8/httpdate.8

clean:
	rm -f httpdate *.o *.d

-include $(wildcard *.d)

.PHONY: all clean install test uninstall
