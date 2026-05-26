/*
 * libllpm/keyring.c — GPG-based keyring management
 *
 * Arch-style trust model:
 *   - Keys live in a GnuPG homedir at <root>/etc/lpm/gnupg/
 *   - Trust levels: unknown → marginal → full → ultimate
 *   - ultimate = the distro's own master signing key
 *   - Package signatures are detached .sig files (.lpkg.sig or pkgbuild.sig)
 *
 * Implementation delegates to gpg(1) for all crypto operations,
 * and parses gpg --with-colons output for key listing/trust queries.
 *
 * The in-memory llpm_key_t array in the handle provides fast lookups
 * without re-invoking gpg for every package.
 */

#include "llpm/keyring.h"
#include "llpm/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#pragma GCC diagnostic ignored "-Wunused-result"


/* ── GnuPG homedir path ──────────────────────────────────────────────── */

static void gnupg_dir(const llpm_handle_t *h, char *out, size_t sz) {
    snprintf(out, sz, "%s/etc/lpm/gnupg", h->root);
}

/* Build a gpg command with our homedir */
static void gpg_cmd(const llpm_handle_t *h, char *out, size_t sz,
                    const char *subcmd) {
    char gpgdir[LLPM_PATH_MAX];
    gnupg_dir(h, gpgdir, sizeof(gpgdir));
    snprintf(out, sz,
             "gpg --homedir '%s' --no-auto-check-trustdb %s",
             gpgdir, subcmd);
}

/* ── internal: ensure keyring dir exists ─────────────────────────────── */

static int ensure_keyring_dir(llpm_handle_t *h) {
    char gpgdir[LLPM_PATH_MAX];
    gnupg_dir(h, gpgdir, sizeof(gpgdir));

    struct stat st;
    if (stat(gpgdir, &st) == 0 && S_ISDIR(st.st_mode)) return 0;

    /* create with strict permissions (gpg requires 0700) */
    char cmd[LLPM_PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' && chmod 700 '%s'",
             gpgdir, gpgdir);
    return system(cmd) == 0 ? 0 : -1;
}

/* ── internal: parse trust char from gpg --with-colons ──────────────── */

static llpm_trust_t parse_trust_char(char c) {
    switch (c) {
        case 'u': return LLPM_TRUST_ULTIMATE;
        case 'f': return LLPM_TRUST_FULL;
        case 'm': return LLPM_TRUST_MARGINAL;
        default:  return LLPM_TRUST_UNKNOWN;
    }
}

/* ── llpm_keyring_load ───────────────────────────────────────────────── */
/*
 * Parse `gpg --with-colons --list-keys` output into h->keys[].
 *
 * Colon format (relevant fields):
 *   pub:trust:...:...:fingerprint...:...:...:uid:
 *   fpr:::::::::FULLFINGERPRINT:
 *   uid:validity:...:...:...:...:...:uid_string:
 */
