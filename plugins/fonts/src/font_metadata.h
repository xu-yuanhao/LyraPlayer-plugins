#pragma once
#include <string>
#include <cstdint>
struct FontMetadata { std::string family; int weight=0; bool variable=false; };
bool read_font_metadata(int fd,FontMetadata *out);
