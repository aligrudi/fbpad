CC = cc
CFLAGS = -Wall -O2
LDFLAGS =

all: fbpad
.c.o:
	$(CC) -c $(CFLAGS) $<
term.o: vt102.c
fbpad: fbpad.o term.o pad.o draw.o font.o
	$(CC) $(LDFLAGS) -o $@ $^
clean:
	rm -f *.o fbpad