int llpm_keyring_load(llpm_handle_t *h) {
    if (!h) return -1;
    if (ensure_keyring_dir(h) != 0) {
        h->last_err = LLPM_ERR_IO;
        return -1;
    }

    char cmd[LLPM_PATH_MAX + 256];
    gpg_cmd(h, cmd, sizeof(cmd),
            "--with-colons --list-keys 2>/dev/null");

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        h->last_err = LLPM_ERR_IO;
        return -1;
    }

    h->nkeys = 0;
    llpm_key_t cur;
    memset(&cur, 0, sizeof(cur));
    int in_key = 0;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = '\0';

        /* split on ':' */
        char *fields[16] = {0};
        int nf = 0;
        char *p = line;
        while (nf < 16) {
            fields[nf++] = p;
            char *next = strchr(p, ':');
            if (!next) break;
            *next = '\0';
            p = next + 1;
        }
        if (nf < 1) continue;

        const char *rec = fields[0];

        if (!strcmp(rec, "pub")) {
            if (in_key && h->nkeys < LLPM_KEY_MAX)
                h->keys[h->nkeys++] = cur;
            memset(&cur, 0, sizeof(cur));
            in_key = 1;
            if (nf > 1 && fields[1][0])
                cur.trust = parse_trust_char(fields[1][0]);
            /* expiry in field 6 */
            if (nf > 6 && fields[6][0])
                cur.expires = (time_t)atol(fields[6]);
        } else if (!strcmp(rec, "fpr") && in_key) {
            if (nf > 9 && fields[9][0])
                strncpy(cur.fingerprint, fields[9],
                        sizeof(cur.fingerprint) - 1);
        } else if (!strcmp(rec, "uid") && in_key) {
            if (nf > 9 && fields[9][0] && !cur.uid[0])
                strncpy(cur.uid, fields[9], sizeof(cur.uid) - 1);
        } else if (!strcmp(rec, "rev") && in_key) {
            cur.revoked = 1;
        }
    }
    if (in_key && h->nkeys < LLPM_KEY_MAX)
        h->keys[h->nkeys++] = cur;

    pclose(fp);
    h->keyring_loaded = 1;
    h->last_err = LLPM_ERR_OK;
    return h->nkeys;
}

/* ── llpm_keyring_save ───────────────────────────────────────────────── */
/*
 * Write trust changes back with `gpg --import-ownertrust`.
 * Format: "FINGERPRINT:trust_level:\n" (gpg ownertrust format).
 */
int llpm_keyring_save(llpm_handle_t *h) {
    if (!h) return -1;

    char tmpfile[64];
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/llpm-ownertrust.%d", (int)getpid());
    FILE *fp = fopen(tmpfile, "w");
    if (!fp) {
        h->last_err = LLPM_ERR_IO;
        return -1;
    }

    /* gpg trust value: 2=unknown, 3=marginal, 4=full, 5=ultimate */
    static const int gpg_trust[] = { 2, 3, 4, 5 };

    for (int i = 0; i < h->nkeys; i++) {
        if (!h->keys[i].fingerprint[0]) continue;
        int tv = gpg_trust[(int)h->keys[i].trust];
        fprintf(fp, "%s:%d:\n", h->keys[i].fingerprint, tv);
    }
    fclose(fp);

    char cmd[LLPM_PATH_MAX + 256];
    gpg_cmd(h, cmd, sizeof(cmd), "");  /* get gpg prefix */
    char full[LLPM_PATH_MAX + 512];
    snprintf(full, sizeof(full),
             "gpg --homedir '%s%s' --import-ownertrust '%s' 2>/dev/null",
             h->root, "/etc/lpm/gnupg", tmpfile);
    int rc = system(full);
    remove(tmpfile);

    if (rc != 0) {
        h->last_err = LLPM_ERR_IO;
        return -1;
    }
    h->last_err = LLPM_ERR_OK;
    return 0;
}

/* ── llpm_keyring_recv ───────────────────────────────────────────────── */

int llpm_keyring_recv(llpm_handle_t *h, const char *keyid) {
    if (!h || !keyid || !keyid[0]) {
        if (h) h->last_err = LLPM_ERR_INVAL;
        return -1;
    }
    if (ensure_keyring_dir(h) != 0) {
        h->last_err = LLPM_ERR_IO;
        return -1;
    }

    char gpgdir[LLPM_PATH_MAX];
    gnupg_dir(h, gpgdir, sizeof(gpgdir));

    /* try a list of common keyservers */
    const char *servers[] = {
        "hkps://keys.openpgp.org",
        "hkps://keyserver.ubuntu.com",
        "hkps://pgp.mit.edu",
    };
    int nservers = (int)(sizeof(servers)/sizeof(servers[0]));

    for (int s = 0; s < nservers; s++) {
        char cmd[LLPM_PATH_MAX + 512];
        snprintf(cmd, sizeof(cmd),
                 "gpg --homedir '%s' --keyserver '%s'"
                 " --recv-keys '%s' 2>/dev/null",
                 gpgdir, servers[s], keyid);
        if (system(cmd) == 0) {
            /* refresh in-memory cache */
            llpm_keyring_load(h);
            h->last_err = LLPM_ERR_OK;
            return 0;
        }
    }

    h->last_err = LLPM_ERR_KEY_MISSING;
    snprintf(h->last_err_detail, sizeof(h->last_err_detail),
             "key '%s' not found on any keyserver", keyid);
    return -1;
}

