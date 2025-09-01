#pragma once
#include <stdint.h>

#ifdef _WIN32
#define FM_API  __declspec(dllexport)
#define FM_CALL __cdecl
#else
#define FM_API
#define FM_CALL
#endif

// Opaque handle that travels over interop
typedef uint64_t fw_handle;

// Logger / error function signatures
typedef const char* (FM_CALL* fw_get_last_error_fn)(void);
typedef void        (FM_CALL* fw_log_fn)(int level, const char* msg, void* user);

// Common header placed at the front of every exported API table
typedef struct fw_header {
    uint32_t             abi_version;
    fw_get_last_error_fn get_last_error;  // returns char*
    void* log_cb;          // fw_log_fn (stored as void*)
    void* log_user;        // user pointer
} fw_header;
