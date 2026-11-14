#pragma once
#include <cstdint>
#include <array>
#include <string>
#include <vector>
#include <iterator>
namespace lyra::custom_fonts {
enum Role {FSheetRowSp,FButtonTextSp,FTitleSp,FArtistSp,FDurationSp,FHeaderSp,FRowTitleSp,FRowMetaSp,FLyricMainMiniSp,FLyricMainExpandedSp,FLyricTranslationMiniSp,FLyricTranslationExpandedSp,FLyricPhoneticSp,FLyricBgSp,FLyricBgMiniSp,FLyricBgTranslationSp,FLyricBgTranslationMiniSp,FLyricRubySp,FImmersiveTitleSp,FImmersiveArtistSp,FImmersiveTimeSp,FAboutNameSp,FAboutSloganSp,FAboutVersionSp,FSupportProductSp,FSupportBodySp,FSupportAmountSp,FPersonalHintSp,FListeningTextSp};
inline constexpr const char *size_keys[]={"typography.sizes.sheet_row_sp","typography.sizes.button_text_sp","typography.sizes.title_sp","typography.sizes.artist_sp","typography.sizes.duration_sp","typography.sizes.header_sp","typography.sizes.row_title_sp","typography.sizes.row_meta_sp","typography.sizes.lyric_main_mini_sp","typography.sizes.lyric_main_expanded_sp","typography.sizes.lyric_translation_mini_sp","typography.sizes.lyric_translation_expanded_sp","typography.sizes.lyric_phonetic_sp","typography.sizes.lyric_bg_sp","typography.sizes.lyric_bg_mini_sp","typography.sizes.lyric_bg_translation_sp","typography.sizes.lyric_bg_translation_mini_sp","typography.sizes.lyric_ruby_sp","typography.sizes.immersive_title_sp","typography.sizes.immersive_artist_sp","typography.sizes.immersive_time_sp","typography.sizes.about_name_sp","typography.sizes.about_slogan_sp","typography.sizes.about_version_sp","typography.sizes.support_product_sp","typography.sizes.support_body_sp","typography.sizes.support_amount_sp","typography.sizes.personal_hint_sp","typography.sizes.listening_text_sp"};
inline constexpr const char *bold_keys[]={"typography.bold.sheet_row_sp","typography.bold.button_text_sp","typography.bold.title_sp","typography.bold.artist_sp","typography.bold.duration_sp","typography.bold.header_sp","typography.bold.row_title_sp","typography.bold.row_meta_sp","typography.bold.lyric_main_mini_sp","typography.bold.lyric_main_expanded_sp","typography.bold.lyric_translation_mini_sp","typography.bold.lyric_translation_expanded_sp","typography.bold.lyric_phonetic_sp","typography.bold.lyric_bg_sp","typography.bold.lyric_bg_mini_sp","typography.bold.lyric_bg_translation_sp","typography.bold.lyric_bg_translation_mini_sp","typography.bold.lyric_ruby_sp","typography.bold.immersive_title_sp","typography.bold.immersive_artist_sp","typography.bold.immersive_time_sp","typography.bold.about_name_sp","typography.bold.about_slogan_sp","typography.bold.about_version_sp","typography.bold.support_product_sp","typography.bold.support_body_sp","typography.bold.support_amount_sp","typography.bold.personal_hint_sp","typography.bold.listening_text_sp"};
inline constexpr auto count=std::size(size_keys);
// One control per user-facing purpose; stored roles remain stable for old
// configuration files and for the text producers' existing style identities.
inline constexpr Role controls[]={FRowTitleSp,FTitleSp,FArtistSp,FDurationSp,
    FLyricMainMiniSp,FLyricMainExpandedSp};
inline constexpr Role control_role(Role role){
    switch(role){
    case FTitleSp:case FImmersiveTitleSp:return FTitleSp;
    case FArtistSp:case FImmersiveArtistSp:return FArtistSp;
    case FDurationSp:case FImmersiveTimeSp:return FDurationSp;
    case FLyricMainMiniSp:case FLyricTranslationMiniSp:case FLyricBgMiniSp:case FLyricBgTranslationMiniSp:return FLyricMainMiniSp;
    case FLyricMainExpandedSp:case FLyricTranslationExpandedSp:case FLyricPhoneticSp:case FLyricBgSp:case FLyricBgTranslationSp:case FLyricRubySp:return FLyricMainExpandedSp;
    default:return FRowTitleSp;
    }
}
struct Settings {
    std::string family, import_mime_types, preferred_families;
    std::array<double,count> sizes{};
    std::array<bool,count> bold{}, default_bold{};
    bool modified=false,follow_system=false,follow_parameters=false;
    std::array<double,count> minimum{},maximum{};
    double minimum_ratio=0,maximum_ratio=0;
    double minimum_size_sp=0,maximum_size_sp=0,import_max_bytes=0,import_timeout_ms=0,import_limit=0,recommended_limit=0;
};
struct Choice { std::string id,label,spec; bool user=false,preferred=false; };
struct View { std::vector<Choice> choices; std::string automatic; uint64_t revision=1; };
}