/* ── llpm_keyring_import ─────────────────────────────────────────────── */

int llpm_keyring_import(llpm_handle_t *h, const char *filepath) {
    if (!h || !filepath) { if (h) h->last_err = LLPM_ERR_INVAL; return -1; }
    if (ensure_keyring_dir(h) != 0) { h->last_err = LLPM_ERR_IO; return -1; }

    char gpgdir[LLPM_PATH_MAX];
    gnupg_dir(h, gpgdir, sizeof(gpgdir));

    char cmd[LLPM_PATH_MAX * 2 + 64];
    snprintf(cmd, sizeof(cmd),
             "gpg --homedir '%s' --import '%s' 2>/dev/null",
             gpgdir, filepath);
    if (system(cmd) != 0) {
        h->last_err = LLPM_ERR_IO;
        return -1;
    }
    llpm_keyring_load(h);
    h->last_err = LLPM_ERR_OK;
    return 0;
}

/* ── llpm_keyring_set_trust ──────────────────────────────────────────── */

int llpm_keyring_set_trust(llpm_handle_t *h, const char *fingerprint,
                            llpm_trust_t level) {
    if (!h || !fingerprint) { if (h) h->last_err = LLPM_ERR_INVAL; return -1; }
    for (int i = 0; i < h->nkeys; i++) {
        if (strstr(h->keys[i].fingerprint, fingerprint) ||
            strstr(fingerprint, h->keys[i].fingerprint)) {
            h->keys[i].trust = level;
            llpm_keyring_save(h);
            h->last_err = LLPM_ERR_OK;
            return 0;
        }
    }
    h->last_err = LLPM_ERR_KEY_MISSING;
    return -1;
}

/* ── llpm_keyring_find ───────────────────────────────────────────────── */

const llpm_key_t *llpm_keyring_find(const llpm_handle_t *h,
                                     const char *fingerprint) {
    if (!h || !fingerprint) return NULL;
    for (int i = 0; i < h->nkeys; i++) {
        /* accept full fingerprint or last 16 chars (keyid) */
        const char *fp = h->keys[i].fingerprint;
        int fplen = (int)strlen(fp);
        int kidlen = (int)strlen(fingerprint);
        if (fplen >= kidlen &&
            !strcasecmp(fp + fplen - kidlen, fingerprint))
            return &h->keys[i];
    }
    return NULL;
}

/* ── llpm_keyring_list ───────────────────────────────────────────────── */

const llpm_key_t *llpm_keyring_list(const llpm_handle_t *h, int *out_n) {
    if (!h) { if (out_n) *out_n = 0; return NULL; }
    if (!h->keyring_loaded) {
        /* const cast is unavoidable for a lazy-load in a const function;
         * we accept this trade-off */
        llpm_keyring_load((llpm_handle_t *)h);
    }
    if (out_n) *out_n = h->nkeys;
    return h->keys;
}

/* ── llpm_keyring_verify ─────────────────────────────────────────────── */
/*
 * Verify a detached GPG signature.
 *
 * sig_required:
 *   0 — if no .sig file exists, return OK; if it exists but is bad, warn
 *   1 — .sig must exist and must verify against a trusted key
 *
 * Returns 0 on success, -1 on failure (h->last_err set).
 */
