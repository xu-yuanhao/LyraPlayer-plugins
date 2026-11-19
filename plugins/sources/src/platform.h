#pragma once
#include "plugin/lyra_plugin.h"
#include <string>
#include <string_view>
namespace sources {
bool initialize(const LyraHostV3 *);
void disconnect();
std::string files_directory();
bool choose_document();
int take_document();
std::string read_file(int fd,size_t limit);
std::string network(std::string_view url,std::string_view method,
                    std::string_view headers,std::string_view body);
std::string icon_tile(std::string_view url);
}
