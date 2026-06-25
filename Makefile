CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Iinclude

# ── versioning ────────────────────────────────────────────────────────
# Bump SOVERSION when ABI breaks (new/removed symbols, struct layout change)
# Bump VERSION for any other change
SOVERSION   = 1
VERSION     = 1.2.0

# ── libllpm source files ──────────────────────────────────────────────
LLPM_LIB_SRCS = \
    src/libllpm/error.c   \
    src/libllpm/handle.c  \
    src/libllpm/repo.c    \
    src/libllpm/trans.c   \
    src/libllpm/dep.c     \
    src/libllpm/keyring.c

# Object files compiled with -fPIC (shared) and without (static)
LLPM_PIC_OBJS  = $(LLPM_LIB_SRCS:.c=.pic.o)
LLPM_OBJS      = $(LLPM_LIB_SRCS:.c=.o)

# Output names
LIBLLPM_A   = libllpm.a
LIBLLPM_SO  = libllpm.so.$(SOVERSION)
LIBLLPM_SONAME = libllpm.so.$(SOVERSION)

# ── lpm binary ────────────────────────────────────────────────────────
# Links against the shared library (lpm is the "pacman" of this setup;
# libllpm is its "libalpm" — lpm depends on it at runtime)
SRCS = src/main.c src/util.c src/db.c src/pkgbuild.c \
       src/build.c src/search.c src/cache.c src/dep.c \
       src/config.c src/download.c src/checksum.c src/sha256.c \
       src/transaction.c src/merge.c src/safety.c src/key.c src/profile.c \
       src/pkgbuild_parser.c src/recommend.c src/sync.c src/lpkg.c \
       src/buildmeta.c src/dryrun.c src/verify.c src/audit.c

TARGET = lpm

# ── runtime directories ───────────────────────────────────────────────
RUNTIME_DIRS = \
    /usr/src/lpm \
    /var/lib/lpm/db \
    /var/lib/lpm/files \
    /var/lib/lpm/buildmeta \
    /var/cache/lpm \
    /var/cache/lpm/packages \
    /var/log/lpm \
    /etc/lpm

# ── build rules ───────────────────────────────────────────────────────
.PHONY: all install uninstall clean

all: $(LIBLLPM_SO) $(LIBLLPM_A) $(TARGET)

# Shared library: compile with -fPIC, link with soname
$(LIBLLPM_SO): $(LLPM_PIC_OBJS)
	$(CC) -shared -Wl,-soname,$(LIBLLPM_SONAME) -o $@ $^
	ln -sf $(LIBLLPM_SO) libllpm.so

src/libllpm/%.pic.o: src/libllpm/%.c
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

# Static library: plain objects (no -fPIC, better for static builds/tools)
$(LIBLLPM_A): $(LLPM_OBJS)
	ar rcs $@ $^

src/libllpm/%.o: src/libllpm/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# lpm binary: link dynamically against libllpm.so
# At runtime it needs libllpm.so.$(SOVERSION) in /usr/lib
$(TARGET): $(SRCS) $(LIBLLPM_SO)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -L. -lllpm -lpthread \
	    -Wl,-rpath,/usr/lib

# ── install ───────────────────────────────────────────────────────────
install: all
	@for d in $(RUNTIME_DIRS); do \
	    install -dm755 "$$d"; \
	done
	# binary
	install -Dm755 $(TARGET)       /usr/bin/lpm
	# shared library: install versioned, create soname + dev symlinks
	# versioned file (the real .so)
	install -Dm755 $(LIBLLPM_SO)   /usr/lib/$(LIBLLPM_SO)
	# libllpm.so → libllpm.so.1  (dev symlink for -lllpm)
	ln -sf $(LIBLLPM_SO) /usr/lib/libllpm.so
	# static library (for developers building against libllpm)
	install -Dm644 $(LIBLLPM_A)    /usr/lib/$(LIBLLPM_A)
	# run ldconfig so the dynamic linker finds the new .so
	ldconfig
	# config
	install -Dm644 lpm.conf        /etc/lpm/lpm.conf
	# headers
	install -Dm644 include/llpm/llpm.h    /usr/include/llpm/llpm.h
	install -Dm644 include/llpm/error.h   /usr/include/llpm/error.h
	install -Dm644 include/llpm/handle.h  /usr/include/llpm/handle.h
	install -Dm644 include/llpm/repo.h    /usr/include/llpm/repo.h
	install -Dm644 include/llpm/trans.h   /usr/include/llpm/trans.h
	install -Dm644 include/llpm/dep.h     /usr/include/llpm/dep.h
	install -Dm644 include/llpm/keyring.h /usr/include/llpm/keyring.h
	# shell completions
	install -Dm644 completions/_lpm          /usr/share/zsh/site-functions/_lpm
	install -Dm644 completions/lpm.bash      /usr/share/bash-completion/completions/lpm

# ── uninstall ─────────────────────────────────────────────────────────
uninstall:
	rm -f /usr/bin/lpm
	rm -f /usr/lib/$(LIBLLPM_SO)
	rm -f /usr/lib/libllpm.so.$(SOVERSION)
	rm -f /usr/lib/libllpm.so
	rm -f /usr/lib/$(LIBLLPM_A)
	ldconfig
	rm -f /usr/include/llpm/llpm.h    \
	      /usr/include/llpm/error.h   \
	      /usr/include/llpm/handle.h  \
	      /usr/include/llpm/repo.h    \
	      /usr/include/llpm/trans.h   \
	      /usr/include/llpm/dep.h     \
	      /usr/include/llpm/keyring.h
	rmdir --ignore-fail-on-non-empty /usr/include/llpm 2>/dev/null || true
	rm -f /usr/share/zsh/site-functions/_lpm
	rm -f /usr/share/bash-completion/completions/lpm

# ── clean ─────────────────────────────────────────────────────────────
clean:
	rm -f $(TARGET)
	rm -f $(LIBLLPM_SO) $(LIBLLPM_A) libllpm.so
	rm -f src/libllpm/*.o src/libllpm/*.pic.o
