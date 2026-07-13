#include "lpm.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"



int cksum_verify(const char *path, const char *expected, CksumType type) {
    if (type == CKSUM_INVALID) {
        fprintf(stderr, C_RED "error:" C_RESET
            " refusing to verify %s: invalid checksum format\n"
            "  expected: sha512:<hex> | sha256:<hex> | md5:<hex> | SKIP\n",
            path);
        return -1;
    }
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
 * LPDF v1 supports exactly four checksum formats, matching SPEC.md:    *
 *   sha512:<hex>   sha256:<hex>   md5:<hex>   SKIP                     *
 * There is no bare-hex auto-detection and no per-algorithm fallback.   *
 * Anything else — bare hex, an unrecognized algo prefix (incl. sha1:,  *
 * which CksumType has never had a variant for), or empty — is a parse *
 * error: CKSUM_INVALID. Callers must reject the package outright, the  *
 * same way they'd handle any other malformed LPDF field — never treat *
 * CKSUM_INVALID as CKSUM_SKIP.                                         *
 *                                                                        *
 * Output: fills *hash_out (just the hex part) for the three real algo  *
 * cases; returns the matching CksumType, CKSUM_SKIP for "SKIP", or     *
 * CKSUM_INVALID for anything that doesn't match the four forms above.  */
CksumType checksum_parse_unified(const char *spec,
                                  char *hash_out, size_t hash_outsz) {
    if (hash_out) hash_out[0] = '\0';

    if (!spec || !spec[0]) return CKSUM_INVALID;
    if (!strcmp(spec, "SKIP")) return CKSUM_SKIP;

    const char *colon = strchr(spec, ':');
    if (!colon) return CKSUM_INVALID; /* bare hex — no longer guessed */

    const char *algo = spec;
    size_t alen = (size_t)(colon - algo);
    const char *hex  = colon + 1;

    CksumType t;
    if      (alen == 6 && !strncmp(algo, "sha512", 6)) t = CKSUM_SHA512;
    else if (alen == 6 && !strncmp(algo, "sha256", 6)) t = CKSUM_SHA256;
    else if (alen == 3 && !strncmp(algo, "md5",    3)) t = CKSUM_MD5;
    else return CKSUM_INVALID; /* e.g. "sha1:..." — not a valid LPDF prefix */

    if (hash_out)
        strncpy(hash_out, hex, hash_outsz - 1);
    return t;
}
