/*
 * libllpm/dep.c — dependency resolution + conflict detection
 *
 * Algorithm (Arch/pacman-inspired):
 *  1. For each install op in the transaction, recursively collect all
 *     transitive dependencies from the repo cache.
 *  2. Remove any dep that is already installed AND satisfies the version
 *     constraint.
 *  3. Detect cycles using DFS with an "in-stack" marker array.
 *  4. Topological sort → final install order stored back in trans ops[].
 *  5. Conflict detection: for every package to be installed, check its
 *     conflicts[] list against (a) other packages in the install set,
 *     (b) currently installed packages.
 */

#include "llpm/dep.h"
#include "llpm/repo.h"
#include "llpm/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#pragma GCC diagnostic ignored "-Wrestrict"


/* ── version compare (local copy — avoids linking util.c) ──────────── */

static int seg_cmp(const char *a, const char *b) {
    while (*a || *b) {
        while (*a && !isalnum((unsigned char)*a)) a++;
        while (*b && !isalnum((unsigned char)*b)) b++;
        if (!*a && !*b) return 0;
        if (!*a) return -1;
        if (!*b) return  1;

        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *na = a, *nb = b;
            while (isdigit((unsigned char)*a)) a++;
            while (isdigit((unsigned char)*b)) b++;
            int la = (int)(a-na), lb = (int)(b-nb);
            if (la != lb) return la < lb ? -1 : 1;
            int c = strncmp(na, nb, la);
            if (c) return c < 0 ? -1 : 1;
        } else {
            const char *sa = a, *sb = b;
            while (isalpha((unsigned char)*a)) a++;
            while (isalpha((unsigned char)*b)) b++;
            int la = (int)(a-sa), lb = (int)(b-sb);
            int mn = la < lb ? la : lb;
            int c = strncmp(sa, sb, mn);
            if (c) return c < 0 ? -1 : 1;
            if (la != lb) return la < lb ? -1 : 1;
        }
    }
    return 0;
}

static int dep_ver_cmp(const char *a, const char *b) {
    const char *ca = strchr(a, ':'), *cb = strchr(b, ':');
    long ea = ca ? atol(a) : 0, eb = cb ? atol(b) : 0;
    if (ea != eb) return ea < eb ? -1 : 1;
    if (ca) a = ca+1;
    if (cb) b = cb+1;
    return seg_cmp(a, b);
}

/* ── llpm_dep_parse_spec ─────────────────────────────────────────────── */

int llpm_dep_parse_spec(const char *spec, llpm_dep_spec_t *out) {
    if (!spec || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->op = LLPM_VC_ANY;

    /* try operators longest-first */
    const struct { const char *op; llpm_vc_op_t code; } OPS[] = {
        { ">=", LLPM_VC_GE }, { "<=", LLPM_VC_LE },
        { ">",  LLPM_VC_GT }, { "<",  LLPM_VC_LT },
        { "=",  LLPM_VC_EQ },
    };
    int nops = (int)(sizeof(OPS)/sizeof(OPS[0]));

    for (int i = 0; i < nops; i++) {
        const char *pos = strstr(spec, OPS[i].op);
        if (pos) {
            int namelen = (int)(pos - spec);
            if (namelen <= 0 || namelen >= LLPM_NAME_MAX) return -1;
            strncpy(out->name, spec, namelen);
            out->op = OPS[i].code;
            strncpy(out->version, pos + strlen(OPS[i].op), LLPM_VER_MAX - 1);
            return 0;
        }
    }
    /* no operator */
    strncpy(out->name, spec, LLPM_NAME_MAX - 1);
    return 0;
}

/* ── llpm_dep_installed_ver ─────────────────────────────────────────── *
 * Returns the installed version of a package, or "" if not installed.  */
static int llpm_dep_installed_ver(llpm_handle_t *h, const char *name,
                                   char *ver_out, size_t ver_outsz) {
    char dbfile[LLPM_PATH_MAX];
    snprintf(dbfile, sizeof(dbfile), "%s/installed", h->dbpath);
    FILE *fp = fopen(dbfile, "r");
    if (!fp) return -1;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = (char)0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = (char)0;
        if (strcmp(line, name) != 0) continue;
        fclose(fp);
        snprintf(ver_out, ver_outsz, "%s", eq + 1);
        return 0;
    }
    fclose(fp);
    return -1;
}

