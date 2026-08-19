#include <cctype>
#include <cstdio>
#include <cstring>

#include <ogc/pad.h>
#include <wiiuse/wpad.h>

#include "common.h"
#include "CutsceneMgr.h"
#include "Font.h"
#include "Frontend.h"
#include "Hud.h"
#include "Pad.h"
#include "PlayerInfo.h"
#include "Sprite2d.h"
#include "Text.h"
#include "Timer.h"
#include "WiiCheats.h"
#include "WiiTrace.h"

namespace
{

constexpr int kMaxCheats = 32;
constexpr int kMaxCheatLen = 40;
constexpr int kVisibleRows = 12;
constexpr uint32 kRepeatDelayMs = 350;
constexpr uint32 kRepeatRateMs = 80;
constexpr uint32 kAppliedFlashMs = 800;

struct CheatList
{
	char words[kMaxCheats][kMaxCheatLen];
	int count;
	bool loaded;
};

struct CheatDef
{
	const char *word;
	const char *label;
};

CheatList s_startCheats;
bool s_startApplied;

bool s_open;
int s_cursor;
int s_scroll;
int s_repeatDir;
uint32 s_repeatAt;
int s_appliedIndex = -1;
uint32 s_appliedAt;

const CheatDef kCatalog[] = {
	{ "ASPIRINE", "Full health" },
	{ "PRECIOUSPROTECTION", "Full armour" },
	{ "LEAVEMEALONE", "Clear wanted" },
	{ "YOUWONTTAKEMEALIVE", "Raise wanted" },
	{ "THUGSTOOLS", "Thug weapons" },
	{ "PROFESSIONALTOOLS", "Pro weapons" },
	{ "NUTTERTOOLS", "Nutter weapons" },
	{ "OURGODGIVENRIGHTTOBEARARMS", "Ped weapons" },
	{ "CHICKSWITHGUNS", "Girls with guns" },
	{ "PANZER", "Rhino" },
	{ "TRAVELINSTYLE", "Bloodring racer" },
	{ "GETTHEREQUICKLY", "Bloodring banger" },
	{ "GETTHEREFAST", "Sabre turbo" },
	{ "GETTHEREFASTINDEED", "Hotring A" },
	{ "GETTHEREAMAZINGLYFAST", "Hotring B" },
	{ "THELASTRIDE", "Romero" },
	{ "ROCKANDROLLCAR", "Love Fist car" },
	{ "RUBBISHCAR", "Trashmaster" },
	{ "BETTERTHANWALKING", "Caddy" },
	{ "APLEASANTDAY", "Sunny" },
	{ "ALOVELYDAY", "Extra sunny" },
	{ "ABITDRIEG", "Cloudy" },
	{ "CATSANDDOGS", "Rain" },
	{ "CANTSEEATHING", "Fog" },
	{ "LIFEISPASSINGMEBY", "Fast weather" },
	{ "ONSPEED", "Fast time" },
	{ "BOOOOOORING", "Slow time" },
	{ "GREENLIGHT", "Green lights" },
	{ "MIAMITRAFFIC", "Mad traffic" },
	{ "AHAIRDRESSERSCAR", "Pink cars" },
	{ "IWANTITPAINTEDBLACK", "Black cars" },
	{ "BIGBANG", "Blow up cars" },
	{ "FIGHTFIGHTFIGHT", "Riot" },
	{ "NOBODYLIKESME", "Everyone attacks" },
	{ "WHEELSAREALLINEED", "Wheels only" },
	{ "COMEFLYWITHME", "Flying cars" },
	{ "GRIPISEVERYTHING", "Sticky cars" },
#ifdef RESTORE_ALLCARSHELI_CHEAT
	{ "CARSAREHELI", "Cars are helis" },
#endif
#ifdef WALLCLIMB_CHEAT
	{ "SPIDERCAR", "Climbing cars" },
#endif
	{ "SEAWAYS", "Hover boats" },
	{ "AIRSHIP", "Flying boats" },
	{ "LOADSOFLITTLETHINGS", "Tiny RC cars" },
	{ "FANNYMAGNET", "Attract women" },
	{ "HOPINGIRL", "Women follow" },
	{ "STILLLIKEDRESSINGUP", "Random clothes" },
	{ "DEEPFRIEDMARSBARS", "Fat Tommy" },
	{ "PROGRAMMER", "Skinny Tommy" },
#ifdef KANGAROO_CHEAT
	{ "KANGAROO", "Super jump" },
#endif
	{ "LOOKLIKELANCE", "Look like Lance" },
	{ "IWANTBIGTITS", "Look like Candy" },
	{ "MYSONISALAWYER", "Look like Ken" },
	{ "ILOOKLIKEHILARY", "Look like Hillary" },
	{ "ROCKANDROLLMAN", "Look like Jezz" },
	{ "ONEARMEDBANDIT", "Look like Phil" },
	{ "IDONTHAVETHEMONEYSONNY", "Look like Sonny" },
	{ "FOXYLITTLETHING", "Look like Mercedes" },
	{ "WELOVEOURDICK", "Look like Dick" },
	{ "CHEATSHAVEBEENCRACKED", "Look like Diaz" },
	{ "ICANTTAKEITANYMORE", "Suicide" },
};

constexpr int kMenuCount = (int)ARRAY_SIZE(kCatalog);

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

void
applyOneCheat(const char *word)
{
	if(word == nullptr || word[0] == '\0')
		return;
	std::memset(CPad::KeyBoardCheatString, ' ', sizeof(CPad::KeyBoardCheatString));
	for(int c = 0; word[c] != '\0'; c++)
		CPad::GetPad(0)->AddToPCCheatString(word[c]);
	std::memset(CPad::KeyBoardCheatString, ' ', sizeof(CPad::KeyBoardCheatString));
}

void
clampScroll(void)
{
	if(kMenuCount <= 0){
		s_scroll = 0;
		s_cursor = 0;
		return;
	}
	if(s_cursor < 0)
		s_cursor = kMenuCount - 1;
	if(s_cursor >= kMenuCount)
		s_cursor = 0;
	if(s_cursor < s_scroll)
		s_scroll = s_cursor;
	if(s_cursor >= s_scroll + kVisibleRows)
		s_scroll = s_cursor - kVisibleRows + 1;
	if(s_scroll < 0)
		s_scroll = 0;
}

void
moveCursor(int delta)
{
	if(kMenuCount <= 0)
		return;
	s_cursor += delta;
	clampScroll();
}

bool
cheatComboJustPressed(void)
{
	const u32 held = WPAD_ButtonsHeld(WPAD_CHAN_0);
	const u32 down = WPAD_ButtonsDown(WPAD_CHAN_0);
	return (held & WPAD_BUTTON_1) != 0 && (held & WPAD_BUTTON_2) != 0
		&& (down & (WPAD_BUTTON_1 | WPAD_BUTTON_2)) != 0;
}

void
readHeldDown(bool &upHeld, bool &downHeld, bool &upDown, bool &downDown,
	bool &applyDown, bool &closeDown)
{
	const u32 wDown = WPAD_ButtonsDown(WPAD_CHAN_0);
	const u32 wHeld = WPAD_ButtonsHeld(WPAD_CHAN_0);
	const u16 gDown = PAD_ButtonsDown(PAD_CHAN0);
	const u16 gHeld = PAD_ButtonsHeld(PAD_CHAN0);

	const u32 upMask = WPAD_BUTTON_UP | WPAD_CLASSIC_BUTTON_UP;
	const u32 downMask = WPAD_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_DOWN;
	const u32 aMask = WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A;
	const u32 bMask = WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B;

	upHeld = (wHeld & upMask) != 0 || (gHeld & PAD_BUTTON_UP) != 0;
	downHeld = (wHeld & downMask) != 0 || (gHeld & PAD_BUTTON_DOWN) != 0;
	upDown = (wDown & upMask) != 0 || (gDown & PAD_BUTTON_UP) != 0;
	downDown = (wDown & downMask) != 0 || (gDown & PAD_BUTTON_DOWN) != 0;
	applyDown = (wDown & aMask) != 0 || (gDown & PAD_BUTTON_A) != 0;
	closeDown = (wDown & bMask) != 0 || (gDown & PAD_BUTTON_B) != 0;
}

void
openMenu(void)
{
	s_open = true;
	s_cursor = 0;
	s_scroll = 0;
	s_repeatDir = 0;
	s_appliedIndex = -1;
	CTimer::SetCodePause(true);
	WiiTraceReport("WII cheats: picker open (%d)\n", kMenuCount);
}

void
closeMenu(void)
{
	s_open = false;
	s_repeatDir = 0;
	CTimer::SetCodePause(false);
	WiiTraceReport("WII cheats: picker close\n");
}

void
printAscii(float x, float y, const char *text)
{
	wchar ws[128];
	AsciiToUnicode(text, ws);
	CFont::PrintString(x, y, ws);
}

} // namespace

