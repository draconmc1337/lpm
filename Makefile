CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Iinclude

# ── main lpm binary ───────────────────────────────────────────────────
SRCS = src/main.c src/util.c src/db.c src/pkgbuild.c \
       src/build.c src/search.c src/cache.c src/dep.c \
       src/config.c src/download.c src/checksum.c \
       src/transaction.c src/merge.c src/safety.c src/key.c src/profile.c
TARGET  = lpm

# ── libllpm static library ────────────────────────────────────────────
LIBLLPM      = libllpm.a
LLPM_LIB_SRCS = \
    src/libllpm/error.c   \
    src/libllpm/handle.c  \
    src/libllpm/repo.c    \
    src/libllpm/trans.c   \
    src/libllpm/dep.c     \
    src/libllpm/keyring.c

LLPM_LIB_OBJS = $(LLPM_LIB_SRCS:.c=.o)

# ── rules ─────────────────────────────────────────────────────────────
all: $(TARGET) $(LIBLLPM)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

$(LIBLLPM): $(LLPM_LIB_OBJS)
	ar rcs $@ $^

src/libllpm/%.o: src/libllpm/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# ── install ───────────────────────────────────────────────────────────
install: $(TARGET) $(LIBLLPM)
	install -Dm755 $(TARGET)  /usr/bin/lpm
	install -Dm644 lpm.conf   /etc/lpm/lpm.conf
	install -Dm644 $(LIBLLPM) /usr/lib/libllpm.a
	install -Dm644 include/llpm/llpm.h    /usr/include/llpm/llpm.h
	install -Dm644 include/llpm/error.h   /usr/include/llpm/error.h
	install -Dm644 include/llpm/handle.h  /usr/include/llpm/handle.h
	install -Dm644 include/llpm/repo.h    /usr/include/llpm/repo.h
	install -Dm644 include/llpm/trans.h   /usr/include/llpm/trans.h
	install -Dm644 include/llpm/dep.h     /usr/include/llpm/dep.h
	install -Dm644 include/llpm/keyring.h /usr/include/llpm/keyring.h

clean:
	rm -f $(TARGET) $(LIBLLPM) src/libllpm/*.o

.PHONY: all install clean