/* ── llpm_dep_satisfied ──────────────────────────────────────────────── */

int llpm_dep_satisfied(llpm_handle_t *h, const llpm_dep_spec_t *spec) {
    if (!h || !spec) return 0;

    /* read from installed DB */
    char dbfile[LLPM_PATH_MAX];
    snprintf(dbfile, sizeof(dbfile), "%s/installed", h->dbpath);
    FILE *fp = fopen(dbfile, "r");
    if (!fp) return 0;

    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = '\0';
        if (!line[0]) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *name = line;
        const char *ver  = eq + 1;

        if (strcmp(name, spec->name) != 0) continue;
        /* name matches — now check version constraint */
        if (spec->op == LLPM_VC_ANY) { found = 1; break; }
        int c = dep_ver_cmp(ver, spec->version);
        switch (spec->op) {
            case LLPM_VC_EQ: found = (c == 0); break;
            case LLPM_VC_GE: found = (c >= 0); break;
            case LLPM_VC_GT: found = (c >  0); break;
            case LLPM_VC_LE: found = (c <= 0); break;
            case LLPM_VC_LT: found = (c <  0); break;
            default:         found = 0;
        }
        break;
    }
    fclose(fp);
    return found;
}

/* ── dep resolver internals ──────────────────────────────────────────── */

#define DEP_VISIT_MAX 1024

typedef struct {
    char name[LLPM_NAME_MAX];
    int  in_stack;
    int  visited;
} DepNode;

typedef struct {
    llpm_handle_t *h;
    DepNode        nodes[DEP_VISIT_MAX];
    int            nnodes;
    char           order[DEP_VISIT_MAX][LLPM_NAME_MAX];
    int            norder;
    /* errors */
    char           missing[LLPM_DEP_MAX][LLPM_NAME_MAX];
    int            nmissing;
    char           cycles[LLPM_DEP_MAX][LLPM_NAME_MAX * 2];
    int            ncycles;
    /* version-upgrade needed: "pkgname:installed_ver:op+required" */
    char           upgrades[LLPM_DEP_MAX][LLPM_NAME_MAX * 3];
    int            nupgrades;
} DepCtx;

static int dep_find_node(DepCtx *ctx, const char *name) {
    for (int i = 0; i < ctx->nnodes; i++)
        if (!strcmp(ctx->nodes[i].name, name)) return i;
    return -1;
}

static int dep_add_node(DepCtx *ctx, const char *name) {
    if (ctx->nnodes >= DEP_VISIT_MAX) return -1;
    int i = ctx->nnodes++;
    memset(&ctx->nodes[i], 0, sizeof(ctx->nodes[i]));
    strncpy(ctx->nodes[i].name, name, LLPM_NAME_MAX - 1);
    return i;
}

