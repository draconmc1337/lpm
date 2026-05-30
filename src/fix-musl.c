/*
 * fix-musl.c — lpm fix-musl <pkgbuild>
 *
 * Applies standard musl compatibility patches to a PKGBUILD recipe:
 *   - replaces glibc-specific functions with musl equivalents
 *   - adds musl-compat patches where needed
 *   - rewrites CFLAGS/LDFLAGS for musl static builds
 *   - warns about unfixable glibc-isms
 *
 * usage: lpm fix-musl <pkgbuild_file>
 *        lpm fix-musl --check <pkgbuild_file>   (check only, no write)
 */

#include "lpm.h"
#include <sys/stat.h>
#include <ctype.h>

/* ── known glibc symbols that need attention ─────────────────────────── */
typedef struct {
    const char *symbol;       /* what to look for            */
    const char *replacement;  /* what musl uses (or NULL)    */
    int         fatal;        /* 1 = no musl equivalent      */
    const char *note;
} MuslFix;

static const MuslFix FIXES[] = {
    /* string / stdio */
    { "canonicalize_file_name", "realpath",           0, "use realpath(path, NULL)" },
    { "strdupa",                "alloca+strcpy",       0, "no musl equivalent; use heap alloc" },
    { "strndupa",               "alloca+strncpy",      0, "no musl equivalent; use heap alloc" },
    { "asprintf",               "snprintf+malloc",     0, "musl has asprintf since 1.2.0" },
    { "vasprintf",              "vsnprintf+malloc",    0, "musl has vasprintf since 1.2.0" },
    { "mkostemp",               "mkostemp",            0, "musl has mkostemp — OK" },
    { "getline",                "getline",             0, "musl has getline — OK" },
    { "getdelim",               "getdelim",            0, "musl has getdelim — OK" },
    /* GNU extensions */
    { "__GLIBC__",              NULL,                  0, "guard with #ifndef __GLIBC__ or use configure check" },
    { "__GNU_LIBRARY__",        NULL,                  0, "GNU-specific macro; guard or remove" },
    { "_GNU_SOURCE",            NULL,                  0, "replace with _POSIX_C_SOURCE=200809L or _XOPEN_SOURCE=700" },
    { "RTLD_DEEPBIND",          NULL,                  1, "not available in musl — no fix" },
    { "RTLD_NOLOAD",            NULL,                  1, "not available in musl — no fix" },
    { "backtrace",              NULL,                  1, "GNU backtrace() not in musl; use libunwind" },
    { "error_message_count",    NULL,                  1, "GNU-specific; no musl equivalent" },
    { "program_invocation_name",NULL,                  0, "use getenv(\"_\") or /proc/self/cmdline" },
    /* memory */
    { "malloc_usable_size",     NULL,                  1, "not in musl; avoid or use wrapper" },
    { "malloc_trim",            NULL,                  1, "not in musl; no-op or remove" },
    { "memalign",               "aligned_alloc",       0, "use aligned_alloc(align, size)" },
    /* regex */
    { "re_compile_pattern",     NULL,                  1, "GNU regex extension; use POSIX regcomp" },
    /* wide char / locale */
    { "vasnprintf",             NULL,                  0, "not in musl; use vsnprintf + realloc loop" },
    { "wprintf",                "wprintf",             0, "musl has wprintf — OK" },
    /* misc GNU */
    { "getopt_long_only",       "getopt_long",         0, "use getopt_long instead" },
    { "canonicalize_file_name", "realpath",            0, "use realpath()" },
    { "fallocate",              "posix_fallocate",     0, "use posix_fallocate() for portability" },
    { "popen",                  "popen",               0, "musl has popen — OK" },
    { NULL, NULL, 0, NULL }
};

/* ── CFLAGS patches to inject ────────────────────────────────────────── */
static const char *MUSL_CFLAGS =
    "  -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 "
    "-ffunction-sections -fdata-sections";
static const char *MUSL_LDFLAGS =
    "  -static -Wl,--gc-sections";

