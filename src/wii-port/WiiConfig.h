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

// Driving with the remote held sideways, like a Wii Wheel.  config.txt
// "wheel=on" seeds this at boot.  The controller menu writes the same byte
// and reVC.ini remembers it after that, which then wins over config.txt.
bool WiiConfigWheelDrive(void);
signed char *WiiConfigWheelDriveSlot(void);
void WiiConfigClampWheelDrive(void);

#endif
