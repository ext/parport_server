all: main.o
	${CC} ${LDFLAGS} main.o -o parserver

%.o: %.c
	${CC} -Wall -std=c99 -D_GNU_SOURCE -c ${CFLAGS} $< -o $@

install:
	install -d $(DESTDIR)/usr/bin
	install parserver $(DESTDIR)/usr/bin/parserver

clean:
	rm -f *.o parserver