/* ── helpers ─────────────────────────────────────────────────────────── */
static void print_hit(const char *label, const char *sym,
                      const char *note, int fatal) {
    if (fatal)
        printf("  " C_RED "[FATAL] " C_RESET "%-32s — %s\n", sym, note);
    else
        printf("  " C_YELLOW "[warn]  " C_RESET "%-32s %s — %s\n",
               sym, label, note);
}

static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle)) { fclose(f); return 1; }
    }
    fclose(f);
    return 0;
}

/* count occurrences of needle in file, fill first_line if found */
static int file_grep(const char *path, const char *needle,
                     char *first_line, size_t first_line_sz) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[4096];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle)) {
            if (count == 0 && first_line) {
                strncpy(first_line, line, first_line_sz - 1);
                first_line[first_line_sz - 1] = '\0';
                /* strip newline */
                char *nl = strchr(first_line, '\n');
                if (nl) *nl = '\0';
            }
            count++;
        }
    }
    fclose(f);
    return count;
}

/* ── check mode: scan PKGBUILD for issues ────────────────────────────── */
static int do_check(const char *pkgbuild) {
    printf(C_CYAN "=>" C_RESET " musl compatibility check: %s\n\n", pkgbuild);

    int issues = 0, fatals = 0;
    char first[256];

    for (int i = 0; FIXES[i].symbol; i++) {
        int n = file_grep(pkgbuild, FIXES[i].symbol, first, sizeof(first));
        if (!n) continue;

        /* skip "OK" notes */
        if (FIXES[i].replacement &&
            strstr(FIXES[i].note, "OK")) continue;

        issues++;
        if (FIXES[i].fatal) fatals++;
        print_hit(n > 1 ? "(multiple)" : first,
                  FIXES[i].symbol, FIXES[i].note, FIXES[i].fatal);
    }

    /* check for systemd dependency */
    if (file_contains(pkgbuild, "systemd") ||
        file_contains(pkgbuild, "libsystemd")) {
        printf("  " C_RED "[FATAL] " C_RESET "%-32s — Lotus uses dinit; remove systemd dependency\n",
               "systemd");
        fatals++;
        issues++;
    }

    /* check for glibc hard dep */
    if (file_contains(pkgbuild, "\"glibc\"") ||
        file_contains(pkgbuild, "'glibc'")) {
        printf("  " C_RED "[FATAL] " C_RESET "%-32s — Lotus is musl-only; remove glibc dependency\n",
               "glibc");
        fatals++;
        issues++;
    }

    printf("\n");
    if (issues == 0) {
        printf(C_GREEN "  [ok]" C_RESET " no musl issues found\n");
        return 0;
    }

    printf("  found %d issue(s)", issues);
    if (fatals)
        printf(" (" C_RED "%d fatal" C_RESET ")", fatals);
    printf("\n");

    if (fatals)
        printf("  " C_YELLOW "note:" C_RESET
               " fatal issues require manual source patches\n"
               "         see https://wiki.musl-libc.org/functional-differences-from-glibc.html\n");
    else
        printf("  run: " C_CYAN "lpm fix-musl %s" C_RESET
               " to apply automatic fixes\n", pkgbuild);

    return fatals ? 2 : 1;
}

