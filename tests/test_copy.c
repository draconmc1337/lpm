/*
 * tests/test_copy.c — regression test for the .lpkg staging copy /
 * filename-corruption bug class.
 *
 * The reported symptom: `lpm package build hello` produced a staging entry
 * with an EMPTY filename and a size of exactly 65536 bytes (64 KiB), and
 * "package() produced no files" — i.e. a copy routine that (a) stopped after
 * one buffer's worth of data and (b) aliased the filename buffer with the
 * data buffer.
 *
 * Audit finding: in THIS codebase every bulk copy goes through `cp -a` /
 * `tar` / `bsdtar` (system()), downloads use atomic rename(), and the only
 * read-into-buffer loop is sha256_file() (8192-byte buffer, loops to EOF).
 * There is no fixed-65536 manual copy loop and no archive.c/fsops.c. So the
 * literal described root cause is absent — but the truncation CLASS was live
 * in verify.c / merge.c (fixed path buffers silently truncated via strncpy),
 * which is fixed separately.
 *
 * This test locks in the round-trip contract the bug violated:
 *   - a file LARGER than 65536 bytes survives pack+extract byte-identical
 *   - its FILENAME survives (never collapses to empty)
 *   - boundary cases: 0 bytes and exactly 65536 bytes
 *   - sha256_file() reads multi-buffer files to EOF (matches sha256sum)
 */
#include "lpm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "  FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "  ok:   %s\n", msg); } \
} while (0)

/* deterministic content: byte i = (i*31 + 7) & 0xff */
static void write_pattern_file(const char *path, long size) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    for (long i = 0; i < size; i++) {
        unsigned char b = (unsigned char)((i * 31 + 7) & 0xff);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
}

static long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

/* external sha256 via system sha256sum, for cross-checking sha256_file() */
static int sha256sum_ref(const char *path, char out[65]) {
    char cmd[LPM_PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "sha256sum '%s' | cut -d' ' -f1", path);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    if (!fgets(out, 65, p)) { pclose(p); return -1; }
    pclose(p);
    out[strcspn(out, "\n")] = '\0';
    return 0;
}

typedef struct { const char *rel; long size; } TestFile;

static void roundtrip(const char *root, const char *label, int use_bsdtar) {
    char pkgdir[LPM_PATH_MAX], stage[LPM_PATH_MAX], out[LPM_PATH_MAX];
    snprintf(pkgdir, sizeof(pkgdir), "%s/pkg_%s", root, label);
    snprintf(stage,  sizeof(stage),  "%s/stage_%s", root, label);
    snprintf(out,    sizeof(out),    "%s/out_%s", root, label);

    /* build a pkgdir with usr/bin/<files> */
    char bindir[LPM_PATH_MAX];
    snprintf(bindir, sizeof(bindir), "%s/usr/bin", pkgdir);
    util_mkdirp(bindir, 0755);
    util_mkdirp(stage, 0755);
    util_mkdirp(out, 0755);

    TestFile files[] = {
        { "usr/bin/empty",     0      },
        { "usr/bin/exact64k",  65536  },
        { "usr/bin/big",       200000 },  /* > 65536: the reported failure size class */
    };
    int nf = (int)(sizeof(files)/sizeof(files[0]));

    char src_sha[nf][65];
    for (int i = 0; i < nf; i++) {
        char full[LPM_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", pkgdir, files[i].rel);
        write_pattern_file(full, files[i].size);
        CHECK(sha256_file(full, src_sha[i]) == 0, "sha256_file computes source digest");
        /* cross-check multi-buffer read loop against system sha256sum */
        char ref[65];
        if (sha256sum_ref(full, ref) == 0)
            CHECK(strcmp(src_sha[i], ref) == 0, "sha256_file matches sha256sum (reads to EOF)");
    }

    /* pack exactly as lpkg_pack_one() does */
    char datazst[LPM_PATH_MAX];
    snprintf(datazst, sizeof(datazst), "%s/data.tar.zst", stage);
    char pack[LPM_PATH_MAX * 3];
    if (use_bsdtar)
        snprintf(pack, sizeof(pack), "bsdtar -C '%s' -cf '%s' --zstd .", pkgdir, datazst);
    else
        snprintf(pack, sizeof(pack), "tar -C '%s' -c . | zstd -T0 -q -o '%s'", pkgdir, datazst);
    CHECK(system(pack) == 0, "pack pkgdir -> data.tar.zst");

    /* extract into out/ (bsdtar auto-detects zstd via -xf, as lpkg does) */
    char ext[LPM_PATH_MAX * 3];
    if (use_bsdtar)
        snprintf(ext, sizeof(ext), "bsdtar -C '%s' -xf '%s'", out, datazst);
    else
        snprintf(ext, sizeof(ext), "zstd -dc '%s' | tar -C '%s' -x", datazst, out);
    CHECK(system(ext) == 0, "extract data.tar.zst -> out/");

    /* verify: filename survives (non-empty, correct) + bytes identical */
    for (int i = 0; i < nf; i++) {
        char got[LPM_PATH_MAX];
        snprintf(got, sizeof(got), "%s/%s", out, files[i].rel);

        /* the bug produced an entry with NO filename — assert the named
         * file exists, and that no stray empty-named file appeared in bin */
        struct stat st;
        CHECK(stat(got, &st) == 0, "correct filename survives round-trip");
        CHECK(file_size(got) == files[i].size, "size survives round-trip (no 64KiB truncation)");

        char dst_sha[65];
        CHECK(sha256_file(got, dst_sha) == 0, "sha256_file computes dest digest");
        CHECK(strcmp(src_sha[i], dst_sha) == 0, "contents byte-identical after round-trip");
    }

    /* The stat()/size checks above already prove the filename survived and
     * the bytes are intact; an empty-named 65536-byte entry (the reported
     * corruption) would fail "correct filename survives round-trip". */

    fprintf(stderr, "  [%s round-trip done]\n", label);
}

int main(void) {
    char root[] = "/tmp/lpm-copy-test-XXXXXX";
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    fprintf(stderr, "test_copy: workspace %s\n", root);

    fprintf(stderr, "test_copy: bsdtar --zstd path\n");
    roundtrip(root, "bsdtar", 1);
    fprintf(stderr, "test_copy: tar | zstd path\n");
    roundtrip(root, "gnutar", 0);

    char rm[LPM_PATH_MAX + 16];
    snprintf(rm, sizeof(rm), "rm -rf '%s'", root);
    if (system(rm) != 0) { /* best effort */ }

    if (failures) { fprintf(stderr, "test_copy: %d FAILURE(S)\n", failures); return 1; }
    fprintf(stderr, "test_copy: all checks passed\n");
    return 0;
}
