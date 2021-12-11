prefix = $(DESTDIR)/usr
bindir = ${prefix}/bin

CC     ?= gcc
CFLAGS += -Wall -std=c11 -pedantic -O2

INSTALL = install -c
STRIP   = strip -s

all: httpdate

httpdate: httpdate.c utils.c
	$(CC) $(CFLAGS) -c httpdate.c utils.c
	$(CC) $(CFLAGS) -o httpdate utils.o httpdate.o -lcurl

test:
	./httpdate https://www.example.com

clean:
	rm -rf httpdate httpdate.o utils.o
