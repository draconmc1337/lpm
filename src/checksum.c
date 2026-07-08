#include "lpm.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"



int cksum_verify(const char *path, const char *expected, CksumType type) {
    if (type == CKSUM_SKIP || !expected || !expected[0]) return 0;

    char cmd[LPM_PATH_MAX + 256];
    char actual[129] = "";

    if (type == CKSUM_SHA256)
        snprintf(cmd, sizeof(cmd), "sha256sum '%s' | cut -d' ' -f1", path);
    else if (type == CKSUM_SHA512)
        snprintf(cmd, sizeof(cmd), "sha512sum '%s' | cut -d' ' -f1", path);
    else if (type == CKSUM_MD5)
        snprintf(cmd, sizeof(cmd), "md5sum '%s' | cut -d' ' -f1", path);
    else return 0;

    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    (void)fgets(actual, sizeof(actual), p);
    pclose(p);
    actual[strcspn(actual, "\n")] = '\0';

    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, C_RED "error:" C_RESET
            " checksum mismatch for %s\n"
            "  expected: %s\n"
            "  got:      %s\n", path, expected, actual);
        return -1;
    }
    return 0;
}

#pragma GCC diagnostic pop

/* ── checksum_parse_unified ─────────────────────────────────────────── *
 * Parse "algo:hexhash" or legacy bare hex (auto-detect by length).     *
 *                                                                        *
 * Input:  "sha512:aabb..."  "sha256:aabb..."  "md5:aabb..."             *
 *         "SKIP"            bare hex (64 → sha256, 128 → sha512, etc.) *
 *                                                                        *
 * Output: fills *hash_out (just the hex part), returns CksumType.      *
 * Returns CKSUM_SKIP for "SKIP" or empty, CKSUM_AUTO on parse error.   */
CksumType checksum_parse_unified(const char *spec,
                                  char *hash_out, size_t hash_outsz) {
    if (!spec || !spec[0] || !strcmp(spec, "SKIP")) {
        if (hash_out) hash_out[0] = (char)0;
        return CKSUM_SKIP;
    }

    /* "algo:hex" format */
    const char *colon = strchr(spec, ':');
    if (colon) {
        const char *algo = spec;
        size_t alen = (size_t)(colon - algo);
        const char *hex  = colon + 1;

        if (hash_out)
            strncpy(hash_out, hex, hash_outsz - 1);

        if      (alen == 6 && !strncmp(algo, "sha512", 6)) return CKSUM_SHA512;
        else if (alen == 6 && !strncmp(algo, "sha256", 6)) return CKSUM_SHA256;
        else if (alen == 3 && !strncmp(algo, "md5",    3)) return CKSUM_MD5;
        /* unrecognized algo (incl. "sha1:", never a valid LPDF prefix —
         * CksumType has no SHA1 variant) falls through to CKSUM_SKIP.
         * Previously this mislabeled sha1: hashes as CKSUM_SHA256, which
         * would silently fail verification against the real sha256 of the
         * file instead of rejecting the recipe outright. */
        return CKSUM_SKIP;
    }

    /* legacy bare hex — auto-detect by length */
    size_t n = strlen(spec);
    int all_hex = 1;
    for (size_t k = 0; k < n && all_hex; k++) {
        char c = spec[k];
        all_hex = (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');
    }
    if (!all_hex) return CKSUM_SKIP;

    if (hash_out)
        strncpy(hash_out, spec, hash_outsz - 1);

    if      (n == 128) return CKSUM_SHA512;
    else if (n == 64)  return CKSUM_SHA256;
    else if (n == 32)  return CKSUM_MD5;
    return CKSUM_SKIP;
}