/* DFS — collects topo order, detects cycles */
static void dep_visit(DepCtx *ctx, int idx) {
    if (ctx->nodes[idx].visited) return;
    if (ctx->nodes[idx].in_stack) {
        /* cycle */
        if (ctx->ncycles < LLPM_DEP_MAX) {
            snprintf(ctx->cycles[ctx->ncycles++],
                     LLPM_NAME_MAX * 2 - 1,
                     "%s (cycle)", ctx->nodes[idx].name);
        }
        return;
    }
    ctx->nodes[idx].in_stack = 1;

    /* fetch deps from repo */
    llpm_pkg_t pkg;
    if (llpm_repo_find_pkg(ctx->h, ctx->nodes[idx].name, &pkg) == 0) {
        for (int di = 0; di < pkg.ndepends; di++) {
            llpm_dep_spec_t spec;
            if (llpm_dep_parse_spec(pkg.depends[di], &spec) != 0) continue;

            /* Check if installed version satisfies the constraint */
            char inst_ver[LLPM_VER_MAX] = "";
            int is_installed = (llpm_dep_installed_ver(ctx->h, spec.name,
                                    inst_ver, sizeof(inst_ver)) == 0);

            if (is_installed) {
                if (llpm_dep_satisfied(ctx->h, &spec)) {
                    /* installed and satisfies constraint — skip */
                    continue;
                }
                /* installed but version too old/wrong — need upgrade */
                if (ctx->nupgrades < LLPM_DEP_MAX) {
                    /* store "pkgname:installed:required" */
                    snprintf(ctx->upgrades[ctx->nupgrades++],
                             LLPM_NAME_MAX * 3,
                             "%s:%s:%s%s",
                             spec.name, inst_ver,
                             spec.op == LLPM_VC_GE ? ">=" :
                             spec.op == LLPM_VC_GT ? ">"  :
                             spec.op == LLPM_VC_LE ? "<=" :
                             spec.op == LLPM_VC_LT ? "<"  :
                             spec.op == LLPM_VC_EQ ? "="  : "",
                             spec.version);
                }
                /* still add to queue — dep resolver will upgrade it */
            }

            int cidx = dep_find_node(ctx, spec.name);
            if (cidx < 0) cidx = dep_add_node(ctx, spec.name);
            if (cidx >= 0) dep_visit(ctx, cidx);
        }
    } else {
        /* pkg not in any repo — add to missing if not already installed */
        llpm_dep_spec_t any;
        memset(&any, 0, sizeof(any));
        strncpy(any.name, ctx->nodes[idx].name, LLPM_NAME_MAX - 1);
        any.op = LLPM_VC_ANY;
        if (!llpm_dep_satisfied(ctx->h, &any) && ctx->nmissing < LLPM_DEP_MAX) {
            strncpy(ctx->missing[ctx->nmissing++],
                    ctx->nodes[idx].name, LLPM_NAME_MAX - 1);
        }
    }

    ctx->nodes[idx].in_stack = 0;
    ctx->nodes[idx].visited  = 1;

    if (ctx->norder < DEP_VISIT_MAX)
        strncpy(ctx->order[ctx->norder++], ctx->nodes[idx].name,
                LLPM_NAME_MAX - 1);
}

/* ── llpm_dep_resolve ────────────────────────────────────────────────── */

