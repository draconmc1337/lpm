#include "lpm.h"
#include <pthread.h>
#include <sys/ioctl.h>

/* ── detect downloader ───────────────────────────────────────────────── */
Downloader dl_detect(const char *override) {
    if (override && strcmp(override, "auto") != 0) {
        if (!strcmp(override, "wget")) return DL_WGET;
        if (!strcmp(override, "curl")) return DL_CURL;
    }
    if (system("command -v wget >/dev/null 2>&1") == 0) return DL_WGET;
    if (system("command -v curl >/dev/null 2>&1") == 0) return DL_CURL;
    return DL_NONE;
}

/* ── progress bar state ──────────────────────────────────────────────── */
typedef struct {
    char     name[64];      /* "pkgname-ver" */
    char     filename[128]; /* source filename */
    long     total;         /* bytes total, -1 if unknown */
    long     done;          /* bytes downloaded */
    int      finished;      /* 1 = done, -1 = failed */
} SlotState;

#define MAX_SLOTS 16
static SlotState    g_slots[MAX_SLOTS];
static int          g_nslots  = 0;
static pthread_mutex_t g_mtx  = PTHREAD_MUTEX_INITIALIZER;

/* ── draw all progress bars (called from main thread) ────────────────── */
static void draw_bars(void) {
    pthread_mutex_lock(&g_mtx);

    /* move cursor up to overwrite previous output */
    static int first = 1;
    if (!first) {
        /* move up g_nslots lines */
        printf("\033[%dA", g_nslots);
    }
    first = 0;

    for (int i = 0; i < g_nslots; i++) {
        SlotState *s = &g_slots[i];

        /* label: "pkgname-ver" padded to 22 chars */
        printf("  %-22s ", s->name);

        if (s->finished == -1) {
            printf(C_RED "[FAILED]" C_RESET "                              \n");
            continue;
        }
        if (s->finished == 1) {
            printf(C_GREEN "[done]  " C_RESET "                              \n");
            continue;
        }

        /* bar: 30 chars wide */
        int pct = 0;
        if (s->total > 0)
            pct = (int)((s->done * 100) / s->total);
        int filled = (pct * 30) / 100;

        printf("[");
        for (int j = 0; j < 30; j++) {
            if (j < filled)        printf(C_CYAN "#" C_RESET);
            else if (j == filled)  printf(">");
            else                   printf(" ");
        }

        if (s->total > 0)
            printf("] %3d%%  ", pct);
        else
            printf("] ???%%  ");

        /* size */
        if (s->done < 1024*1024)
            printf("%ldK   \n", s->done/1024);
        else
            printf("%.1fM  \n", s->done/1024.0/1024.0);
    }
    fflush(stdout);
    pthread_mutex_unlock(&g_mtx);
}

/* ── worker: download one file, update slot ──────────────────────────── */
typedef struct {
    int       slot;
    FetchJob *job;
} WorkerArg;

static void *dl_worker(void *arg) {
    WorkerArg *wa  = (WorkerArg *)arg;
    FetchJob  *job = wa->job;
    int        sl  = wa->slot;
    free(wa);

    Downloader dl = dl_detect(g_cfg.downloader);
    if (dl == DL_NONE) {
        pthread_mutex_lock(&g_mtx);
        g_slots[sl].finished = -1;
        pthread_mutex_unlock(&g_mtx);
        job->result = -1;
        return NULL;
    }

    /* use a temp file */
    char part[LPM_PATH_MAX];
    snprintf(part, sizeof(part), "%s.part", job->dest);
    remove(part);

    /* build command that prints progress to a temp file we poll */
    char prog_file[LPM_PATH_MAX];
    snprintf(prog_file, sizeof(prog_file), "/tmp/lpm_dl_%d.prog", sl);

    char cmd[LPM_URL_MAX + LPM_PATH_MAX + 256];
    if (dl == DL_WGET) {
        /* wget writes progress to stderr; redirect to prog_file */
        snprintf(cmd, sizeof(cmd),
            "wget -q --show-progress --progress=dot:mega "
            "--timeout=30 --tries=3 -O '%s' '%s' 2>'%s'",
            part, job->url, prog_file);
    } else {
        /* curl: write progress to prog_file */
        snprintf(cmd, sizeof(cmd),
            "curl -L --retry 3 --connect-timeout 30 "
            "--progress-bar -o '%s' '%s' 2>'%s'",
            part, job->url, prog_file);
    }

    /* spawn download in background via fork+system trick:
       we want to poll progress while it runs */
    int rc = system(cmd);

    struct stat st;
    int ok = (rc == 0 && stat(part, &st) == 0 && st.st_size > 0);

    if (ok) rename(part, job->dest);
    else    remove(part);

    /* verify checksum */
    if (ok && job->cksum_type != CKSUM_SKIP && job->checksum[0])
        ok = (cksum_verify(job->dest, job->checksum, job->cksum_type) == 0);

    remove(prog_file);

    pthread_mutex_lock(&g_mtx);
    if (ok) {
        g_slots[sl].finished = 1;
        /* fill bar to 100% */
        if (g_slots[sl].total > 0)
            g_slots[sl].done = g_slots[sl].total;
        else {
            struct stat fs;
            if (stat(job->dest, &fs) == 0)
                g_slots[sl].done = g_slots[sl].total = fs.st_size;
        }
    } else {
        g_slots[sl].finished = -1;
    }
    pthread_mutex_unlock(&g_mtx);

    job->result = ok ? 0 : -1;
    return NULL;
}

