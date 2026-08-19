#ifndef WIIVIDEO_H
#define WIIVIDEO_H

// Analog always fills the 720-wide NTSC/PAL line.  The EFB stays 640x480;
// PAL 50Hz Y-scales that into a 576-line XFB.  When this is true the game is
// anamorphic 16:9 (HOR+ FOV, wide HUD).  When false it is packed 4:3
// (original FOV and HUD) for a 4:3 TV that still wants the analog line filled.
bool WiiVideoIsWide(void);
bool WiiVideoDefaultIsWide(void);
void WiiVideoSetWide(bool wide);
void WiiVideoSyncGamePref(void);

#endif
