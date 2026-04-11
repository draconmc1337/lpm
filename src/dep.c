#include "lpm.h"

#define MAX_QUEUE 256

/* repo folders to search — same order as cmd_sync */
#define REPO_BASE "https://raw.githubusercontent.com/draconmc1337/lotus-repository/main"

typedef struct {
    char name[MAX_STR];
    char ver[MAX_STR];
    char folder[16];   /* base / extra / lotus / ? */
    int  installed;
    int  has_src;
    int  depth;
} DepNode;

static DepNode resolved[MAX_QUEUE];
static int     nresolved = 0;
static int     build_order[MAX_QUEUE];
static int     nbuild = 0;
static int     visited[MAX_QUEUE];
static int     in_stack[MAX_QUEUE];

static int already_seen(const char *name) {
    for (int i = 0; i < nresolved; i++)
        if (strcmp(resolved[i].name, name) == 0) return 1;
    return 0;
}

static int index_of(const char *name) {
    for (int i = 0; i < nresolved; i++)
        if (strcmp(resolved[i].name, name) == 0) return i;
    return -1;
}

static void dep_name_only(const char *spec, char *out) {
    strncpy(out, spec, MAX_STR - 1);
    out[MAX_STR - 1] = '\0';
    char *op = strpbrk(out, "><= ");
    if (op) *op = '\0';
}


static void collect(const char *pkgname, int depth) {
    if (already_seen(pkgname)) return;
    if (nresolved >= MAX_QUEUE) return;

    DepNode node;
    memset(&node, 0, sizeof(node));
    strncpy(node.name, pkgname, MAX_STR - 1);
    node.depth     = depth;
    node.installed = db_is_installed(pkgname);
    strncpy(node.folder, "?", sizeof(node.folder) - 1);

    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s",
             LPM_PKGBUILD_DIR, pkgname);

    struct stat st;
    node.has_src = (stat(pbfile, &st) == 0);

    if (node.has_src) {
        Pkg pkg;
        pkgbuild_parse(pbfile, &pkg);
        snprintf(node.ver, MAX_STR, "%s", pkg.pkgver);
        resolved[nresolved++] = node;
        for (int i = 0; i < pkg.ndepends; i++) {
            char depname[MAX_STR];
            dep_name_only(pkg.depends[i], depname);
            collect(depname, depth + 1);
        }
    } else {
        strncpy(node.ver, "?", MAX_STR - 1);
        resolved[nresolved++] = node;
    }
}

static void topo_visit(int idx) {
    if (visited[idx]) return;
    if (in_stack[idx]) {
        warn("circular dependency: %s", resolved[idx].name);
        return;
    }
    in_stack[idx] = 1;
    if (resolved[idx].has_src) {
        char pbfile[MAX_STR];
        snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s",
                 LPM_PKGBUILD_DIR, resolved[idx].name);
        Pkg pkg;
        if (pkgbuild_parse(pbfile, &pkg) == 0) {
            for (int d = 0; d < pkg.ndepends; d++) {
                char depname[MAX_STR];
                dep_name_only(pkg.depends[d], depname);
                int di = index_of(depname);
                if (di >= 0) topo_visit(di);
            }
        }
    }
    in_stack[idx] = 0;
    visited[idx]  = 1;
    build_order[nbuild++] = idx;
}

static void toposort(void) {
    memset(visited,  0, sizeof(visited));
    memset(in_stack, 0, sizeof(in_stack));
    nbuild = 0;
    for (int i = 0; i < nresolved; i++)
        topo_visit(i);
}

int dep_resolve_queue(const char *pkgname,
                      char out[][MAX_STR], int maxout) {
    nresolved = 0;
    collect(pkgname, 0);
    toposort();
    int n = 0;
    for (int i = 0; i < nbuild && n < maxout; i++) {
        DepNode *node = &resolved[build_order[i]];
        if (node->has_src && !node->installed)
            strncpy(out[n++], node->name, MAX_STR - 1);
    }
    return n;
}

/* ── print emerge-style package list ────────────────────────────────────── */
static void print_pkg_list(void) {
    int to_build = 0;
    for (int i = 0; i < nresolved; i++)
        if (resolved[i].has_src && !resolved[i].installed) to_build++;

    printf("\n");
    for (int i = 0; i < nresolved; i++) {
        DepNode *n = &resolved[i];

        /* [src Y/N] */
        const char *sc = n->has_src ? C_GREEN : C_YELLOW;
        const char  sy = n->has_src ? 'Y' : 'N';

        /* status: N=new, R=reinstall */
        char status = n->installed ? 'R' : 'N';
        const char *stc = n->installed ? C_CYAN : C_GREEN;

        /* installed marker */
        const char *inst = n->installed
            ? " " C_CYAN "[installed]" C_RESET : "";

        printf("[%ssrc %c" C_RESET " %s%c" C_RESET "] "
               C_BOLD "%s" C_RESET "/"
               C_BOLD "%s" C_RESET "-"
               "%s"
               "%s\n",
               sc, sy,
               stc, status,
               n->folder[0] != '?' ? n->folder : "repo",
               n->name,
               n->ver[0] ? n->ver : "?",
               inst);
    }

    printf("\n");
    printf("Total: " C_BOLD "%d" C_RESET
           "  installed: " C_CYAN "%d" C_RESET
           "  to build: " C_YELLOW "%d" C_RESET,
           nresolved,
           nresolved - to_build,
           to_build);

    int missing = 0;
    for (int i = 0; i < nresolved; i++)
        if (!resolved[i].has_src) missing++;
    if (missing)
        printf("  no PKGBUILD: " C_RED "%d" C_RESET, missing);
    printf("\n");

    /* build order */
    if (to_build > 0) {
        printf("\n  Build order:\n");
        int order = 1;
        for (int i = 0; i < nresolved; i++) {
            DepNode *n = &resolved[i];
            if (n->has_src && !n->installed)
                printf("    " C_CYAN "%d." C_RESET " %s/%s-%s\n",
                       order++, n->folder[0] != '?' ? n->folder : "repo",
                       n->name, n->ver[0] ? n->ver : "?");
        }
    }
    printf("\n");
}

/* ── cmd_deptree ─────────────────────────────────────────────────────────── */
void cmd_deptree(int argc, char **argv) {
    if (argc == 0) die("No package specified.\nUsage: lpm -D <package>");

    /* collect all packages into single resolved list */
    nresolved = 0;
    for (int a = 0; a < argc; a++) {
        collect(argv[a], 0);
    }

    if (nresolved == 0) {
        fprintf(stderr, C_RED "error: " C_RESET
                "No PKGBUILDs found\n");
        return;
    }

    toposort();
    print_pkg_list();
}

/* called from cmd_sync — set folder info on resolved nodes */
void dep_set_folder(const char *pkgname, const char *folder) {
    for (int i = 0; i < nresolved; i++)
        if (strcmp(resolved[i].name, pkgname) == 0) {
            strncpy(resolved[i].folder, folder, sizeof(resolved[i].folder) - 1);
            return;
        }
}
