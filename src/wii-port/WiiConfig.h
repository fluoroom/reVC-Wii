#ifndef WIICONFIG_H
#define WIICONFIG_H

// config.txt next to boot.dol, same place as cheats.txt.
// Read once after the install directory is known.  Missing file keeps 16:9.
void WiiConfigLoad(void);
bool WiiConfigWantWide(void);

#endif
