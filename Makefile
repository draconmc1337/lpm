CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Iinclude

# ── versioning ────────────────────────────────────────────────────────
# Bump SOVERSION when ABI breaks (new/removed symbols, struct layout change)
# Bump VERSION for any other change
SOVERSION   = 1
VERSION     = 2.1.0

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
       src/buildmeta.c src/dryrun.c src/verify.c src/audit.c src/ui.c

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
.PHONY: all install uninstall clean test test-asan manpages clean-doc

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
install: all manpages
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
	# man pages
	install -Dm644 lpm.1       $(DESTDIR)/usr/share/man/man1/lpm.1
	install -Dm644 lpm.conf.5  $(DESTDIR)/usr/share/man/man5/lpm.conf.5
	install -Dm644 PKGBUILD.5  $(DESTDIR)/usr/share/man/man5/PKGBUILD.5

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
	rm -f /usr/share/man/man1/lpm.1
	rm -f /usr/share/man/man5/lpm.conf.5
	rm -f /usr/share/man/man5/PKGBUILD.5

# ── man pages ─────────────────────────────────────────────────────────
# pacman-style: AsciiDoc sources compiled with asciidoctor (Ruby).
# Requires `asciidoctor` (gem install asciidoctor) on the build host.
DESTDIR     ?=
ASCIIDOCTOR ?= asciidoctor
MANPAGES     = lpm.1 lpm.conf.5 PKGBUILD.5
MAN_SRCS     = doc/lpm.1.asciidoc doc/lpm.conf.5.asciidoc doc/PKGBUILD.5.asciidoc

manpages: $(MANPAGES)

$(MANPAGES): %: doc/%.asciidoc
	@command -v $(ASCIIDOCTOR) >/dev/null 2>&1 || { \
	    echo "asciidoctor not found — install it (e.g. 'gem install asciidoctor') to build man pages"; exit 1; }
	$(ASCIIDOCTOR) -b manpage -a revnumber=$(VERSION) -o $@ $<

clean-doc:
	rm -f $(MANPAGES)

# ── tests ───────────────────────────────────────────────────────────────
TEST_BINS = tests/test_config tests/test_copy tests/test_ui

test: $(TEST_BINS)
	@echo "== static g_cfg single-source guard =="
	@if grep -rn "LpmConfig cfg;" src/*.c | grep -v "src/config.c"; then \
	    echo "FAIL: divergent local config struct — handlers must read g_cfg"; exit 1; \
	fi
	@if grep -rn "lpm_config_load" src/*.c | grep -v "src/config.c"; then \
	    echo "FAIL: lpm_config_load called outside config.c"; exit 1; \
	fi
	@echo "  ok: g_cfg written only by lpm_config_init()"
	@echo "== runtime config regression test =="
	@./tests/test_config
	@echo "== copy / .lpkg round-trip regression test =="
	@./tests/test_copy
	@echo "== atomic output writer (no interleaving) test =="
	@./tests/test_ui

tests/test_config: tests/test_config.c src/config.c src/util.c src/ui.c include/lpm.h
	$(CC) $(CFLAGS) -o $@ tests/test_config.c src/config.c src/util.c src/ui.c -lpthread

tests/test_copy: tests/test_copy.c src/sha256.c src/util.c src/ui.c include/lpm.h
	$(CC) $(CFLAGS) -o $@ tests/test_copy.c src/sha256.c src/util.c src/ui.c -lpthread

tests/test_ui: tests/test_ui.c src/ui.c src/util.c include/lpm.h
	$(CC) $(CFLAGS) -o $@ tests/test_ui.c src/ui.c src/util.c -lpthread

test-asan:
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
	    -o tests/test_config tests/test_config.c src/config.c src/util.c src/ui.c -lpthread
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
	    -o tests/test_copy tests/test_copy.c src/sha256.c src/util.c src/ui.c -lpthread
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
	    -o tests/test_ui tests/test_ui.c src/ui.c src/util.c -lpthread
	@echo "== ASAN/UBSAN: config =="
	@./tests/test_config
	@echo "== ASAN/UBSAN: copy round-trip =="
	@./tests/test_copy
	@echo "== ASAN/UBSAN: atomic writer =="
	@./tests/test_ui

# ── clean ─────────────────────────────────────────────────────────────
clean:
	rm -f $(TARGET)
	rm -f $(LIBLLPM_SO) $(LIBLLPM_A) libllpm.so
	rm -f src/libllpm/*.o src/libllpm/*.pic.o
	rm -f $(TEST_BINS)
