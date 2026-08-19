#ifndef WIICHEATS_H
#define WIICHEATS_H

// usb:/apps/reVC after chdir at boot.  FileMgr then SetDir("DATA"), so cheats
// must be opened with this prefix rather than a bare filename.
const char *WiiInstallDirectory(void);

// cheats.txt runs once when Tommy exists.  Wiimote 1+2 opens an in-game picker
// (CodePause) over the built-in cheat list.
void WiiCheatsProcess(void);
void WiiCheatsRender(void);
bool WiiCheatsMenuActive(void);

#endif
