#include "ui/app_font.h"
#include "utils/asset_manager.h"
#include "utils/logger.h"
#include <src/font/lv_font.h>
#include <src/misc/lv_types.h>

#if LV_USE_FREETYPE
#include "lvgl/src/libs/freetype/lv_freetype.h"
#endif

#include <fmt/ranges.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ui::font {
namespace {

constexpr const char* INTER_REGULAR_FILE  = "inter-regular.ttf";
constexpr const char* INTER_MEDIUM_FILE   = "inter-medium.ttf";
constexpr const char* INTER_SEMIBOLD_FILE = "inter-semibold.ttf";
constexpr const char* INTER_BOLD_FILE     = "inter-bold.ttf";
constexpr int32_t DEFAULT_FONT_SIZE = 16;

struct FontSlot {
    InterWeight weight;
    int32_t size;
    lv_font_t* font;
    std::string path;
};

std::array<FontSlot, 8> font_slots{};
std::array<FontSlot, 4> keyboard_icon_slots{};
std::array<FontSlot, 4> camera_icon_slots{};
bool initialized = false;

const char* file_for_weight(InterWeight weight)
{
    switch (weight) {
        case InterWeight::Medium:   return INTER_MEDIUM_FILE;
        case InterWeight::SemiBold: return INTER_SEMIBOLD_FILE;
        case InterWeight::Bold:     return INTER_BOLD_FILE;
        case InterWeight::Regular:
        default:                    return INTER_REGULAR_FILE;
    }
}

std::vector<std::string> asset_paths_for_weight(InterWeight weight)
{
    const std::string file = file_for_weight(weight);
    return {"fonts/" + file, file};
}

lv_font_t* create_freetype_font(const std::vector<std::string>& asset_paths,
                                int32_t size,
                                std::string& path)
{
#if LV_USE_FREETYPE
    path.clear();
    const auto candidate_paths = asset::AssetManager::candidate_paths(asset_paths);
    const auto existing_paths = asset::AssetManager::existing_paths(asset_paths);
    for (const auto& candidate_path : existing_paths) {
        path = candidate_path;
        lv_font_t* font = lv_freetype_font_create(path.c_str(),
                                                  LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                                  static_cast<uint32_t>(size),
                                                  LV_FREETYPE_FONT_STYLE_NORMAL);
        if (!font) {
            continue;
        }

        path = candidate_path;
        LOG_DEBUG("Created FreeType font from asset: {}", path);
        return font;
    }

    path.clear();
    if (existing_paths.empty()) {
        LOG_WARN("FreeType font asset not found. Tried: {}", fmt::join(candidate_paths, ", "));
    } else {
        LOG_WARN("Failed to load FreeType font. Readable candidates: {}", fmt::join(existing_paths, ", "));
    }
    return nullptr;
#else
    (void)asset_paths;
    (void)size;
    (void)path;
    LOG_WARN("LV_USE_FREETYPE is disabled; using default LVGL font");
    return nullptr;
#endif
}

lv_font_t* fallback_font()
{
    return const_cast<lv_font_t*>(&lv_font_montserrat_14);
}

} // namespace

void AppFont::init()
{
    if (initialized) {
        return;
    }

    font_slots = {};
    keyboard_icon_slots = {};
    camera_icon_slots = {};
    initialized = true;
}

void AppFont::deinit()
{
#if LV_USE_FREETYPE
    auto delete_font = [](FontSlot& slot) {
        if (slot.font) {
            lv_freetype_font_delete(slot.font);
        }
        slot.font = nullptr;
        slot.path.clear();
    };

    for (auto& slot : keyboard_icon_slots) {
        delete_font(slot);
    }

    for (auto& slot : camera_icon_slots) {
        delete_font(slot);
    }

    for (auto& slot : font_slots) {
        delete_font(slot);
    }
#endif
    initialized = false;
}

lv_font_t* AppFont::inter(InterWeight weight, int32_t size)
{
    init();

    if (size <= 0) {
        size = DEFAULT_FONT_SIZE;
    }

    for (auto& slot : font_slots) {
        if (slot.font && slot.weight == weight && slot.size == size) {
            return slot.font;
        }
    }

    for (auto& slot : font_slots) {
        if (!slot.font) {
            slot.weight = weight;
            slot.size = size;
            slot.font = create_freetype_font(asset_paths_for_weight(weight), size, slot.path);
            return slot.font ? slot.font : fallback_font();
        }
    }

    LOG_WARN("Inter font cache is full; using default LVGL font");
    return fallback_font();
}

lv_font_t* AppFont::keyboard_icons(int32_t size)
{
    init();
    if (size <= 0) {
        size = DEFAULT_FONT_SIZE;
    }

    for (auto& slot : keyboard_icon_slots) {
        if (slot.font && slot.size == size) {
            return slot.font;
        }
    }

    for (auto& slot : keyboard_icon_slots) {
        if (slot.font) {
            continue;
        }

        const std::vector<std::string> asset_paths = {
            "fonts/kenney_input_keyboard_and_mouse.ttf",
            "kenney_input_keyboard_and_mouse.ttf",
        };

        slot.size = size;
        slot.font = create_freetype_font(asset_paths, size, slot.path);
        if (!slot.font) {
            return fallback_font();
        }

        return slot.font;
    }

    LOG_WARN("Keyboard icon font cache is full; using default LVGL font");
    return fallback_font();
}


lv_font_t* AppFont::camera_icons(int32_t size)
{
    init();
    if (size <= 0) {
        size = DEFAULT_FONT_SIZE;
    }

    for (auto& slot : camera_icon_slots) {
        if (slot.font && slot.size == size) {
            return slot.font;
        }
    }

    for (auto& slot : camera_icon_slots) {
        if (slot.font) {
            continue;
        }

        const std::vector<std::string> asset_paths = {
            "fonts/camera_icons.ttf",
            "camera_icons.ttf",
        };

        slot.size = size;
        slot.font = create_freetype_font(asset_paths, size, slot.path);
        if (!slot.font) {
            return fallback_font();
        }

        return slot.font;
    }

    LOG_WARN("Camera icon font cache is full; using default LVGL font");
    return fallback_font();
}

} // namespace ui::font
