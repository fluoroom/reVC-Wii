#include <cctype>
#include <cstdio>
#include <cstring>

#include <wiiuse/wpad.h>

#include "common.h"
#include "Frontend.h"
#include "Hud.h"
#include "Pad.h"
#include "PlayerPed.h"
#include "PlayerInfo.h"
#include "Text.h"
#include "WiiCheats.h"
#include "WiiTrace.h"

namespace
{

constexpr int kMaxCheats = 32;
constexpr int kMaxCheatLen = 40;

struct CheatList
{
	char words[kMaxCheats][kMaxCheatLen];
	int count;
	bool loaded;
};

CheatList s_startCheats;
CheatList s_ingameCheats;
bool s_startApplied;

FILE *
openCheatFile(const char *filename, char *opened, size_t openedSize)
{
	static const char *const dirs[] = {
		"sd:/apps/reVC", "usb:/apps/reVC", "usb2:/apps/reVC",
		"usb3:/apps/reVC", "usb4:/apps/reVC"
	};
	char path[192];

	const char *install = WiiInstallDirectory();
	if(install != nullptr && install[0] != '\0'){
		std::snprintf(path, sizeof(path), "%s/%s", install, filename);
		FILE *file = std::fopen(path, "r");
		if(file != nullptr){
			std::snprintf(opened, openedSize, "%s", path);
			return file;
		}
	}

	for(const char *directory : dirs){
		std::snprintf(path, sizeof(path), "%s/%s", directory, filename);
		FILE *file = std::fopen(path, "r");
		if(file != nullptr){
			std::snprintf(opened, openedSize, "%s", path);
			return file;
		}
	}

	return nullptr;
}

void
loadCheatList(CheatList &list, const char *filename, bool force)
{
	if(list.loaded && !force)
		return;
	list.loaded = true;
	list.count = 0;

	char opened[192];
	FILE *file = openCheatFile(filename, opened, sizeof(opened));
	if(file == nullptr){
		WiiTraceReport("WII cheats: no %s\n", filename);
		return;
	}
	WiiTraceReport("WII cheats: reading %s\n", opened);

	char line[128];
	while(list.count < kMaxCheats && std::fgets(line, sizeof(line), file)){
		char *start = line;
		while(*start == ' ' || *start == '\t')
			start++;
		if(*start == '\0' || *start == '#' || *start == '\n' || *start == '\r')
			continue;

		int length = 0;
		for(char *p = start; *p != '\0' && *p != '#' && *p != '\n' && *p != '\r'; p++){
			if(*p == ' ' || *p == '\t')
				break;
			if(length + 1 >= kMaxCheatLen)
				break;
			list.words[list.count][length++] = (char)std::toupper((unsigned char)*p);
		}
		if(length == 0)
			continue;
		list.words[list.count][length] = '\0';
		WiiTraceReport("WII cheats: %s queued %s\n", filename, list.words[list.count]);
		list.count++;
	}
	std::fclose(file);
}

void
applyCheatList(const CheatList &list)
{
	for(int i = 0; i < list.count; i++){
		std::memset(CPad::KeyBoardCheatString, ' ', sizeof(CPad::KeyBoardCheatString));
		for(int c = 0; list.words[i][c] != '\0'; c++)
			CPad::GetPad(0)->AddToPCCheatString(list.words[i][c]);
	}
	std::memset(CPad::KeyBoardCheatString, ' ', sizeof(CPad::KeyBoardCheatString));
}

bool
cheatComboJustPressed(void)
{
	const u32 held = WPAD_ButtonsHeld(WPAD_CHAN_0);
	const u32 down = WPAD_ButtonsDown(WPAD_CHAN_0);
	return (held & WPAD_BUTTON_1) != 0 && (held & WPAD_BUTTON_2) != 0
		&& (down & (WPAD_BUTTON_1 | WPAD_BUTTON_2)) != 0;
}

} // namespace

void
WiiCheatsProcess(void)
{
	if(FrontEndMenuManager.m_bGameNotLoaded){
		s_startApplied = false;
		return;
	}

	if(FindPlayerPed() == nil)
		return;

	loadCheatList(s_startCheats, "cheats.txt", false);
	if(!s_startApplied){
		if(s_startCheats.count > 0){
			applyCheatList(s_startCheats);
			WiiTraceReport("WII cheats: start applied %d\n", s_startCheats.count);
		}
		s_startApplied = true;
	}

	if(!cheatComboJustPressed())
		return;

	loadCheatList(s_ingameCheats, "cheats-ingame.txt", true);
	if(s_ingameCheats.count == 0)
		return;

	applyCheatList(s_ingameCheats);
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	WiiTraceReport("WII cheats: ingame applied %d\n", s_ingameCheats.count);
}
