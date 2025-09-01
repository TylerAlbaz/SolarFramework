#pragma once
#include "c_api.h"

/*
    Minimal renderer module API (Vulkan-backed in your implementation).
    One function exports the table: fmGetRendererAPI(FM_ABI_VERSION).

    Notes:
    - All functions are thread-affine to the "render thread" you choose.
    - No HWND lifetime management is done by the module; you pass HWND.
    - Resize / present are explicit; end_frame does NOT implicitly present
      so you can choose your pacing strategy.
*/

#ifdef __cplusplus
extern "C" {
#endif

    /* Creation/Swapchain description */
    typedef struct fm_renderer_desc {
        void* hwnd;              /* HWND on Windows */
        uint32_t width;
        uint32_t height;
        uint32_t enable_validation; /* 0/1 */
        uint32_t vsync;             /* 0/1 */
    } fm_renderer_desc;

    /* Function table for ABI v3 */
    typedef struct fm_renderer_api {
        fm_header hdr;  /* must be first; abi_version == FM_ABI_VERSION */

        /* -------- diagnostics -------- */
        /* Install a logger (optional). Pass NULL to clear. */
        void (FM_CALL* set_logger)(fm_log_fn cb, void* user);

        /* -------- device / swapchain -------- */
        int  (FM_CALL* create_device)(const fm_renderer_desc* desc, fm_handle* out_dev);
        void (FM_CALL* destroy_device)(fm_handle dev);
        int  (FM_CALL* resize_swapchain)(fm_handle dev, uint32_t width, uint32_t height);

        /* -------- per-frame control -------- */
        /* Begin a frame and clear the backbuffer. */
        int  (FM_CALL* begin_frame)(fm_handle dev, float clear_r, float clear_g, float clear_b, float clear_a);
        /* End recording; does NOT present. */
        int  (FM_CALL* end_frame)(fm_handle dev);
        /* Present the current swapchain image. */
        int  (FM_CALL* present)(fm_handle dev);

        /* -------- camera / matrices -------- */
        /* Set camera-relative matrices. view3x4 = 3x4 row-major (rotation + translation),
           proj4x4 = 4x4 row-major, origin3 = world origin in doubles. */
        int  (FM_CALL* set_matrices)(fm_handle dev,
            const double* view3x4, /* 12 doubles */
            const double* proj4x4, /* 16 doubles */
            const double* origin3  /* 3 doubles  */);

        /* -------- immediate line drawing (NDC or camera-relative) --------
           Draw a line strip from 'count' 3D vertices. If you pass positions that
           are already camera-relative (worldPos - origin), set_matrices can be
           identity and the shader will just transform. */
        int  (FM_CALL* draw_lines)(fm_handle dev,
            const float* xyz,  /* 3*count floats */
            uint32_t     count,
            float        r, float g, float b, float a,
            float        line_width_pixels);
    } fm_renderer_api;

    /* Exported symbol that returns the function table for the requested ABI.
       Returns NULL if the requested ABI is not supported. */
    FM_API void* FM_CALL fmGetRendererAPI(uint32_t requested_abi);

#ifdef __cplusplus
} // extern "C"
#endif