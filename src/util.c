#include "lpm.h"
#include <signal.h>
#include <time.h>

void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, C_RED "error: " C_RESET);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

void warn(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, C_YELLOW "warning: " C_RESET);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

void lpm_log(const char *fmt, ...) {
    FILE *f = fopen(LPM_LOG, "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    fprintf(f, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/*
 * lpm_audit — like lpm_log but writes to LPM_AUDIT_LOG.
 * Used for security-sensitive actions: force-remove, --no-confirm overrides,
 * critical package bypass attempts. Kept separate so audit trail is not
 * mixed with normal build/install noise.
 */
void lpm_audit(const char *fmt, ...) {
    FILE *f = fopen(LPM_AUDIT_LOG, "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    /* log uid so we know who ran with sudo */
    fprintf(f, "[%s] uid=%d ", ts, (int)getuid());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

int confirm(const char *prompt) {
    printf("%s", prompt);
    fflush(stdout);
    char buf[16];
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    buf[strcspn(buf, "\n")] = '\0';
    /* accept: y Y yes Yes YES — reject everything else */
    return (strcasecmp(buf, "y")   == 0 ||
            strcasecmp(buf, "yes") == 0);
}

/*
 * confirm_word — require the user to type an exact word (case-sensitive).
 * Used for destructive operations like force-removing critical packages.
 */
int confirm_word(const char *prompt, const char *word) {
    printf("%s", prompt);
    fflush(stdout);
    char buf[64];
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    buf[strcspn(buf, "\n")] = '\0';
    return (strcmp(buf, word) == 0);
}

/* pkg_log_path — fills out with LPM_LOG_DIR/<pkgname>.log */
void pkg_log_path(const char *pkgname, char *out, size_t outsz) {
    snprintf(out, outsz, "%s/%s.log", LPM_LOG_DIR, pkgname);
}

/*
 * lpm_parse_flags — scan argv for known flags, populate LpmFlags,
 * write non-flag arguments (package names) into pkgs[].
 * Returns number of package names found.
 */
int lpm_parse_flags(int argc, char **argv, LpmFlags *f, char **pkgs, int maxpkgs) {
    memset(f, 0, sizeof(*f));
    int n = 0;
    for (int i = 0; i < argc; i++) {
        if      (strcmp(argv[i], "--yes")        == 0) f->yes        = 1;
        else if (strcmp(argv[i], "--strict")     == 0) f->strict     = 1;
        else if (strcmp(argv[i], "--force")      == 0) f->force      = 1;
        else if (strcmp(argv[i], "--no-confirm") == 0) f->no_confirm = 1;
        else if (n < maxpkgs) pkgs[n++] = argv[i];
    }
    return n;
}

void init_dirs(void) {
    mkdir(LPM_BUILD_DIR, 0755);
    mkdir("/var/lib/lpm", 0755);
    mkdir(LPM_FILES_DIR, 0755);   /* per-package file ownership lists */
    mkdir(LPM_LOG_DIR,   0755);   /* per-package build logs */
    /* touch global log files */
    FILE *f;
    f = fopen(LPM_DB,        "a"); if (f) fclose(f);
    f = fopen(LPM_LOG,       "a"); if (f) fclose(f);
    f = fopen(LPM_AUDIT_LOG, "a"); if (f) fclose(f);
}

void check_root(void) {
    if (geteuid() != 0)
        die("you cannot perform this operation unless you are root.\n"
            C_YELLOW "hint: " C_RESET "use sudo or doas");
}

/* compare version strings using sort -V logic */
int version_gte(const char *have, const char *need) {
    /* popen "printf '%s\n%s\n' need have | sort -V | head -1" */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "printf '%%s\\n%%s\\n' '%s' '%s' | sort -V | head -1", need, have);
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    char out[MAX_STR];
    out[0] = '\0';
    fgets(out, sizeof(out), p);
    pclose(p);
    /* strip newline */
    out[strcspn(out, "\n")] = '\0';
    return (strcmp(out, need) == 0);
}
