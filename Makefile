CC = cc
CFLAGS = -std=gnu89 -pedantic -Wall -O2 `pkg-config --cflags freetype2`
LDFLAGS = -lutil `pkg-config --libs freetype2`

all: fbpterm
.c.o:
	$(CC) -c $(CFLAGS) $<
fbpterm: fbpterm.o pad.o draw.o util.o
	$(CC) $(LDFLAGS) -o $@ $^
clean:
	rm -f *.o fbpterm
