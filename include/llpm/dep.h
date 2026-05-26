#pragma once

#include "handle.h"
#include "trans.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LLPM_DEP_MAX   64
#define LLPM_VER_MAX   64

typedef enum {
    LLPM_VC_ANY = 0,
    LLPM_VC_EQ,
    LLPM_VC_GE,
    LLPM_VC_GT,
    LLPM_VC_LE,
    LLPM_VC_LT,
} llpm_vc_t;
typedef llpm_vc_t llpm_vc_op_t;  /* alias used in dep.c */

typedef struct {
    char    name[LLPM_NAME_MAX];
    llpm_vc_t op;
    char    version[LLPM_VER_MAX];
} llpm_dep_spec_t;

typedef struct {
    char name[LLPM_NAME_MAX];
    char version[LLPM_VER_MAX];
    char depends[LLPM_DEP_MAX][LLPM_NAME_MAX];
    int  ndepends;
    char conflicts[LLPM_DEP_MAX][LLPM_NAME_MAX];
    int  nconflicts;
} llpm_pkg_t;

int llpm_dep_parse_spec(const char *spec, llpm_dep_spec_t *out);
int llpm_dep_satisfied(llpm_handle_t *h, const llpm_dep_spec_t *spec);
int llpm_dep_resolve(llpm_handle_t *h, llpm_trans_t *t);
int llpm_check_dependencies(llpm_handle_t *h, const char *const *pkgs, size_t count);

#ifdef __cplusplus
}
#endif
