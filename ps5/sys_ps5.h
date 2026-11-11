/* reColossus: the PS5 system library functions the host calls (libkernel, libSceVideoOut, libScePad,
 * libSceUserService, libSceAudioOut, libSceSystemService). Prototypes as reUndr-ps5 and reKing declare them. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* libkernel */
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type, int64_t *physical);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t physical, size_t align);
int sceKernelReleaseDirectMemory(int64_t physical, size_t len);
int sceKernelSendNotificationRequest(int device, void *request, size_t size, int blocking);
int sceKernelDebugOutText(int channel, const char *text);
int sceKernelUsleep(unsigned microseconds);
int scePthreadSetaffinity(unsigned long thread, unsigned long mask);

/* libSceVideoOut */
typedef struct { void *address, *metadata, *reserved[2]; } SceVideoOutBuffer;
typedef struct { uint8_t reserved[80]; } SceVideoOutAttribute;
int sceVideoOutOpen(int user, int bus, int index, const void *param);
int sceVideoOutClose(int handle);
int sceVideoOutWaitVblank(int handle);
int sceVideoOutSubmitFlip(int handle, int index, uint32_t mode, int64_t arg);
int sceVideoOutSetFlipRate(int handle, int rate);
void sceVideoOutSetBufferAttribute2(SceVideoOutAttribute *attr, uint64_t format, uint32_t tiling, uint32_t width,
                                    uint32_t height, uint64_t option, uint32_t dcc_control, uint64_t dcc_cb);
int sceVideoOutRegisterBuffers2(int handle, int set, int start, SceVideoOutBuffer *buffers, int count,
                                SceVideoOutAttribute *attr, int category, void *option);

/* libSceSystemService */
int sceSystemServiceHideSplashScreen(void);

/* libSceUserService */
int sceUserServiceInitialize(const void *param);
int sceUserServiceGetInitialUser(int *user);

/* libScePad */
int scePadInit(void);
int scePadOpen(int user, int type, int index, const void *param);
int scePadGetHandle(int user, int type, int index);
int scePadClose(int handle);
int scePadReadState(int handle, void *data);
int scePadSetVibration(int handle, const void *motors);
int scePadSetVibrationMode(int handle, int mode);

/* libSceAudioOut */
int sceAudioOutInit(void);
int sceAudioOutOpen(int user, int type, int index, unsigned grain, unsigned rate, unsigned format);
int sceAudioOutOutput(int handle, const void *samples);
int sceAudioOutSetVolume(int handle, int channel_mask, const int *volumes);

#ifdef __cplusplus
}
#endif
