// reColossus: the screen between the host's raylib subset (ps5/raylib/raylib_ps5.cpp) and the Vulkan renderer
// (ps5/vk/native_renderer_vk.cpp). The renderer presents to the display itself once its swapchain is up
// (rc_screen_gpu_owns); the host then draws only an overlay (the fps line, the settings menu), which the renderer
// lays over its picture.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
void rc_screen_gpu_owns(void);
int rc_screen_overlay_size(int *width, int *height);
// The overlay's region that changed since the last call, into dst (rows of x1 - x0 pixels, B8G8R8A8, straight
// alpha); 1 when there is one.
int rc_screen_overlay_take(void *dst, size_t capacity, int *x0, int *y0, int *x1, int *y1);
unsigned long long rc_screen_overlay_serial(void);
// The picture the host drew last (the emulated GS's: the boot, the films), when newer than *serial: R8G8B8A8 rows,
// top down; 1 when there is one (*serial then its own).
int rc_screen_picture_take(void *dst, size_t capacity, int *width, int *height, unsigned long long *serial);
#ifdef __cplusplus
}
#endif
