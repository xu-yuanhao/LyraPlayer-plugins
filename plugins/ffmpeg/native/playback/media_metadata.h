#pragma once
#include <string>
// Ownership transfers to the metadata reader, which closes the fd exactly once.
struct EmbeddedTags {
    std::string title;
    std::string artist;
    std::string album;
    std::string genre;
    std::string lyrics;
};
EmbeddedTags read_embedded_tags(int fd);
std::string read_embedded_lyrics(int fd);
