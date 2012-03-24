PREFIX:=/usr
BINDIR:=$(PREFIX)/bin
MANDIR:=$(PREFIX)/share/man

.PHONY=clean

all: main.o
	${CC} ${LDFLAGS} main.o -o parserver

%.o: %.c
	${CC} -Wall -std=c99 -D_GNU_SOURCE -c ${CFLAGS} $< -o $@

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(MANDIR)/man1
	install parserver $(DESTDIR)$(BINDIR)/parserver
	install parserver.1 $(DESTDIR)$(MANDIR)/man1

clean:
	rm -f *.o parserver
