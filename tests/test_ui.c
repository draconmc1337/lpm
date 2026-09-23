/*
 * tests/test_ui.c — proves the shared atomic writer (ui.c) does not
 * interleave output from concurrent threads.
 *
 * The UX contract requires that with MAX_DL_THREADS > 1, each job's
 * "(N/M) Installing/Building" line and each debug line appear as ONE atomic
 * block — never character-by-character mashed across workers. Both go through
 * the same writer (ui_out / ui_dbg→ui_err), so one test covers both.
 *
 * Strategy: N threads each emit the same number of long, uniquely-prefixed
 * lines. If the writer were not atomic, output lines would be corrupted
 * (a line would not start with exactly one thread's prefix). We count
 * well-formed complete lines in the captured buffer and assert none are lost
 * or merged.
 */
#include "lpm.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NTHREAD 8
#define NLINES  200

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "  FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "  ok:   %s\n", msg); } \
} while (0)

static void *worker(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < NLINES; i++)
        ui_out("thread%ld-line%06d-payload-payload-payload-payload\n", id, i);
    return NULL;
}

int main(void) {
    /* redirect stdout to a temp file we can inspect */
    char path[] = "/tmp/lpm-ui-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 2; }
    fflush(stdout);
    if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2"); return 2; }
    close(fd);

    pthread_t th[NTHREAD];
    for (long i = 0; i < NTHREAD; i++)
        pthread_create(&th[i], NULL, worker, (void *)i);
    for (int i = 0; i < NTHREAD; i++)
        pthread_join(th[i], NULL);

    fflush(stdout);

    /* inspect the captured file */
    FILE *f = fopen(path, "r");
    if (!f) { perror("open captured"); return 2; }
    char line[512];
    long total = 0, wellformed = 0;
    while (fgets(line, sizeof(line), f)) {
        total++;
        /* a well-formed line: "threadK-lineNNNNNN-payload...\n" exactly */
        if (strncmp(line, "thread", 6) != 0) continue;
        char *dash = strchr(line, '-');
        if (!dash) continue;
        if (strncmp(dash, "-line", 5) != 0) continue;       /* corruption: text before -line */
        /* payload must not itself contain another "thread" (would mean a merge) */
        if (strstr(dash, "thread")) continue;
        size_t len = strlen(line);
        if (len == 0 || line[len-1] != '\n') continue;       /* truncated/merged */
        wellformed++;
    }
    fclose(f);
    unlink(path);

    fprintf(stderr, "  (captured %ld lines, %ld well-formed)\n", total, wellformed);
    CHECK(total == (long)NTHREAD * NLINES, "no lines lost");
    CHECK(wellformed == (long)NTHREAD * NLINES, "no lines interleaved/corrupted");

    if (failures) { fprintf(stderr, "test_ui: %d FAILURE(S)\n", failures); return 1; }
    fprintf(stderr, "test_ui: all checks passed\n");
    return 0;
}
