# lpm

**lpm (Lotus Package Manager)** is a simple source-based package manager written in C for the Lotus Linux distribution.

It builds packages from PKGBUILD-style scripts and follows a minimal KISS philosophy: *Simple is more functional.*

---

## About

lpm was created while developing **Lotus Linux**, a small experimental Linux distribution built from scratch.

The project started as a personal learning experiment: writing a package manager in C to understand how Linux distributions manage packages, repositories, and builds.

The first versions were written by a 14-year-old developer during spare time (often after finishing advanced math homework). The goal was not to compete with existing systems, but to explore how a simple package manager could work internally.

---

## Features

* Simple PKGBUILD-style package scripts
* Source-based builds
* Minimal repository structure
* Designed for Lotus Linux

---

## Status

**Version:** v1.0.0-alpha

⚠ WARNING
This project is experimental. Interfaces, commands, and package formats may change at any time.

---

## Installation

Clone the repository:

```bash
git clone https://github.com/draconmc1337/lpm
cd lpm
```

Build:

```bash
make
```

Install (optional):

```bash
sudo make install
```

---

## Basic Usage

Example commands:

```bash
lpm -S hello ## Sync + Build + Install
lpm -U
lpm -c hello ## Test suite
```

---

## Repository Structure

Example Lotus repository layout:

```
lotus-repository/
 ├─ base/
 │   ├─ pkgbuild_gcc
 │   └─ pkgbuild_glibc
 ├─ extra/
 └─ lotus/
     └─ pkgbuild_hello
```

---

## License

Open source. See the LICENSE file for details.