int llpm_keyring_verify(llpm_handle_t *h, const char *filepath,
                         const char *sigpath, int sig_required) {
    if (!h || !filepath) { if (h) h->last_err = LLPM_ERR_INVAL; return -1; }

    /* determine sigpath if not provided */
    char auto_sig[LLPM_PATH_MAX];
    if (!sigpath) {
        snprintf(auto_sig, sizeof(auto_sig), "%s.sig", filepath);
        sigpath = auto_sig;
    }

    struct stat st;
    int sig_exists = (stat(sigpath, &st) == 0 && st.st_size > 0);

    if (!sig_exists) {
        if (!sig_required) {
            h->last_err = LLPM_ERR_OK;
            return 0;   /* no sig, not required — OK */
        }
        h->last_err = LLPM_ERR_SIG_MISSING;
        snprintf(h->last_err_detail, sizeof(h->last_err_detail),
                 "signature file not found: %s", sigpath);
        return -1;
    }

    char gpgdir[LLPM_PATH_MAX];
    gnupg_dir(h, gpgdir, sizeof(gpgdir));

    /* run gpg verify; capture output to parse the signing key */
    char cmd[LLPM_PATH_MAX * 3 + 128];
    snprintf(cmd, sizeof(cmd),
             "gpg --homedir '%s' --status-fd 1 --verify '%s' '%s' 2>/dev/null",
             gpgdir, sigpath, filepath);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        h->last_err = LLPM_ERR_IO;
        return -1;
    }

    int good_sig = 0;
    char signer_fpr[65] = "";
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* [GNUPG:] GOODSIG <keyid> <uid> */
        if (strstr(line, "[GNUPG:] GOODSIG")) good_sig = 1;
        /* [GNUPG:] VALIDSIG <fpr> ... */
        if (strstr(line, "[GNUPG:] VALIDSIG")) {
            char *p = strstr(line, "VALIDSIG ");
            if (p) {
                p += 9;
                int i = 0;
                while (*p && !isspace((unsigned char)*p) && i < 64)
                    signer_fpr[i++] = *p++;
                signer_fpr[i] = '\0';
            }
        }
    }
    pclose(fp);

    if (!good_sig) {
        h->last_err = LLPM_ERR_SIG_INVALID;
        snprintf(h->last_err_detail, sizeof(h->last_err_detail),
                 "signature verification failed for %s", filepath);
        return -1;
    }

    /* if sig_required, ensure the signing key is trusted */
    if (sig_required && signer_fpr[0]) {
        const llpm_key_t *k = llpm_keyring_find(h, signer_fpr);
        if (!k || k->trust < LLPM_TRUST_FULL) {
            h->last_err = LLPM_ERR_KEY_UNTRUSTED;
            snprintf(h->last_err_detail, sizeof(h->last_err_detail),
                     "signing key '%s' is not trusted (need full/ultimate trust)",
                     signer_fpr[0] ? signer_fpr : "unknown");
            return -1;
        }
        if (k->revoked) {
            h->last_err = LLPM_ERR_KEY_UNTRUSTED;
            snprintf(h->last_err_detail, sizeof(h->last_err_detail),
                     "signing key '%s' has been revoked", k->fingerprint);
            return -1;
        }
    }

    h->last_err = LLPM_ERR_OK;
    return 0;
}

/* ── llpm_keyring_delete ─────────────────────────────────────────────── */

int llpm_keyring_delete(llpm_handle_t *h, const char *fingerprint) {
    if (!h || !fingerprint) { if (h) h->last_err = LLPM_ERR_INVAL; return -1; }

    char gpgdir[LLPM_PATH_MAX];
    gnupg_dir(h, gpgdir, sizeof(gpgdir));

    char cmd[LLPM_PATH_MAX + 256];
    snprintf(cmd, sizeof(cmd),
             "gpg --homedir '%s' --batch --yes"
             " --delete-key '%s' 2>/dev/null",
             gpgdir, fingerprint);
    if (system(cmd) != 0) {
        h->last_err = LLPM_ERR_KEY_MISSING;
        return -1;
    }
    llpm_keyring_load(h);
    h->last_err = LLPM_ERR_OK;
    return 0;
}

#pragma GCC diagnostic pop
