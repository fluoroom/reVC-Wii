#include <cctype>
#include <cstdio>
#include <cstring>

#include "WiiCheats.h"
#include "WiiConfig.h"
#include "WiiTrace.h"

namespace
{

bool s_wantWide = true;
WiiConfigVideo s_wantVideo = WiiConfigVideoAuto;

void
trim(char *&start, char *&end)
{
	while(*start == ' ' || *start == '\t')
		start++;
	end = start + std::strlen(start);
	while(end > start && (end[-1] == ' ' || end[-1] == '\t' ||
		end[-1] == '\n' || end[-1] == '\r'))
		*--end = '\0';
}

void
lowerCopy(char *out, size_t outSize, const char *in)
{
	size_t i = 0;
	for(; in[i] != '\0' && i + 1 < outSize; i++)
		out[i] = (char)std::tolower((unsigned char)in[i]);
	out[i] = '\0';
}

bool
parseWideValue(const char *value, bool &wide)
{
	char lowered[32];
	lowerCopy(lowered, sizeof(lowered), value);

	if(std::strcmp(lowered, "16:9") == 0 ||
	   std::strcmp(lowered, "16/9") == 0 ||
	   std::strcmp(lowered, "16x9") == 0 ||
	   std::strcmp(lowered, "widescreen") == 0 ||
	   std::strcmp(lowered, "wide") == 0)
		wide = true;
	else if(std::strcmp(lowered, "4:3") == 0 ||
		std::strcmp(lowered, "4/3") == 0 ||
		std::strcmp(lowered, "4x3") == 0 ||
		std::strcmp(lowered, "standard") == 0 ||
		std::strcmp(lowered, "fullscreen") == 0)
		wide = false;
	else
		return false;
	return true;
}

bool
parseVideoValue(const char *value, WiiConfigVideo &video)
{
	char lowered[32];
	lowerCopy(lowered, sizeof(lowered), value);

	if(std::strcmp(lowered, "pal") == 0 ||
	   std::strcmp(lowered, "pal50") == 0 ||
	   std::strcmp(lowered, "50hz") == 0 ||
	   std::strcmp(lowered, "50") == 0 ||
	   std::strcmp(lowered, "576") == 0 ||
	   std::strcmp(lowered, "576p") == 0 ||
	   std::strcmp(lowered, "576i") == 0)
		video = WiiConfigVideoPal;
	else if(std::strcmp(lowered, "ntsc") == 0 ||
		std::strcmp(lowered, "60hz") == 0 ||
		std::strcmp(lowered, "60") == 0 ||
		std::strcmp(lowered, "480") == 0 ||
		std::strcmp(lowered, "480p") == 0 ||
		std::strcmp(lowered, "480i") == 0)
		video = WiiConfigVideoNtsc;
	else if(std::strcmp(lowered, "auto") == 0 ||
		std::strcmp(lowered, "sysconf") == 0 ||
		std::strcmp(lowered, "default") == 0 ||
		std::strcmp(lowered, "hbc") == 0)
		video = WiiConfigVideoAuto;
	else
		return false;
	return true;
}

const char *
videoName(WiiConfigVideo video)
{
	switch(video){
	case WiiConfigVideoPal:
		return "PAL";
	case WiiConfigVideoNtsc:
		return "NTSC";
	default:
		return "auto";
	}
}

FILE *
openConfigFile(char *opened, size_t openedSize)
{
	static const char *const dirs[] = {
		"sd:/apps/reVC", "usb:/apps/reVC", "usb2:/apps/reVC",
		"usb3:/apps/reVC", "usb4:/apps/reVC"
	};
	char path[192];

	const char *install = WiiInstallDirectory();
	if(install != nullptr && install[0] != '\0'){
		std::snprintf(path, sizeof(path), "%s/config.txt", install);
		FILE *file = std::fopen(path, "r");
		if(file != nullptr){
			std::snprintf(opened, openedSize, "%s", path);
			return file;
		}
	}

	for(const char *directory : dirs){
		std::snprintf(path, sizeof(path), "%s/config.txt", directory);
		FILE *file = std::fopen(path, "r");
		if(file != nullptr){
			std::snprintf(opened, openedSize, "%s", path);
			return file;
		}
	}

	return nullptr;
}

} // namespace

void
WiiConfigLoad(void)
{
	s_wantWide = true;
	s_wantVideo = WiiConfigVideoAuto;

	char opened[192];
	FILE *file = openConfigFile(opened, sizeof(opened));
	if(file == nullptr){
		WiiTraceReport("WII config: no config.txt, aspect=16:9 video=auto\n");
		return;
	}
	WiiTraceReport("WII config: reading %s\n", opened);

	char line[128];
	while(std::fgets(line, sizeof(line), file)){
		char *start = line;
		char *end = line;
		trim(start, end);
		if(*start == '\0' || *start == '#')
			continue;

		char *comment = start;
		while(*comment != '\0' && *comment != '#')
			comment++;
		if(*comment == '#')
			*comment = '\0';
		trim(start, end);

		char *sep = start;
		while(*sep != '\0' && *sep != '=' && *sep != ' ' && *sep != '\t')
			sep++;
		if(*sep == '\0')
			continue;
		*sep = '\0';
		char *value = sep + 1;
		while(*value == ' ' || *value == '\t' || *value == '=')
			value++;
		trim(start, end);
		char *valueEnd = value + std::strlen(value);
		trim(value, valueEnd);

		char key[32];
		lowerCopy(key, sizeof(key), start);
		if(std::strcmp(key, "aspect") == 0 ||
		   std::strcmp(key, "widescreen") == 0 ||
		   std::strcmp(key, "ratio") == 0){
			bool wide;
			if(!parseWideValue(value, wide)){
				WiiTraceReport("WII config: ignored aspect=%s\n", value);
				continue;
			}
			s_wantWide = wide;
			WiiTraceReport("WII config: aspect=%s\n", wide ? "16:9" : "4:3");
			continue;
		}
		if(std::strcmp(key, "video") == 0 ||
		   std::strcmp(key, "videomode") == 0 ||
		   std::strcmp(key, "tv") == 0){
			WiiConfigVideo video;
			if(!parseVideoValue(value, video)){
				WiiTraceReport("WII config: ignored video=%s\n", value);
				continue;
			}
			s_wantVideo = video;
			WiiTraceReport("WII config: video=%s\n", videoName(video));
		}
	}
	std::fclose(file);
}

bool
WiiConfigWantWide(void)
{
	return s_wantWide;
}

WiiConfigVideo
WiiConfigWantVideo(void)
{
	return s_wantVideo;
}
