CC = cc
CFLAGS = -Wall -O2
LDFLAGS =

OBJS = fbpad.o term.o pad.o draw.o font.o isdw.o scrsnap.o conf.o

all: fbpad
.c.o:
	$(CC) -c $(CFLAGS) $<
fbpad: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)
clean:
	rm -f *.o fbpad
