PREFIX ?= /usr/local
CC ?= cc

CFLAGS ?= -std=c23 -Wall -Wextra -Wpedantic \
	-Wno-deprecated-declarations \
	-D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700L \
	-DVERSION=\"0.1.0\" -I.

LDFLAGS ?= -lX11

SRC = util.c xtile.c
OBJ = $(SRC:.c=.o)

all: xtile

config.h:
	cp config.def.h config.h

xtile: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

install: xtile
	mkdir -p $(PREFIX)/bin
	cp xtile $(PREFIX)/bin/xtile
	chmod 755 $(PREFIX)/bin/xtile

uninstall:
	rm -f $(PREFIX)/bin/xtile

clean:
	rm -f xtile $(OBJ)

run:
	startx debug/xinitrc

.PHONY: all clean install uninstall run
