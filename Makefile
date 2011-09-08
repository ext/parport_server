all: main.o
	${CC} ${LDFLAGS} main.o -o parserver

%.o: %.c
	${CC} -Wall -std=c99 -D_GNU_SOURCE -c ${CFLAGS} $< -o $@

clean:
	rm -f *.o parserver

