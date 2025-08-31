#pragma once
#include <stdint.h>

#ifdef _WIN32
#define FW_API  __declspec(dllexport)
#define FW_CALL __cdecl
#else
#define FW_API
#define FW_CALL
#endif

typedef uint64_t fw_handle;

typedef const char* (FW_CALL* fw_get_last_error_fn)(void);
typedef void       (FW_CALL* fw_log_fn)(int level, const char* msg, void* user);

typedef struct fw_header {
    uint32_t              abi_version;
    fw_get_last_error_fn  get_last_error;
    void* log_cb;   // (fw_log_fn) optional
    void* log_user; // opaque
} fw_header;