
CC      := gcc
CFLAGS  := -g -Wall -O3 `pkg-config --cflags twolame`
LDFLAGS := -g `pkg-config --libs twolame`
OBJS    := adrenc.o

all: adrenc

adrenc: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c Makefile
	$(CC) $(CFLAGS) -c $< -o $@

install:
	cp -f hacktv $(PREFIX)/usr/local/bin/

clean:
	rm -f *.o *.d adrenc