int llpm_dep_resolve(llpm_handle_t *h, llpm_trans_t *t) {
    if (!h || !t) return -1;

    /* skip if NODEPS flag */
    if (t->flags & LLPM_TRANS_NODEPS) {
        t->nmissing = 0;
        h->last_err = LLPM_ERR_OK;
        return 0;
    }

    DepCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.h = h;

    /* seed with install targets */
    for (int i = 0; i < t->nops; i++) {
        if (t->ops[i].type == LLPM_TRANS_TYPE_INSTALL ||
            t->ops[i].type == LLPM_TRANS_TYPE_UPGRADE) {
            int idx = dep_find_node(&ctx, t->ops[i].pkg.name);
            if (idx < 0) idx = dep_add_node(&ctx, t->ops[i].pkg.name);
        }
    }

    /* run DFS for each seed */
    for (int i = 0; i < ctx.nnodes; i++)
        dep_visit(&ctx, i);

    /* copy results into transaction */
    t->nmissing          = ctx.nmissing;
    t->nconflicts        = 0;
    t->nupgrades_needed  = ctx.nupgrades;

    for (int i = 0; i < ctx.nmissing && i < LLPM_DEP_MAX; i++)
        strncpy(t->missing_deps[i], ctx.missing[i], LLPM_NAME_MAX - 1);

    for (int i = 0; i < ctx.nupgrades && i < 64; i++)
        strncpy(t->upgrades_needed[i], ctx.upgrades[i], LLPM_NAME_MAX * 3 - 1);

    /* conflict detection */
    if (!(t->flags & LLPM_TRANS_NOCONFLICT)) {
        for (int oi = 0; oi < ctx.norder; oi++) {
            llpm_pkg_t pkg;
            if (llpm_repo_find_pkg(h, ctx.order[oi], &pkg) != 0) continue;
            for (int ci = 0; ci < pkg.nconflicts; ci++) {
                /* check against install set */
                for (int oj = 0; oj < ctx.norder; oj++) {
                    if (oi == oj) continue;
                    if (!strcmp(ctx.order[oj], pkg.conflicts[ci])) {
                        if (t->nconflicts < LLPM_DEP_MAX) {
                            snprintf(t->conflicts[t->nconflicts++],
                                     LLPM_NAME_MAX * 2 - 1,
                                     "%s <-> %s",
                                     ctx.order[oi], pkg.conflicts[ci]);
                        }
                    }
                }
                /* check against installed */
                llpm_dep_spec_t spec;
                llpm_dep_parse_spec(pkg.conflicts[ci], &spec);
                if (llpm_dep_satisfied(h, &spec)) {
                    if (t->nconflicts < LLPM_DEP_MAX) {
                        snprintf(t->conflicts[t->nconflicts++],
                                 LLPM_NAME_MAX * 2 - 1,
                                 "%s conflicts with installed %s",
                                 ctx.order[oi], spec.name);
                    }
                }
            }
        }
    }

    /* Expand install ops with resolved deps (prepend deps in topo order) */
    /* Build a new ops list: deps first, then original explicit targets */
    llpm_trans_op_t new_ops[LLPM_TRANS_OP_MAX];
    int n_new = 0;

    /* deps (from topo order, skip already-in-ops) */
    for (int oi = 0; oi < ctx.norder && n_new < LLPM_TRANS_OP_MAX; oi++) {
        const char *dep_name = ctx.order[oi];
        /* skip if already an explicit target */
        int is_explicit = 0;
        for (int ti = 0; ti < t->nops; ti++) {
            if (!strcmp(t->ops[ti].pkg.name, dep_name)) {
                is_explicit = 1; break;
            }
        }
        /* skip if already installed (and no upgrade needed) */
        llpm_dep_spec_t any; memset(&any, 0, sizeof(any));
        strncpy(any.name, dep_name, LLPM_NAME_MAX-1);
        any.op = LLPM_VC_ANY;
        if (llpm_dep_satisfied(h, &any) && !is_explicit) continue;

        if (!is_explicit) {
            llpm_trans_op_t *op = &new_ops[n_new++];
            memset(op, 0, sizeof(*op));
            op->type = LLPM_TRANS_TYPE_INSTALL;
            op->pkg.reason = LLPM_REASON_DEP;
            strncpy(op->pkg.name, dep_name, LLPM_NAME_MAX - 1);
            /* fetch version from repo if available */
            { llpm_pkg_t _tmp;
              if (llpm_repo_find_pkg(h, dep_name, &_tmp) == 0)
                  strncpy(op->pkg.version, _tmp.version,
                          sizeof(op->pkg.version) - 1); }
        }
    }

    /* now add original ops */
    for (int ti = 0; ti < t->nops && n_new < LLPM_TRANS_OP_MAX; ti++)
        new_ops[n_new++] = t->ops[ti];

    /* replace */
    memcpy(t->ops, new_ops, n_new * sizeof(llpm_trans_op_t));
    t->nops = n_new;

    if (ctx.nmissing > 0 || (ctx.ncycles > 0)) {
        h->last_err = (ctx.ncycles > 0) ? LLPM_ERR_DEP_CYCLE
                                         : LLPM_ERR_DEP_MISSING;
        return -1;
    }
    if (t->nconflicts > 0 && !(t->flags & LLPM_TRANS_NOCONFLICT)) {
        h->last_err = LLPM_ERR_CONFLICT;
        return -1;
    }

    h->last_err = LLPM_ERR_OK;
    return 0;
}

/* ── llpm_dep_reverse ────────────────────────────────────────────────── */

int llpm_dep_reverse(llpm_handle_t *h, const char *pkgname,
                      char rdeps[][LLPM_NAME_MAX], int maxout) {
    if (!h || !pkgname || !rdeps || maxout <= 0) return 0;

    char dbfile[LLPM_PATH_MAX];
    snprintf(dbfile, sizeof(dbfile), "%s/installed", h->dbpath);
    FILE *fp = fopen(dbfile, "r");
    if (!fp) return 0;

    int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp) && n < maxout) {
        line[strcspn(line, "\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *name = line;
        if (!strcmp(name, pkgname)) continue;

        /* check if this package depends on pkgname */
        char pbfile[LLPM_PATH_MAX];
        snprintf(pbfile, sizeof(pbfile), LLPM_PKGBUILD_DIR "/pkgbuild_%s", name);
        FILE *pb = fopen(pbfile, "r");
        if (!pb) continue;
        char pbline[2048];
        int found_dep = 0;
        while (fgets(pbline, sizeof(pbline), pb)) {
            if (strstr(pbline, pkgname)) { found_dep = 1; break; }
        }
        fclose(pb);
        if (found_dep)
            strncpy(rdeps[n++], name, LLPM_NAME_MAX - 1);
    }
    fclose(fp);
    return n;
}

#pragma GCC diagnostic pop
