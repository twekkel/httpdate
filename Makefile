CC      ?= cc
CFLAGS  ?= -Wall -std=c11 -pedantic -O2 -flto
LDFLAGS ?=
LIBS    ?= -lcurl

PREFIX ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin
MANDIR ?= $(PREFIX)/share/man

all: httpdate

httpdate: httpdate.o ntpd.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ httpdate.o ntpd.o $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: httpdate
	./httpdate example.com example.net https://example.com

install: all
	mkdir -p $(DESTDIR)$(SBINDIR)
	mkdir -p $(DESTDIR)$(MANDIR)/man8
	install -m 755 httpdate $(DESTDIR)$(SBINDIR)/httpdate
	install -m 644 httpdate.8 $(DESTDIR)$(MANDIR)/man8/httpdate.8

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/httpdate
	rm -f $(DESTDIR)$(MANDIR)/man8/httpdate.8

clean:
	rm -f httpdate *.o

.PHONY: all clean install test uninstall
