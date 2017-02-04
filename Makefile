PREFIX=/usr/local
CFLAGS=-std=c99 -Os -Wall

all: htrack

install: htrack
	install -s htrack $(PREFIX)/bin

clean:
	rm -f *.[ios] htrack
