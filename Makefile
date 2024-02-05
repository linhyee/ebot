CFLAGS = -Wall -g -O0

test:
	gcc ${CFLAGS} -I. -c ebot.c test.c -lpthread

clean:
	rm -f *.o *.fifo