CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Iinclude

# ── main lpm binary ───────────────────────────────────────────────────
SRCS = src/main.c src/util.c src/db.c src/pkgbuild.c \
       src/build.c src/search.c src/cache.c src/dep.c \
       src/config.c src/download.c src/checksum.c \
       src/transaction.c src/merge.c src/safety.c src/key.c \
       src/pkgbuild_parser.c src/recommend.c src/sync.c src/lpkg.c \
       src/buildmeta.c src/dryrun.c

TARGET  = lpm

# ── libllpm static library ────────────────────────────────────────────
LIBLLPM       = libllpm.a
LLPM_LIB_SRCS = \
    src/libllpm/error.c   \
    src/libllpm/handle.c  \
    src/libllpm/repo.c    \
    src/libllpm/trans.c   \
    src/libllpm/dep.c     \
    src/libllpm/keyring.c
LLPM_LIB_OBJS = $(LLPM_LIB_SRCS:.c=.o)

# ── install paths (all respect DESTDIR) ──────────────────────────────
DESTDIR  ?=
PREFIX   ?= /usr
BINDIR    = $(DESTDIR)$(PREFIX)/bin
LIBDIR    = $(DESTDIR)$(PREFIX)/lib
INCDIR    = $(DESTDIR)$(PREFIX)/include
ETCDIR    = $(DESTDIR)/etc
VARDIR    = $(DESTDIR)/var
LOGDIR    = $(DESTDIR)/var/log

# ── runtime directories created at install ───────────────────────────
RUNTIME_DIRS = \
    $(VARDIR)/lib/lpm/db         \
    $(VARDIR)/lib/lpm/files      \
    $(VARDIR)/lib/lpm/buildmeta  \
    $(VARDIR)/cache/lpm/packages \
    $(VARDIR)/log/lpm            \
    $(DESTDIR)/usr/src/lpm       \
    $(ETCDIR)/lpm/gnupg

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
	@echo "==> Creating runtime directories..."
	@for d in $(RUNTIME_DIRS); do \
	    install -dm755 "$$d"; \
	done
	@# gnupg needs stricter perms
	@chmod 700 $(ETCDIR)/lpm/gnupg

	@echo "==> Installing lpm binary..."
	install -Dm755 $(TARGET)    $(BINDIR)/lpm

	@echo "==> Installing default config..."
	@# Only install lpm.conf if it doesn't exist yet (never overwrite user config)
	@if [ ! -f $(ETCDIR)/lpm/lpm.conf ]; then \
	    install -Dm644 lpm.conf $(ETCDIR)/lpm/lpm.conf; \
	    echo "  -> installed default /etc/lpm/lpm.conf"; \
	else \
	    echo "  -> /etc/lpm/lpm.conf already exists, skipping"; \
	fi

	@echo "==> Installing libllpm..."
	install -Dm644 $(LIBLLPM)             $(LIBDIR)/libllpm.a
	install -Dm644 include/llpm/llpm.h    $(INCDIR)/llpm/llpm.h
	install -Dm644 include/llpm/error.h   $(INCDIR)/llpm/error.h
	install -Dm644 include/llpm/handle.h  $(INCDIR)/llpm/handle.h
	install -Dm644 include/llpm/repo.h    $(INCDIR)/llpm/repo.h
	install -Dm644 include/llpm/trans.h   $(INCDIR)/llpm/trans.h
	install -Dm644 include/llpm/dep.h     $(INCDIR)/llpm/dep.h
	install -Dm644 include/llpm/keyring.h $(INCDIR)/llpm/keyring.h

	@echo ""
	@echo "==> lpm installed successfully!"
	@echo "    binary : $(BINDIR)/lpm"
	@echo "    config : $(ETCDIR)/lpm/lpm.conf"
	@echo "    db     : $(VARDIR)/lib/lpm/db"
	@echo "    cache  : $(VARDIR)/cache/lpm"
	@echo "    logs   : $(LOGDIR)/lpm"
	@echo ""
	@echo "    Run 'lpm -K init' to initialize the keyring."

# ── uninstall ─────────────────────────────────────────────────────────
uninstall:
	rm -f  $(BINDIR)/lpm
	rm -f  $(LIBDIR)/libllpm.a
	rm -rf $(INCDIR)/llpm
	@echo "NOTE: config and data dirs left intact:"
	@echo "  $(ETCDIR)/lpm  $(VARDIR)/lib/lpm  $(VARDIR)/cache/lpm"
	@echo "Remove them manually if you want a clean purge."

# ── purge (uninstall + wipe all data) ────────────────────────────────
purge: uninstall
	rm -rf $(ETCDIR)/lpm
	rm -rf $(VARDIR)/lib/lpm
	rm -rf $(VARDIR)/cache/lpm
	rm -rf $(LOGDIR)/lpm
	rm -rf $(DESTDIR)/usr/src/lpm
	@echo "==> lpm purged."

clean:
	rm -f $(TARGET) $(LIBLLPM) src/libllpm/*.o

.PHONY: all install uninstall purge clean
