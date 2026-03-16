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

int confirm(const char *prompt) {
    printf("%s", prompt);
    fflush(stdout);
    char buf[16];
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    buf[strcspn(buf, "\n")] = '\0';
    /* accept: y Y yes YES Yes */
    return (strcasecmp(buf, "y")   == 0 ||
            strcasecmp(buf, "yes") == 0);
}

void init_dirs(void) {
    mkdir(LPM_BUILD_DIR, 0755);
    mkdir("/var/lib/lpm", 0755);
    mkdir(LPM_FILES_DIR, 0755);
    /* touch DB and log */
    FILE *f;
    f = fopen(LPM_DB,  "a"); if (f) fclose(f);
    f = fopen(LPM_LOG, "a"); if (f) fclose(f);
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
