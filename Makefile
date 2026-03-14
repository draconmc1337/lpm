CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Iinclude
SRCS    = src/main.c src/util.c src/db.c src/pkgbuild.c \
          src/build.c src/search.c src/cache.c
TARGET  = lpm

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/bin/lpm

clean:
	rm -f $(TARGET)

.PHONY: all install clean
