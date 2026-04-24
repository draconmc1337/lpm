CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Iinclude
SRCS = src/main.c src/util.c src/db.c src/pkgbuild.c \
       src/build.c src/search.c src/cache.c src/dep.c \
       src/config.c src/download.c src/checksum.c \
       src/transaction.c src/merge.c src/safety.c
TARGET  = lpm
all: $(TARGET)
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread
install: $(TARGET)
	install -Dm755 $(TARGET) /usr/bin/lpm && \
	install -Dm644 lpm.conf /etc/lpm/lpm.conf
clean:
	rm -f $(TARGET)
.PHONY: all install clean
