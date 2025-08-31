#pragma once
#include "c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct fw_renderer_desc {
        void* hwnd; // HWND on Windows
    } fw_renderer_desc;

    // ABI v3
    typedef struct fw_renderer_api {
        fw_header hdr;

        void (FW_CALL* set_logger)(void* cb, void* user);

        // device-aware screen-space/NDC line upload
        int  (FW_CALL* lines_upload)(fw_handle dev,
            const float* xy, uint32_t count,
            float r, float g, float b, float a);

        int  (FW_CALL* create_device)(const fw_renderer_desc*, fw_handle* out);
        void (FW_CALL* destroy_device)(fw_handle dev);
        void (FW_CALL* begin_frame)(fw_handle dev);
        void (FW_CALL* end_frame)(fw_handle dev);
    } fw_renderer_api;

    FW_API void* FW_CALL fwGetRendererAPI(uint32_t requested_abi);

#ifdef __cplusplus
} // extern "C"
#endif