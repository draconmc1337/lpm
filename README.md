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
  <img src="https://img.shields.io/badge/2.0.0-A56039?style=for-the-badge">
</p>

A source-based package manager for [Lotus Linux](https://draconmc1337.github.io), written in C.
PKGBUILDs are plain bash scripts. No sandbox, no namespace magic, no fakechroot.
You write the build script, LPM runs it — and owns the filesystem so nothing can sneak a `rm -rf /` past you.

---

## Why LPM?

Most package managers either trust the script too much (AUR helpers) or wrap everything in so many layers you forget what's actually happening.

LPM lands somewhere in the middle:
- **PKGBUILD does the building** — configure, make, DESTDIR install. That's it.
- **LPM owns the filesystem** — after `package()` runs into `$pkgdir`, LPM scans, records, and merges. Uninstall reads the file list and unlinks. No script runs.
- **Config drives protection** — `/etc/lpm/lpm.conf` lists critical packages. Removing them requires `--force`.

---

## Features

- **PKGBUILD-based** — familiar Arch-style recipes
- **File ownership database** — every installed file tracked at `/var/lib/lpm/files/<pkg>/files.list`
- **Critical package protection** — `CriticalPkg` entries require `--force` to remove
- **Dependency resolution** — toposorted build queue with cycle detection
- **Per-package build logs** — `/var/log/lpm/<pkgname>.log`
- **`IgnorePkg`** — skip packages during system upgrades
- **Configurable build environment** — `CFLAGS`, `JOBS`, `MAKEFLAGS`, and related options in `lpm.conf`

---

## Installation

```sh
git clone https://github.com/draconmc1337/lpm
cd lpm
make
doas make install
```

---

## Commands

```sh
lpm install   <pkg...>      # fetch PKGBUILD, build, and install
lpm remove    <pkg...>      # remove package(s)
lpm upgrade   [pkg...]      # upgrade packages (all if none given)
lpm update                  # sync package databases
lpm search    <term>        # search available packages
lpm info      <pkg...>      # package information
lpm deps      [pkg...]      # dependency tree
lpm list                    # list installed packages
lpm owns      <path>        # which package owns a file
lpm files     <pkg>         # list files in a package
lpm orphans                 # show orphaned packages
lpm cache     [clean]       # manage build cache
lpm verify    [pkg...]      # verify installed package integrity
lpm test      <pkg...>      # run check() test suite
lpm audit                   # show audit log
lpm bootstrap -C <target>   # install base system into a target directory
lpm key       <sub>         # key management
lpm package   <sub>         # package tools (build/pack)
lpm repo      <sub>         # repository management
```

### Common options

| Flag | Description |
|------|-------------|
| `--force` | Override conflict and CriticalPkg checks |
| `--dry-run` | Simulate without making changes |
| `--debug=N` | Debug level 1–3 |

---

## PKGBUILD format

Standard bash script. Four functions, all optional except `package()`:

```bash
pkgname="htop"
pkgver="3.3.0"
pkgrel="1"
depends=("ncurses")
makedepends=()

sources=(
    "https://github.com/htop-dev/htop/releases/download/${pkgver}/htop-${pkgver}.tar.xz"
)

checksums=(
    "sha256:abc123..."
)

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

`sources` is a bash array — one entry per file to fetch.
`checksums` is parallel — `checksums[i]` describes `sources[i]`.
Each checksum entry is `"<algo>:<hex>"` (`sha512:`, `sha256:`, or `md5:`) or `"SKIP"`.

`uninstall()` may exist in the file but is never executed. Removal is handled by LPM via `files.list`.

---

## Configuration — `/etc/lpm/lpm.conf`

```ini
# Parallel build jobs.
# 0 = automatic.
JOBS = 0

# Extra flags for make.
MAKEFLAGS = -j4

# Build workspace.
BUILDDIR = /var/cache/lpm

# Per-package build logs.
LOGDIR = /var/log/lpm

# File ownership database.
FILESDIR = /var/lib/lpm/files

# Packages that require --force to remove.
CriticalPkg = musl musl-dev ld-musl gcc binutils bash coreutils
CriticalPkg = linux dinit lpm busybox

# Packages skipped during system upgrades.
IgnorePkg =
```

`CriticalPkg` lines are additive. See the shipped `lpm.conf` for the full option set.

---

## Removal

- Ordinary packages prompt once before removal.
- Packages listed in `CriticalPkg` are refused unless `--force` is given.
- Reverse dependencies block removal unless `--force` or `--recursive` (`-Rs`) is used.

---

## File layout

```
lpm/
├── include/
│   ├── lpm.h          # types, structs, prototypes
│   ├── config.h       # config API notes
│   └── llpm/          # libllpm public headers
├── src/
│   ├── main.c         # entry point, command dispatch
│   ├── build.c        # install, remove, upgrade, bootstrap
│   ├── config.c       # lpm.conf parser
│   ├── db.c           # installed DB, file ownership
│   ├── dep.c          # dependency resolution
│   ├── sync.c         # repository sync / upgrades
│   └── libllpm/       # shared library sources
├── lpm.conf           # default configuration
└── Makefile
```

---

## Paths

| Path | Purpose |
|------|---------|
| `/usr/src/lpm/pkgbuild_<name>` | PKGBUILD storage |
| `/var/cache/lpm/<name>/` | Build workspace |
| `/var/lib/lpm/db/installed` | Installed package DB |
| `/var/lib/lpm/files/<name>/files.list` | File ownership list |
| `/var/log/lpm/<name>.log` | Per-package build log |
| `/var/log/lpm/audit.log` | Security audit log |
| `/etc/lpm/lpm.conf` | Configuration |

---

## License

GPL-3.0
