CC = cc
CFLAGS = -std=gnu89 -pedantic -Wall -O2 `pkg-config --cflags freetype2`
LDFLAGS = -lutil `pkg-config --libs freetype2`

all: fbpad
.c.o:
	$(CC) -c $(CFLAGS) $<
fbpad: fbpad.o term.o pad.o draw.o util.o font.o
	$(CC) $(LDFLAGS) -o $@ $^
clean:
	rm -f *.o fbpad
