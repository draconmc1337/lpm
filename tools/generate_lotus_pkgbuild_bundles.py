import os, re, zipfile

PACKAGES = [
"musl-1.2.5","linux-api-headers-6.18","zlib-1.3.2","bzip2-1.0.8","xz-5.8.2","lz4-1.10.0","zstd-1.5.7","file-5.46","readline-8.3","pcre2-10.47","m4-1.4.21","bc-7.0.3","flex-2.6.4","tcl-8.6.17","expect-5.45.4","dejagnu-1.6.3","pkgconf-2.5.1","binutils-2.46.0","gmp-6.3.0","mpfr-4.2.2",
"mpc-1.3.1","attr-2.5.2","acl-2.3.2","libcap-2.77","shadow-4.19.3","gcc-15.2.0","ncurses-6.6","sed-4.9","psmisc-23.7","gettext-0.26","bison-3.8.2","grep-3.12","bash-5.3","libtool-2.5.4","gdbm-1.26","gperf-3.3","expat-2.7.4","inetutils-2.7","less-692","perl-5.42.0",
"xml-parser-2.47","intltool-0.51.0","autoconf-2.72","automake-1.18.1","openssl-3.6.1","elfutils-0.194","libffi-3.5.2","sqlite-3510200","python-3.14.3","flit-core-3.12.0","packaging-26.0","wheel-0.46.3","setuptools-82.0.0","ninja-1.13.2","meson-1.10.1","kmod-34.2","coreutils-9.10","diffutils-3.12","gawk-5.3.2","findutils-4.10.0",
"groff-1.23.0","gzip-1.14","iproute2-6.18.0","kbd-2.9.0","libpipeline-1.5.8","make-4.4.1","patch-2.8","tar-1.35","texinfo-7.2","vim-9.2.0078","markupsafe-3.0.3","jinja2-3.1.6","dinit-0.19.0","dbus-1.16.2","man-db-2.13.1","procps-ng-4.0.6","util-linux-2.41.3","e2fsprogs-1.47.3","busybox-1.36.1","man-pages-6.17",
"iana-etc-20260202","wpa_supplicant-2.11","networkmanager-1.52.1","connman-1.43","iw-6.17","dhcpcd-10.2.4","wget-1.25.0","curl-8.17.0","ca-certificates-20250520","links-2.31","limine-binary-9.2.0"
]

def split_pkg(s):
    m = re.match(r"^(.+)-([0-9].*)$", s)
    if m:
        return m.group(1), m.group(2)
    return s, "latest"

def normalize_name(name):
    return name.lower().replace("::", "-").replace("_", "-").replace(" ", "-")

def make_template(name, ver):
    return f'''pkgname="{name}"\npkgver="{ver}"\npkgrel="1"\ndepends=()\nmakedepends=()\nsource=()\nsha256sums=()\n\nbuild() {{\n  :\n}}\n\ncheck() {{\n  :\n}}\n\npackage() {{\n  :\n}}\n'''

os.makedirs("dist/generated-pkgbuilds-musl", exist_ok=True)
for f in os.listdir("dist"):
    if f.startswith("lotus-p") and f.endswith(".zip"):
        os.remove(os.path.join("dist", f))

chunks = [PACKAGES[i:i+20] for i in range(0, len(PACKAGES), 20)]
for idx, chunk in enumerate(chunks, start=1):
    zpath = f"dist/lotus-p{idx}-musl.zip"
    with zipfile.ZipFile(zpath, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for item in chunk:
            raw_name, ver = split_pkg(item)
            name = normalize_name(raw_name)
            fname = f"pkgbuild_{name}"
            body = make_template(name, ver)
            with open(f"dist/generated-pkgbuilds-musl/{fname}", "w") as fp:
                fp.write(body)
            z.writestr(fname, body)
print(f"generated {len(chunks)} musl bundles")