/* ── size probe: get Content-Length before download ─────────────────── */
static long probe_size(const char *url) {
    char cmd[LPM_URL_MAX + 64];
    snprintf(cmd, sizeof(cmd),
        "curl -sI --connect-timeout 5 '%s' 2>/dev/null"
        " | grep -i content-length | tail -1 | awk '{print $2}'",
        url);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    long sz = -1;
    fscanf(p, "%ld", &sz);
    pclose(p);
    return sz > 0 ? sz : -1;
}

/* ── dl_file (single, used by old code paths) ────────────────────────── */
int dl_file(const char *url, const char *dest,
            const char *filename, int slot, int total) {
    (void)slot; (void)total;
    FetchJob job = {0};
    strncpy(job.url,      url,      LPM_URL_MAX-1);
    strncpy(job.dest,     dest,     LPM_PATH_MAX-1);
    strncpy(job.filename, filename, LPM_NAME_MAX-1);
    job.cksum_type = CKSUM_SKIP;
    return dl_fetch_all(&job, 1);
}

/* ── dl_fetch_all: main entry — parallel download with live bars ─────── */
int dl_fetch_all(FetchJob *jobs, int njobs) {
    if (njobs == 0) return 0;

    /* init slot state */
    g_nslots = njobs < MAX_SLOTS ? njobs : MAX_SLOTS;
    memset(g_slots, 0, sizeof(g_slots));

    printf(C_PINK "::" C_RESET " Downloading sources...\n");

    for (int i = 0; i < g_nslots; i++) {
        /* label = basename of URL stripped of extension clutter */
        char *base = strrchr(jobs[i].url, '/');
        strncpy(g_slots[i].filename, base ? base+1 : jobs[i].url, 127);
        strncpy(g_slots[i].name,     jobs[i].filename[0]
                                     ? jobs[i].filename
                                     : (base ? base+1 : "?"), 63);
        g_slots[i].total    = -1;
        g_slots[i].done     = 0;
        g_slots[i].finished = 0;
    }

    /* probe sizes in background (best-effort) */
    for (int i = 0; i < g_nslots; i++) {
        long sz = probe_size(jobs[i].url);
        pthread_mutex_lock(&g_mtx);
        g_slots[i].total = sz;
        pthread_mutex_unlock(&g_mtx);
    }

    /* print initial bars */
    for (int i = 0; i < g_nslots; i++) printf("\n");
    draw_bars();

    /* spawn workers */
    int max_t = g_cfg.parallel_dl ? g_cfg.max_dl_threads : 1;
    if (max_t < 1) max_t = 1;
    if (max_t > MAX_SLOTS) max_t = MAX_SLOTS;

    pthread_t threads[MAX_SLOTS];
    int       base = 0;

    while (base < g_nslots) {
        int batch = g_nslots - base;
        if (batch > max_t) batch = max_t;

        for (int i = 0; i < batch; i++) {
            WorkerArg *wa = malloc(sizeof(WorkerArg));
            wa->slot = base + i;
            wa->job  = &jobs[base + i];
            pthread_create(&threads[i], NULL, dl_worker, wa);
        }

        /* poll + redraw while threads run */
        int all_done = 0;
        while (!all_done) {
            usleep(200000); /* 200ms */

            /* update progress from dest file sizes (rough) */
            pthread_mutex_lock(&g_mtx);
            all_done = 1;
            for (int i = 0; i < batch; i++) {
                int sl = base + i;
                if (g_slots[sl].finished == 0) {
                    all_done = 0;
                    /* estimate progress by checking .part file size */
                    char part[LPM_PATH_MAX];
                    snprintf(part, sizeof(part), "%s.part", jobs[sl].dest);
                    struct stat st;
                    if (stat(part, &st) == 0 && st.st_size > 0)
                        g_slots[sl].done = st.st_size;
                }
            }
            pthread_mutex_unlock(&g_mtx);

            draw_bars();
        }

        for (int i = 0; i < batch; i++)
            pthread_join(threads[i], NULL);

        base += batch;
    }

    /* final redraw */
    draw_bars();
    printf("\n");

    /* count failures */
    int failed = 0;
    for (int i = 0; i < g_nslots; i++)
        if (jobs[i].result != 0) failed++;

    if (failed > 0) {
        fprintf(stderr, C_RED "error:" C_RESET
            " %d source(s) failed to download or verify\n", failed);
        return -1;
    }
    return 0;
}
