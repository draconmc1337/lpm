#pragma once

#include "handle.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LLPM_TRANS_OP_MAX  256

/* Transaction flags */
#define LLPM_TRANS_NODEPS     (1 << 0)
#define LLPM_TRANS_NOCONFLICT (1 << 1)
#define LLPM_TRANS_FORCE      (1 << 2)

/* Transaction operation types */
typedef enum {
    LLPM_TRANS_TYPE_INSTALL = 0,
    LLPM_TRANS_TYPE_UPGRADE,
    LLPM_TRANS_TYPE_REMOVE,
} llpm_trans_type_t;

/* Install reason */
typedef enum {
    LLPM_REASON_EXPLICIT = 0,
    LLPM_REASON_DEP,
} llpm_reason_t;

/* A package entry inside a transaction op */
typedef struct {
    char          name[LLPM_NAME_MAX];
    char          version[64];
    llpm_reason_t reason;
} llpm_trans_pkg_t;

/* A single operation in a transaction */
typedef struct {
    llpm_trans_type_t type;
    llpm_trans_pkg_t  pkg;
} llpm_trans_op_t;

typedef struct llpm_trans {
    llpm_handle_t   *h;
    int              flags;
    int              prepared;
    int              committed;
    /* dep-resolve results */
    llpm_trans_op_t  ops[LLPM_TRANS_OP_MAX];
    int              nops;
    char             missing_deps[64][LLPM_NAME_MAX];
    int              nmissing;
    char             conflicts[64][LLPM_NAME_MAX * 2 + 8];
    int              nconflicts;
    /* version-upgrade required: "pkgname:installed:op+required" */
    char             upgrades_needed[64][LLPM_NAME_MAX * 3];
    int              nupgrades_needed;
} llpm_trans_t;

llpm_trans_t *llpm_trans_init(llpm_handle_t *h, int flags);
int llpm_trans_prepare(llpm_trans_t *t);
int llpm_trans_commit(llpm_trans_t *t);
int llpm_trans_release(llpm_trans_t *t);

#ifdef __cplusplus
}
#endif
