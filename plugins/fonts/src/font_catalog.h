#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <pthread.h>
#include "font_settings.h"

namespace lyra {

// Picks one clean type family from the fonts installed on the device.
// The result is a descriptor for the Java side: "lyra:<weight>=<path>|...".
// Decorative, monospace and script variants are never selected.
class ManagedFontCatalog {
  public:
    ~ManagedFontCatalog();
    struct InstalledFont {
        std::string path;
        std::string name;
    };

    // Scans the platform font directories once. Safe to call more than once.
    void scan();
    // Rebuilds the descriptor for the four weights this app uses.
    void configure(int regular, int light, int bold, int heavy);
    // Writes the current descriptor into out. Returns the byte count.
    int supply(int64_t id, char *out, int cap) const;
    const std::string &spec() const { return spec_; }
    bool scanned() const { return scanned_; }
    void user_root(std::string root);
    void preferences(const custom_fonts::Settings &settings);
    const custom_fonts::View &view() const { return view_; }
    const custom_fonts::Choice *choice(const std::string &id) const;
    bool picking=false;
    bool import_fd(int fd,const custom_fonts::Settings &,void *user,void (*wake)(void*));
    enum ImportResult { ImportInvalid=-1,ImportLimit=-2 };
    int finish_import();
    bool remove(const std::string &id);
    void join();
    static bool clean_family(const std::string &);
    void scan_directories(const std::vector<std::string> &directories);


  private:
    void build_choices(int regular,int light,int bold,int heavy);
    static void *import_worker(void *);
    struct Import;
    Import *import_=nullptr;
    pthread_t thread_{};
    std::atomic<int> result_{0};
    bool started_=false;
    std::string root_,requested_="auto",automatic_spec_;
    custom_fonts::View view_;
    int regular_=0,light_=0,bold_=0,heavy_=0;
    size_t choice_limit_=0;
    bool choices_dirty_=true;
    custom_fonts::Settings settings_;
    bool scanned_ = false;
    std::vector<InstalledFont> files_;
    std::string spec_ = "sans-serif";
    std::string family_;
};

} // namespace lyra