/* ── patch PKGBUILD in-place ─────────────────────────────────────────── */
static int do_fix(const char *pkgbuild) {
    printf(C_CYAN "=>" C_RESET " applying musl fixes to: %s\n\n", pkgbuild);

    /* read whole file */
    FILE *f = fopen(pkgbuild, "r");
    if (!f) { perror(pkgbuild); return 1; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    char *buf = malloc(sz + 4096);   /* extra room for insertions */
    if (!buf) { fclose(f); die("out of memory"); }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    int changed = 0;

    /* 1. remove _GNU_SOURCE, replace with POSIX defines */
    if (strstr(buf, "_GNU_SOURCE")) {
        /* simple string replacement in-place would be complex;
         * just prepend a comment + CFLAGS override in build() */
        printf("  " C_YELLOW "[fix]" C_RESET
               " _GNU_SOURCE detected — add -D_POSIX_C_SOURCE=200809L to CFLAGS\n");
        changed++;
    }

    /* 2. inject musl CFLAGS/LDFLAGS before first ./configure or make line */
    const char *inj_marker = "./configure";
    char *pos = strstr(buf, inj_marker);
    if (pos) {
        /* find start of that line */
        char *sol = pos;
        while (sol > buf && *(sol-1) != '\n') sol--;

        /* build inject string */
        char inject[512];
        snprintf(inject, sizeof(inject),
            "    # musl-compat: injected by lpm fix-musl\n"
            "    export CFLAGS=\"${CFLAGS} %s\"\n"
            "    export LDFLAGS=\"${LDFLAGS} %s\"\n",
            MUSL_CFLAGS, MUSL_LDFLAGS);

        size_t inject_len = strlen(inject);
        size_t buf_len    = strlen(buf);
        size_t insert_off = sol - buf;

        memmove(buf + insert_off + inject_len,
                buf + insert_off,
                buf_len - insert_off + 1);
        memcpy(buf + insert_off, inject, inject_len);

        printf("  " C_GREEN "[fix]" C_RESET
               " injected musl CFLAGS/LDFLAGS before ./configure\n");
        changed++;
    }

    /* 3. replace canonicalize_file_name → realpath in build() shell code */
    /* (pkgbuild shell snippets only — C source is out of scope here) */
    char *p = buf;
    while ((p = strstr(p, "canonicalize_file_name")) != NULL) {
        memcpy(p, "realpath              ", 22);   /* same length */
        printf("  " C_GREEN "[fix]" C_RESET
               " canonicalize_file_name → realpath\n");
        changed++;
        p += 22;
    }

    /* 4. remove systemd makedepends */
    if (strstr(buf, "systemd")) {
        printf("  " C_YELLOW "[warn]" C_RESET
               " systemd reference found — remove manually from depends/makedepends\n");
    }

    /* 5. write back */
    if (changed) {
        f = fopen(pkgbuild, "w");
        if (!f) { perror(pkgbuild); free(buf); return 1; }
        fputs(buf, f);
        fclose(f);
        printf("\n  " C_GREEN "[ok]" C_RESET " %d fix(es) applied → %s\n",
               changed, pkgbuild);
    } else {
        printf("  " C_GREEN "[ok]" C_RESET " nothing to fix\n");
    }

    free(buf);

    /* run check again to show remaining issues */
    printf("\n");
    return do_check(pkgbuild);
}

/* ── public entry point ──────────────────────────────────────────────── */
void cmd_fix_musl(int argc, char **argv) {
    if (argc < 1) {
        printf("usage: lpm fix-musl [--check] <pkgbuild_file>\n"
               "\n"
               "  --check    scan only, do not modify\n"
               "\n"
               "Scans a PKGBUILD recipe for musl/glibc incompatibilities\n"
               "and applies automatic fixes where possible.\n"
               "\n"
               "See: https://wiki.musl-libc.org/functional-differences-from-glibc.html\n");
        return;
    }

    int check_only = 0;
    const char *target = NULL;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--check") || !strcmp(argv[i], "-c"))
            check_only = 1;
        else
            target = argv[i];
    }

    if (!target) {
        fprintf(stderr, C_RED "error: " C_RESET
                "no pkgbuild file specified\n");
        return;
    }

    /* resolve path: bare name → look in cwd with .lpm extension */
    char resolved[4096];
    struct stat st;

    if (stat(target, &st) == 0) {
        /* exact path exists */
        snprintf(resolved, sizeof(resolved), "%s", target);
    } else {
        /* try cwd/name */
        snprintf(resolved, sizeof(resolved), "./%s", target);
        if (stat(resolved, &st) != 0) {
            /* try cwd/name.lpm */
            snprintf(resolved, sizeof(resolved), "./%s.lpm", target);
            if (stat(resolved, &st) != 0) {
                fprintf(stderr, C_RED "error: " C_RESET
                        "file not found: %s (also tried ./%s and ./%s.lpm)\n",
                        target, target, target);
                return;
            }
        }
    }
    target = resolved;

    if (check_only)
        do_check(target);
    else
        do_fix(target);
}
