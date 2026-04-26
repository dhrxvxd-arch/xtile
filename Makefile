PREFIX ?= /usr/local
CC ?= cc
STRIP ?= strip

CFLAGS ?= -std=c23 -Wall -Wextra -Wpedantic \
	-Wno-deprecated-declarations \
	-D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700L \
	-DVERSION=\"0.1.0\" \
	-O2 -march=native -flto \
	-ffunction-sections -fdata-sections \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-fno-stack-protector \
	-fvisibility=hidden -static

LDFLAGS ?= -lX11 \
	-flto \
	-Wl,--gc-sections \
	-Wl,-O1

SRC = xtile.c
OBJ = $(SRC:.c=.o)

all: xtile

xtile: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

install: xtile
	mkdir -p $(PREFIX)/bin
	cp xtile $(PREFIX)/bin/xtile
	$(STRIP) $(PREFIX)/bin/xtile
	chmod 755 $(PREFIX)/bin/xtile

uninstall:
	rm -f $(PREFIX)/bin/xtile

clean:
	rm -f xtile $(OBJ)

run:
	startx ./xinitrc

.PHONY: all clean install uninstall run