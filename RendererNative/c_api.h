#pragma once
#include <stdint.h>

/*
    Core C ABI for SolarFramework native modules.
    - Pure C surface (no STL, no exceptions across the boundary).
    - Stable calling convention + dllexport macro.
    - Blittable types only; no `bool`, no variable-length strings.
*/

#ifdef _WIN32
#define FM_API   __declspec(dllexport)
#define FM_CALL  __cdecl
#else
#define FM_API
#define FM_CALL
#endif

/* ------------------------------
   Common primitive types
--------------------------------*/
typedef uint64_t fm_handle;   /* Opaque handle to native objects */

/* Return codes (int). 0 == OK. Keep negative for errors. */
enum {
    FM_OK = 0,
    FM_E_UNSPECIFIED = -1,
    FM_E_BADARGS = -2,
    FM_E_NOMEM = -3,
    FM_E_DEVICE = -4,
    FM_E_NOTREADY = -5,
    FM_E_OUTOFDATE = -6,
    FM_E_UNSUPPORTED = -7
};

/* Optional logging callback from native → managed (or console) */
typedef void (FM_CALL* fm_log_fn)(int level, const char* msg, void* user);
/* Retrieve last error message (thread-local) from native side */
typedef const char* (FM_CALL* fm_get_last_error_fn)(void);

/* ABI version of the function table returned by each module.
   Bump this when you change struct layouts / signatures. */
#define FM_ABI_VERSION 3

   /* Header that prefixes every exported API table. */
typedef struct fm_header {
    uint32_t               abi_version;     /* == FM_ABI_VERSION */
    fm_get_last_error_fn   get_last_error;  /* never NULL */
    fm_log_fn              log;             /* optional */
    void* log_user;        /* user data for 'log' */
} fm_header;
