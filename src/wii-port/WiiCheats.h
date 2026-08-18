#ifndef WIICHEATS_H
#define WIICHEATS_H

// usb:/apps/reVC after chdir at boot.  FileMgr then SetDir("DATA"), so cheats
// must be opened with this prefix rather than a bare filename.
const char *WiiInstallDirectory(void);

// cheats.txt runs once when Tommy exists.  cheats-ingame.txt runs on Wiimote 1+2.
void WiiCheatsProcess(void);

#endif
