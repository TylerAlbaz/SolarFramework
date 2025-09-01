#pragma once
#include "c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

    // Minimal device creation info (current native only needs HWND)
    typedef struct fw_renderer_desc {
        void* hwnd; // HWND on Windows
    } fw_renderer_desc;

    // Full function table returned by fmGetRendererAPI
    typedef struct fw_renderer_api {
        fw_header hdr;

        // Logging
        void (FM_CALL* set_logger)(void* cb /*fw_log_fn*/, void* user);

        // Device lifetime
        int  (FM_CALL* create_device)(const fw_renderer_desc* desc, fw_handle* out_dev);
        void (FM_CALL* destroy_device)(fw_handle dev);

        // Per-frame
        void (FM_CALL* begin_frame)(fw_handle dev);
        void (FM_CALL* end_frame)  (fw_handle dev);

        // Demo draw path: upload an array of NDC line vertices [x0,y0, x1,y1, ...]
        int  (FM_CALL* lines_upload)(fw_handle dev, const float* xy, uint32_t count,
            float r, float g, float b, float a);
    } fw_renderer_api;

    // Single exported function that returns &fw_renderer_api
    FM_API void* FM_CALL fmGetRendererAPI(uint32_t requested_abi);

#ifdef __cplusplus
} // extern "C"
#endif
