PREFIX:=/usr

.PHONY=clean

all: main.o
	${CC} ${LDFLAGS} main.o -o parserver

%.o: %.c
	${CC} -Wall -std=c99 -D_GNU_SOURCE -c ${CFLAGS} $< -o $@

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install parserver $(DESTDIR)$(PREFIX)/bin/parserver

clean:
	rm -f *.o parserver
