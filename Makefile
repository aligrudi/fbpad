CC = cc
CFLAGS = -Wall -Os
LDFLAGS =

all: fbpad
.c.o:
	$(CC) -c $(CFLAGS) $<
fbpad: fbpad.o term.o pad.o draw.o font.o
	$(CC) $(LDFLAGS) -o $@ $^
clean:
	rm -f *.o fbpad
