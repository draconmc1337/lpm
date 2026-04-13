<div align="center">
  <h1>LPM — Lotus Package Manager</h1>
  <p><i>Build it. Own it. No magic.</i></p>
</div>

<p align="center">
  <a href="https://github.com/draconmc1337/lpm">
    <img src="https://img.shields.io/badge/github-lpm-11111b?style=for-the-badge&logo=github">
  </a>
</p>

<p align="center">
  <img src="https://img.shields.io/github/last-commit/draconmc1337/lpm?style=for-the-badge&color=30575F">
  <img src="https://img.shields.io/github/stars/draconmc1337/lpm?style=for-the-badge&color=A56039">
  <img src="https://img.shields.io/github/issues/draconmc1337/lpm?style=for-the-badge&color=5a5e6f">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C-0b0e1b?style=for-the-badge&logo=c&logoColor=c2c2c6">
  <img src="https://img.shields.io/badge/Lotus_Linux-30575F?style=for-the-badge">
  <img src="https://img.shields.io/badge/license-GPL--3.0-5a5e6f?style=for-the-badge">
  <img src="https://img.shields.io/badge/1.1.5--alpha-A56039?style=for-the-badge">
</p>

> **⚠ Alpha software.** LPM is under active development for Lotus Linux. Expect rough edges — but it already builds GCC 15.2.0 from scratch, so it's not *that* rough.

A source-based package manager for [Lotus Linux](https://draconmc1337.github.io), written in C.  
PKGBUILDs are plain bash scripts. No sandbox, no namespace magic, no fakechroot.  
You write the build script, LPM runs it — and owns the filesystem so nothing can sneak a `rm -rf /` past you.

---

## Why LPM?

Most package managers either trust the script too much (old LPM, AUR helpers) or wrap everything in so many layers you forget what's actually happening (Portage, I'm looking at you).

LPM lands somewhere in the middle:
- **PKGBUILD does the building** — configure, make, DESTDIR install. That's it.
- **LPM owns the filesystem** — after `package()` runs into `$pkgdir`, LPM scans, records, and merges. Uninstall is just reading the list back and calling `unlink()`. No script runs.
- **Config drives protection** — `/etc/lpm/lpm.conf` decides what's critical. htop gets a simple `[Yes/No]`. glibc gets three confirmation prompts and has to type `DELETE`.

---

## Features

- **PKGBUILD-based** — same format as Arch, familiar and simple
- **File ownership database** — every installed file tracked at `/var/lib/lpm/files/<pkg>/files.list`
- **Tiered removal protection** — NORMAL / IMPORTANT / CRITICAL, driven by `/etc/lpm/lpm.conf`
- **Dependency resolution** — toposorted build queue with cycle detection
- **Per-package build logs** — `/var/log/lpm/<pkgname>.log`, overwritten each run. No 50k-line pile-up.
- **Fetch retry system** — 3 attempts with 1s delay, DNS failure detection, detailed error classification
- **`--yes` / `--strict` / `--bootstrap`** — automation-friendly flags for scripting, CI, and LFS builds
- **`IgnorePkg`** — skip packages during `lpm -u`, same as pacman
- **`MAKEFLAGS`** — configurable via `lpm.conf`, defaults to `ceil(nproc/2)`
- **`lpm-dev`** — separate binary for developer workflow, `-bi` only, no repo fetch

---

## Installation

```sh
git clone https://github.com/draconmc1337/lpm
cd lpm
make
doas make install        # installs lpm to /usr/bin/lpm
make dev                 # build lpm-dev
doas make dev-install    # installs lpm-dev to /usr/bin/lpm-dev
```

---

## Commands

```sh
lpm -S   <pkg...>   # fetch PKGBUILD from repo + build + install
lpm -bi  <pkg...>   # build + install from local PKGBUILD (offline)
lpm -Sy  <pkg...>   # fetch PKGBUILD only, don't build
lpm -r   <pkg...>   # remove package(s)
lpm -u   [pkg...]   # update — all installed if no args
lpm -s   <term>     # search available packages
lpm -D   <pkg...>   # show dependency tree
lpm -qi  <pkg...>   # package info
lpm -c   <pkg...>   # run check() test suite
lpm -rcc [pkg...]   # clean build cache
lpm -l              # list installed packages
lpm -l --count      # print number of installed packages (bare integer)
```

### Flags for `-S` and `-bi`

| Flag | Description |
|------|-------------|
| `--yes` | Skip all prompts, auto-run `check()` |
| `--strict` | `check()` failure blocks install — hard stop |
| `--bootstrap` | LFS/fresh system mode: auto-yes, skip `check()`, ignore strict |

```sh
# typical developer run — no questions asked
lpm -bi gcc --yes

# CI / automated build — fail fast on test failures
lpm -bi python3 --yes --strict

# LFS build — no questions, no tests, just install
lpm -bi setuptools ninja meson kmod coreutils --bootstrap
```

### Flags for `-r`

| Flag | Description |
|------|-------------|
| `--force` | Override dep check and CriticalPkg protection |
| `--no-confirm` | Non-interactive removal (scripts only) |

---

