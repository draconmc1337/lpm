# lpm – Lazy Package Manager

lpm is a lightweight source-based package manager written in C.

It was originally created as a learning project to understand how Linux distributions manage packages, repositories, and source builds. Over time it evolved into a small but capable package manager designed for simplicity, transparency, and control.

The project follows the **SIMF principle — Simple Is More Functional**.

Instead of implementing a complex ecosystem, lpm focuses on doing a few core tasks well:

* fetching package sources
* resolving dependencies
* building software from source
* installing files into the system

Everything is intentionally simple and visible. Packages are defined using small PKGBUILD-style scripts so users can clearly see how software is built.

---

# Features

Core capabilities included in **v1.1.0-alpha**:

* Source-based package builds
* PKGBUILD-style package scripts
* Automatic dependency resolver
* Topological build order (dependencies first)
* Dependency tree preview before installation
* SHA256 source integrity verification
* Multi-source package support

Reliability features:

* Atomic downloads using `.part` staging files
* Partial download detection and automatic re-fetch
* Corrupt archive detection (`tar -tf` validation)
* Critical package blacklist (prevents removal of core packages)
* Confirmation prompts for destructive operations


Repository integration:

* Automatic PKGBUILD lookup across repositories
* Repository search support
* Package information inspection

---

# Status

Version: **v1.1.0-alpha**

⚠ **WARNING**

lpm is experimental software.

Commands, repository layout, package formats, and internal behavior may change between versions.

Use at your own risk.

---

# Installation

Clone and build **lpm**:

```bash
git clone https://github.com/draconmc1337/lpm
cd lpm
make
```

Install:

```bash
doas make install ## LOtus Linux uses doas as 
```

---

# Basic Usage

Common commands:

```bash
lpm -S <package>    # install package
lpm -u              # update installed packages
lpm -s <package>    # search repository
lpm -qi <package>   # show package info
lpm -r <package>    # remove package
lpm -c <package>    # run package test suite
```

---

# Repository Structure

Example Lotus repository layout:

```text
repo/
├── base/
│   ├── pkgbuild_gcc
│   └── pkgbuild_glibc
├── extra/
└── lotus/
    └── pkgbuild_hello
```

Package lookup order:

```
base → extra → lotus
```


# Philosophy

lpm follows a minimal design philosophy.

The system avoids unnecessary complexity and tries to keep the package manager understandable. Package builds remain transparent and reproducible.

If something can be implemented simply, it should not be complicated.

---

# License

Open source. See the LICENSE file for details.
