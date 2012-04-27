CC = cc
CFLAGS = -Wall -O2
LDFLAGS =

all: fbpad
%.o: %.c config.h
	$(CC) -c $(CFLAGS) $<
fbpad: fbpad.o term.o pad.o draw.o font.o scrsnap.o
	$(CC) -o $@ $^ $(LDFLAGS)
clean:
	rm -f *.o fbpad
