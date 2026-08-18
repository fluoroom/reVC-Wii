#ifndef WIIVIDEO_H
#define WIIVIDEO_H

// Wii framebuffer stays 640x480; this is true when analog output is filled
// to 720 and the game should use 16:9 FOV (anamorphic widescreen).
bool WiiVideoIsWide(void);

#endif
