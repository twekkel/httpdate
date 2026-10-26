PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CC     ?= gcc
CFLAGS ?= -Wall -std=c11 -pedantic -O2 -flto
LIBS   := -lcurl -lm

INSTALL = install
STRIP   = strip -s

SRC = httpdate.c
OBJ = httpdate.o

all: httpdate

httpdate: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

httpdate.o: httpdate.c

test: httpdate
	./httpdate https://www.example.com

install: httpdate
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 755 httpdate $(DESTDIR)$(BINDIR)/httpdate
	$(STRIP) $(DESTDIR)$(BINDIR)/httpdate

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/httpdate

clean:
	rm -f httpdate $(OBJ)

.PHONY: all test install uninstall clean