bool
WiiCheatsMenuActive(void)
{
	return s_open;
}

void
WiiCheatsProcess(void)
{
	if(FrontEndMenuManager.m_bGameNotLoaded){
		if(s_open)
			closeMenu();
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

	if(s_open){
		if(cheatComboJustPressed()){
			closeMenu();
			return;
		}

		bool upHeld, downHeld, upDown, downDown, applyDown, closeDown;
		readHeldDown(upHeld, downHeld, upDown, downDown, applyDown, closeDown);

		if(closeDown){
			closeMenu();
			return;
		}

		const uint32 now = CTimer::GetTimeInMillisecondsPauseMode();
		if(upDown && !downDown){
			moveCursor(-1);
			s_repeatDir = -1;
			s_repeatAt = now + kRepeatDelayMs;
		}else if(downDown && !upDown){
			moveCursor(1);
			s_repeatDir = 1;
			s_repeatAt = now + kRepeatDelayMs;
		}else if(s_repeatDir < 0 && upHeld && !downHeld){
			if(now >= s_repeatAt){
				moveCursor(-1);
				s_repeatAt = now + kRepeatRateMs;
			}
		}else if(s_repeatDir > 0 && downHeld && !upHeld){
			if(now >= s_repeatAt){
				moveCursor(1);
				s_repeatAt = now + kRepeatRateMs;
			}
		}else{
			s_repeatDir = 0;
		}

		if(applyDown && s_cursor >= 0 && s_cursor < kMenuCount){
			applyOneCheat(kCatalog[s_cursor].word);
			CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
			s_appliedIndex = s_cursor;
			s_appliedAt = now;
			WiiTraceReport("WII cheats: applied %s\n", kCatalog[s_cursor].word);
		}
		return;
	}

	if(FrontEndMenuManager.m_bMenuActive)
		return;
	if(CCutsceneMgr::IsCutsceneProcessing())
		return;
	if(!cheatComboJustPressed())
		return;

	openMenu();
}

void
WiiCheatsRender(void)
{
	if(!s_open)
		return;

	CSprite2d::DrawRect(CRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT),
		CRGBA(0, 0, 0, 160));

	const float panelLeft = SCREEN_SCALE_X(70.0f);
	const float panelRight = SCREEN_SCALE_FROM_RIGHT(70.0f);
	const float panelTop = SCREEN_SCALE_Y(36.0f);
	const float panelBottom = SCREEN_SCALE_FROM_BOTTOM(36.0f);
	CSprite2d::DrawRect(CRect(panelLeft, panelTop, panelRight, panelBottom),
		CRGBA(16, 16, 24, 230));

	CFont::SetBackgroundOff();
	CFont::SetJustifyOff();
	CFont::SetCentreOff();
	CFont::SetRightJustifyOff();
	CFont::SetPropOn();
	CFont::SetWrapx(panelRight - SCREEN_SCALE_X(12.0f));
	CFont::SetDropShadowPosition(1);
	CFont::SetDropColor(CRGBA(0, 0, 0, 255));

	CFont::SetFontStyle(FONT_HEADING);
	CFont::SetScale(SCREEN_SCALE_X(0.7f), SCREEN_SCALE_Y(0.9f));
	CFont::SetColor(CRGBA(255, 220, 80, 255));
	printAscii(panelLeft + SCREEN_SCALE_X(16.0f), panelTop + SCREEN_SCALE_Y(8.0f),
		"CHEATS");

	CFont::SetFontStyle(FONT_STANDARD);
	CFont::SetScale(SCREEN_SCALE_X(0.42f), SCREEN_SCALE_Y(0.52f));
	CFont::SetColor(CRGBA(180, 180, 180, 255));
	printAscii(panelLeft + SCREEN_SCALE_X(16.0f), panelBottom - SCREEN_SCALE_Y(28.0f),
		"A apply   B close   D-Pad move");

	const float rowTop = panelTop + SCREEN_SCALE_Y(40.0f);
	const float rowH = SCREEN_SCALE_Y(22.0f);
	const uint32 now = CTimer::GetTimeInMillisecondsPauseMode();
	const int last = s_scroll + kVisibleRows;
	int row = 0;
	for(int i = s_scroll; i < kMenuCount && i < last; i++, row++){
		const float y = rowTop + row * rowH;
		const bool selected = (i == s_cursor);
		if(selected){
			CSprite2d::DrawRect(
				CRect(panelLeft + SCREEN_SCALE_X(8.0f), y - SCREEN_SCALE_Y(2.0f),
					panelRight - SCREEN_SCALE_X(8.0f), y + rowH - SCREEN_SCALE_Y(4.0f)),
				CRGBA(80, 50, 10, 220));
		}

		char line[96];
		std::snprintf(line, sizeof(line), "%s%s", selected ? "> " : "  ",
			kCatalog[i].label);
		if(i == s_appliedIndex && now - s_appliedAt < kAppliedFlashMs)
			CFont::SetColor(CRGBA(120, 255, 120, 255));
		else if(selected)
			CFont::SetColor(CRGBA(255, 255, 180, 255));
		else
			CFont::SetColor(CRGBA(220, 220, 220, 255));
		printAscii(panelLeft + SCREEN_SCALE_X(16.0f), y, line);
	}
}
