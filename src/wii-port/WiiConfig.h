#ifndef WIICONFIG_H
#define WIICONFIG_H

// config.txt next to boot.dol, same place as cheats.txt.
// Read once after the install directory is known.  Missing file keeps 16:9
// and follows SYSCONF video the same way HBC does (VIDEO_GetPreferredMode).
void WiiConfigLoad(void);
bool WiiConfigWantWide(void);

enum WiiConfigVideo {
	WiiConfigVideoAuto = 0,
	WiiConfigVideoPal,
	WiiConfigVideoNtsc
};

WiiConfigVideo WiiConfigWantVideo(void);

#endif
