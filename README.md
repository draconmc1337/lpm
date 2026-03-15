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

Developer tools:

* `newpkg` script for generating PKGBUILD templates
* `lpm-bootstrap.sh` for bootstrapping systems after manual builds

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

Clone the repository:

git clone https://github.com/draconmc1337/lpm
cd lpm

Build:

make

Install (optional):

sudo make install

---

# Basic Usage

Install a package:

lpm -S <package>

Update installed packages:

lpm -u

Search for a package:

lpm -s <package>

Show package information:

lpm -qi <package>

Remove a package:

lpm -r <package>

Run package test suite:

lpm -c <package>

---

# Repository Structure

Example repository layout:

repo/
├─ base/
│   ├─ pkgbuild_gcc
│   └─ pkgbuild_glibc
├─ extra/
└─ lotus/
└─ pkgbuild_hello

lpm automatically searches for packages in this order:

base → extra → lotus

---

# Philosophy

lpm follows a minimal design philosophy.

The system avoids unnecessary complexity and tries to keep the package manager understandable. Package builds remain transparent and reproducible.

If something can be implemented simply, it should not be complicated.

---

# License

Open source. See the LICENSE file for details.
