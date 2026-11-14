#pragma once
#include "plugin/lyra_plugin.h"
#include <string>
bool font_platform_ready(const LyraHostV3*);
std::string font_storage(const LyraHostV3*);
int font_pick(const LyraHostV3*,const char*,const char*);
int font_pick_result(const LyraHostV3*);
