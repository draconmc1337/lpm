CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Iinclude
SRCS = src/main.c src/util.c src/db.c src/pkgbuild.c \
       src/build.c src/search.c src/cache.c src/dep.c \
       src/config.c src/download.c src/checksum.c \
       src/transaction.c src/merge.c src/safety.c src/key.c
TARGET  = lpm
LIBLLPM = libllpm.a
LLPM_LIB_SRCS = src/libllpm/error.c src/libllpm/handle.c src/libllpm/repo.c src/libllpm/trans.c
all: $(TARGET)
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread
$(LIBLLPM): $(LLPM_LIB_SRCS)
	$(CC) $(CFLAGS) -c src/libllpm/error.c -o src/libllpm/error.o
	$(CC) $(CFLAGS) -c src/libllpm/handle.c -o src/libllpm/handle.o
	$(CC) $(CFLAGS) -c src/libllpm/repo.c -o src/libllpm/repo.o
	$(CC) $(CFLAGS) -c src/libllpm/trans.c -o src/libllpm/trans.o
	ar rcs $(LIBLLPM) src/libllpm/error.o src/libllpm/handle.o src/libllpm/repo.o src/libllpm/trans.o
install: $(TARGET)
	install -Dm755 $(TARGET) /usr/bin/lpm && \
	install -Dm644 lpm.conf /etc/lpm/lpm.conf
clean:
	rm -f $(TARGET) $(LIBLLPM) src/libllpm/*.o
.PHONY: all install clean
