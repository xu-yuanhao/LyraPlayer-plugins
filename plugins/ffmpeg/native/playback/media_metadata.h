#pragma once
#include <string>
// Ownership transfers to the metadata reader, which closes the fd exactly once.
std::string read_embedded_lyrics(int fd);