## PKGBUILD format

Standard bash script. Four functions, all optional except `package()`:

```bash
pkgname="htop"
pkgver="3.3.0"
pkgrel="1"
depends=("ncurses")
makedepends=()
source="https://github.com/htop-dev/htop/releases/download/${pkgver}/htop-${pkgver}.tar.xz"
sha256sums="abc123..."

build() {
    tar -xf htop-${pkgver}.tar.xz
    cd htop-${pkgver}
    ./configure --prefix=/usr
    make -j$(nproc)
}

check() {
    cd htop-${pkgver}
    make check
}

package() {
    cd htop-${pkgver}
    make DESTDIR="$pkgdir" install
}
```

`uninstall()` — can exist in the file, **will never be executed**. Removal is handled entirely by LPM via `files.list`. This is intentional and not changing.

---

## Configuration — `/etc/lpm/lpm.conf`

Created automatically on first run with sane defaults.

```ini
# Packages requiring triple-YES confirmation before removal.
# Without --force, these are hard-blocked.
CriticalPkg = glibc gcc binutils bash coreutils sed grep gawk
CriticalPkg = findutils tar make patch perl python3 openssl
CriticalPkg = linux dinit lpm

# Packages skipped during 'lpm -u'.
# Can still be updated manually with 'lpm -u <pkg>'.
IgnorePkg =

# Passed to every make invocation during build.
# Default: ceil(nproc/2) — conservative to avoid OOM on large builds.
MAKEFLAGS = -j4

# Skip all confirmation prompts (automation/scripting).
DEFAULT_YES = n

# Treat check() failure as a fatal install block globally.
DEFAULT_STRICT = n

# Automatically run check() after build without asking.
RUN_CHECK = n

# check() failure blocks install (same as --strict flag).
STRICT_BUILD = n

# Per-package build log directory.
LogDir = /var/log/lpm

# File ownership database root.
FilesDir = /var/lib/lpm/files
```

`CriticalPkg` lines are additive — split across multiple lines for readability.  
Boolean values: only `y` or `n` accepted — anything else is a hard error.

---

## Removal flow

LPM classifies packages at removal time:

**🟢 NORMAL** — no reverse deps, not in CriticalPkg:
```
Packages to remove (1):
  htop

Would you like to remove these packages? [Yes/No]
```

**🟡 IMPORTANT** — has reverse deps, not in CriticalPkg:
```
warning: Removing 'ffmpeg' may break some applications.
Required by: mpv vlc

Proceed? [Yes/No]
```

**🔴 CRITICAL** — in CriticalPkg (requires `--force`):
```
ERROR: GLIBC IS A CRITICAL PACKAGE!!
REQUIRED BY: gcc bash coreutils ... 4 more

YOU ARE ABOUT TO BREAK YOUR SYSTEM.
THIS ACTION CANNOT BE UNDONE.

Type YES to continue:
> YES

FINAL CONFIRMATION: REMOVE THESE PACKAGES? (type YES to continue)
> YES

LAST WARNING: SYSTEM MAY BECOME UNUSABLE.
Type DELETE to proceed:
> DELETE
```

---

## lpm-dev

A stripped-down binary for the Lotus developer workflow. Exposes only `-bi`.  
No removal, no repo fetch, no critical package protection — you're the developer, you know what you're doing.

```sh
lpm-dev -bi gcc            # build + install, asks at each step
lpm-dev -bi gcc --yes      # fully automatic
lpm-dev -bi gcc --yes --strict  # automatic, fails hard on test failures
```

Shares the same DB, file ownership records, and logs as `lpm` — fully compatible.

---

## File layout

```
lpm/
├── include/
│   ├── lpm.h          # types, structs, defines, all prototypes
│   └── config.h       # LpmConfig struct, parser prototypes
├── src/
│   ├── main.c         # entry point, command dispatch
│   ├── main_dev.c     # lpm-dev entry point
│   ├── build.c        # cmd_sync, cmd_local, cmd_remove, cmd_update, cmd_check
│   ├── build_dev.c    # cmd_local_dev — developer build+install
│   ├── config.c       # /etc/lpm/lpm.conf parser
│   ├── db.c           # installed DB, file ownership database
│   ├── dep.c          # dependency resolution, toposort
│   ├── pkgbuild.c     # PKGBUILD parser, reverse_deps
│   ├── search.c       # cmd_search, cmd_info, cmd_list
│   ├── cache.c        # cmd_rcc — build cache cleanup
│   └── util.c         # die, warn, confirm, lpm_log, lpm_audit
└── Makefile
```

---

## Paths

| Path | Purpose |
|------|---------|
| `/usr/src/lpm/pkgbuild_<name>` | PKGBUILD storage |
| `/var/cache/lpm/<name>/` | Build workspace |
| `/var/lib/lpm/installed` | Installed package DB |
| `/var/lib/lpm/files/<name>/files.list` | File ownership list |
| `/var/log/lpm/<name>.log` | Per-package build log |
| `/var/log/lpm/audit.log` | Security audit log |
| `/etc/lpm/lpm.conf` | Configuration |

---

## License

GPL-3.0
