CFLAGS = -Wall -g -O0

all:
	gcc ${CFLAGS} -I. -c ebt.c -lpthread -DDEBUG

clean:
	rm -f *.o *.fifo