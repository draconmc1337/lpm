#include "lpm.h"
#include <stdarg.h>
#include <ctype.h>
#include <sys/statvfs.h>
#include <time.h>

/* ── die / warn ──────────────────────────────────────────────────────── */
void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, C_RED "error: " C_RESET);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

void warn(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, C_YELLOW "warning: " C_RESET);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
}

/* ── lpm_log / lpm_audit ─────────────────────────────────────────────── */
void lpm_log(const char *fmt, ...) {
    FILE *f = fopen(LPM_LOG_FILE, "a");
    if (!f) return;
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(f, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

void lpm_audit(const char *fmt, ...) {
    FILE *f = fopen(LPM_AUDIT_LOG, "a");
    if (!f) return;
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(f, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* ── confirm ─────────────────────────────────────────────────────────── */
int confirm(const char *prompt) {
    printf("%s", prompt);
    fflush(stdout);
    char buf[8];
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    return (buf[0] == 'y' || buf[0] == 'Y');
}

int confirm_word(const char *prompt, const char *word) {
    printf("%s", prompt);
    fflush(stdout);
    char buf[128];
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    buf[strcspn(buf, "\n")] = '\0';
    return (strcmp(buf, word) == 0);
}

/* ── init_dirs ───────────────────────────────────────────────────────── */
void init_dirs(void) {
    util_mkdirp(LPM_DB_DIR,    0755);
    util_mkdirp(LPM_FILES_DIR, 0755);
    util_mkdirp(LPM_BUILD_DIR, 0755);
    util_mkdirp(LPM_LOG_DIR,   0755);
    /* ensure DB file exists */
    FILE *f = fopen(LPM_DB, "a");
    if (f) fclose(f);
}

/* ── check_root ──────────────────────────────────────────────────────── */
void check_root(void) {
    if (geteuid() != 0)
        die("lpm must be run as root");
}

/* ── version_compare: chuẩn, có epoch support ───────────────────────── *
 * Format: [epoch:]version[-release]                                      *
 * Returns: -1 if a < b, 0 if a == b, 1 if a > b                        *
 * So sánh từng segment số/chữ xen kẽ như RPM/pacman rpmvercmp           *
 * ──────────────────────────────────────────────────────────────────── */

/* compare one chunk: numeric if both digits, else lexicographic */
static int cmp_chunk(const char *a, const char *b) {
    int a_digit = isdigit((unsigned char)*a);
    int b_digit = isdigit((unsigned char)*b);

    if (a_digit && b_digit) {
        /* skip leading zeros for numeric compare */
        while (*a == '0') a++;
        while (*b == '0') b++;
        size_t la = 0, lb = 0;
        while (isdigit((unsigned char)a[la])) la++;
        while (isdigit((unsigned char)b[lb])) lb++;
        if (la != lb) return la < lb ? -1 : 1;
        int r = strncmp(a, b, la);
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    /* alpha chunk */
    while (*a && *b && *a == *b &&
           !isdigit((unsigned char)*a) && *a != '.' && *a != '-') {
        a++; b++;
    }
    if (*a == *b) return 0;
    if (!*a || isdigit((unsigned char)*a) || *a == '.' || *a == '-') return -1;
    if (!*b || isdigit((unsigned char)*b) || *b == '.' || *b == '-') return 1;
    return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
}

int version_compare(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";

    /* extract epoch (everything before first ':') */
    long ea = 0, eb = 0;
    const char *ca = strchr(a, ':');
    const char *cb = strchr(b, ':');
    if (ca) { ea = strtol(a, NULL, 10); a = ca + 1; }
    if (cb) { eb = strtol(b, NULL, 10); b = cb + 1; }
    if (ea != eb) return ea < eb ? -1 : 1;

    /* compare version + release together, split on . and - */
    while (*a || *b) {
        /* skip separators */
        while (*a && (*a == '.' || *a == '-')) a++;
        while (*b && (*b == '.' || *b == '-')) b++;
        if (!*a && !*b) return 0;
        if (!*a) return -1;
        if (!*b) return 1;
        int r = cmp_chunk(a, b);
        if (r) return r;
        /* advance past current chunk */
        while (*a && isdigit((unsigned char)*a)) a++;
        while (*b && isdigit((unsigned char)*b)) b++;
        while (*a && isalpha((unsigned char)*a)) a++;
        while (*b && isalpha((unsigned char)*b)) b++;
    }
    return 0;
}

/* kept for compat — wraps version_compare */
int version_gte(const char *have, const char *need) {
    return version_compare(have, need) >= 0;
}

/* ── util_nproc ──────────────────────────────────────────────────────── */
int util_nproc(void) {
    FILE *f = popen("nproc 2>/dev/null", "r");
    if (f) {
        int n = 0;
        if (fscanf(f, "%d", &n) == 1 && n > 0) { pclose(f); return n; }
        pclose(f);
    }
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        int count = 0; char line[128];
        while (fgets(line, sizeof(line), f))
            if (strncmp(line, "processor", 9) == 0) count++;
        fclose(f);
        if (count > 0) return count;
    }
    return 1;
}

/* ── util_disk_free (KB) ─────────────────────────────────────────────── */
long util_disk_free(const char *path) {
    struct statvfs st;
    if (statvfs(path, &st) != 0) return -1;
    return (long)((unsigned long long)st.f_bfree * st.f_bsize / 1024);
}

/* ── util_mkdirp ─────────────────────────────────────────────────────── */
int util_mkdirp(const char *path, mode_t mode) {
    char tmp[LPM_PATH_MAX];
    strncpy(tmp, path, sizeof(tmp)-1);
    size_t len = strlen(tmp);
    if (len && tmp[len-1] == '/') tmp[len-1] = '\0';
    for (char *p = tmp+1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* ── util_rmrf ───────────────────────────────────────────────────────── */
int util_rmrf(const char *path) {
    char cmd[LPM_PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    return system(cmd);
}

/* ── util_copy_file ──────────────────────────────────────────────────── */
int util_copy_file(const char *src, const char *dst) {
    char cmd[LPM_PATH_MAX * 2 + 16];
    snprintf(cmd, sizeof(cmd), "cp -a '%s' '%s'", src, dst);
    return system(cmd);
}

/* ── util_strip ──────────────────────────────────────────────────────── */
char *util_strip(char *s) {
    if (!s) return NULL;
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

/* ── util_run ────────────────────────────────────────────────────────── */
int util_run(const char *cmd) {
    if (g_verbose) printf(C_GRAY "  $ %s\n" C_RESET, cmd);
    int ret = system(cmd);
    if (ret == -1) return -1;
    return WEXITSTATUS(ret);
}

int util_run_env(const char *cmd, char *const envp[]) {
    (void)envp; /* simplified: just run via shell */
    return util_run(cmd);
}

/* ── util_progress_bar ───────────────────────────────────────────────── */
void util_progress_bar(int slot, int total, const char *name,
                       int percent, int done, int failed) {
    printf("  " C_GRAY "[%d/%d]" C_RESET " %-38s ", slot, total, name);
    if (done)        printf(C_GREEN  "done"   C_RESET "\n");
    else if (failed) printf(C_RED    "FAILED" C_RESET "\n");
    else             printf(C_YELLOW "%d%%"   C_RESET "\r", percent);
    fflush(stdout);
}
