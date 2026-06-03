#include "views/gallery_view.h"

#include "ui/app_color.h"
#include "ui/app_font.h"

#include <algorithm>
#include <filesystem>
#include <src/core/lv_obj.h>
#include <src/core/lv_obj_pos.h>
#include <src/core/lv_obj_style.h>
#include <src/core/lv_obj_style_gen.h>
#include <src/core/lv_observer.h>
#include <src/draw/lv_image_decoder.h>
#include <src/layouts/flex/lv_flex.h>
#include <src/misc/lv_area.h>
#include <src/misc/lv_color.h>
#include <src/widgets/image/lv_image.h>
#include <src/widgets/label/lv_label.h>

namespace view {

using Font = ui::font::AppFont;
namespace color = ui::color;
namespace font = ui::font;

namespace {

constexpr int32_t kBottomBarHeight  = 36;
constexpr int32_t kActionLiftY      = -9;
constexpr int32_t kGuideLineHeight  = 7;
constexpr int32_t kPreviewFitWidth  = 320;
constexpr int32_t kPreviewFitHeight = 170;

void style_transparent_container(lv_obj_t* obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void gallery_image_observer_cb(lv_observer_t* observer, lv_subject_t* subject)
{
    auto* view = static_cast<GalleryView*>(lv_observer_get_user_data(observer));
    auto* image = lv_observer_get_target_obj(observer);
    if (!view || !image) {
        return;
    }

    const char* path = lv_subject_get_string(subject);
    view->set_observed_image_path(image, path ? path : "");
}

void gallery_delete_choice_observer_cb(lv_observer_t* observer, lv_subject_t* subject)
{
    auto* view = static_cast<GalleryView*>(lv_observer_get_user_data(observer));
    if (!view) {
        return;
    }

    view->update_delete_choice_(lv_subject_get_int(subject));
}

void style_dialog_action(lv_obj_t* obj, bool selected, bool destructive)
{
    if (!obj) {
        return;
    }

    const uint32_t bg = selected
                            ? (destructive ? color::DARK_ERRORCONTAINER : color::DARK_PRIMARYCONTAINER)
                            : color::DARK_SURFACECONTAINER;
    const uint32_t border = selected
                                ? (destructive ? color::DARK_ERROR : color::DARK_PRIMARY)
                                : color::DARK_OUTLINEVARIANT;
    const uint32_t text = selected
                              ? (destructive ? color::DARK_ONERRORCONTAINER : color::DARK_ONPRIMARYCONTAINER)
                              : color::DARK_ONSURFACEVARIANT;

    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(obj, selected ? LV_OPA_COVER : LV_OPA_60, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(obj, selected ? 2 : 1, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(text), 0);
}

} // namespace

GalleryView::GalleryView(lv_obj_t* parent)
    : BaseView(parent)
{
    build_();
}

void GalleryView::bind(lv_subject_t* image_path_subject,
                       lv_subject_t* counter_subject,
                       lv_subject_t* title_subject,
                       lv_subject_t* status_subject,
                       lv_subject_t* empty_visible_subject,
                       lv_subject_t* confirm_delete_subject,
                       lv_subject_t* delete_choice_subject)
{
    if (preview_image_ && image_path_subject) {
        lv_subject_add_observer_obj(image_path_subject, gallery_image_observer_cb, preview_image_, this);
    }

    if (counter_label_ && counter_subject) {
        lv_label_bind_text(counter_label_, counter_subject, nullptr);
    }

    if (filename_label_ && title_subject) {
        lv_label_bind_text(filename_label_, title_subject, nullptr);
    }

    if (empty_label_ && status_subject) {
        lv_label_bind_text(empty_label_, status_subject, nullptr);
    }

    if (empty_label_ && empty_visible_subject) {
        lv_obj_bind_flag_if_eq(empty_label_, empty_visible_subject, LV_OBJ_FLAG_HIDDEN, 0);
    }

    if (dialog_scrim_ && confirm_delete_subject) {
        lv_obj_bind_flag_if_eq(dialog_scrim_, confirm_delete_subject, LV_OBJ_FLAG_HIDDEN, 0);
    }

    if (dialog_ && delete_choice_subject) {
        lv_subject_add_observer_obj(delete_choice_subject, gallery_delete_choice_observer_cb, dialog_, this);
        update_delete_choice_(lv_subject_get_int(delete_choice_subject));
    }
}

void GalleryView::build_()
{
    if (!root_) {
        return;
    }

    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    build_preview_();
    build_top_bar_();
    build_bottom_bar_();
    build_delete_dialog_();
}

void GalleryView::build_preview_()
{
    preview_container_ = lv_obj_create(root_);
    lv_obj_set_size(preview_container_, LV_PCT(100), LV_PCT(100));
    style_transparent_container(preview_container_);

    preview_image_ = lv_image_create(preview_container_);
    lv_obj_set_size(preview_image_, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(preview_image_, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(preview_image_, false);
    lv_obj_center(preview_image_);

    empty_label_ = lv_label_create(preview_container_);
    lv_obj_set_style_text_font(empty_label_, Font::inter_medium(16), 0);
    lv_obj_set_style_text_color(empty_label_, lv_color_hex(color::DARK_ONSURFACE), 0);
    lv_label_set_text(empty_label_, "No photos");
    lv_obj_center(empty_label_);
}

void GalleryView::build_top_bar_()
{
    top_bar_ = lv_obj_create(root_);
    lv_obj_set_size(top_bar_, LV_PCT(100), 34);
    style_transparent_container(top_bar_);
    lv_obj_set_style_bg_color(top_bar_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    counter_label_ = lv_label_create(top_bar_);
    lv_obj_set_style_text_font(counter_label_, Font::inter_bold(13), 0);
    lv_obj_set_style_text_color(counter_label_, lv_color_white(), 0);
    lv_label_set_text(counter_label_, "0 / 0");
    lv_obj_align(counter_label_, LV_ALIGN_LEFT_MID, 8, 0);

    filename_label_ = lv_label_create(top_bar_);
    lv_obj_set_style_text_font(filename_label_, Font::inter_regular(12), 0);
    lv_obj_set_style_text_color(filename_label_, lv_color_hex(color::DARK_ONSURFACEVARIANT), 0);
    lv_obj_set_width(filename_label_, 210);
    lv_label_set_long_mode(filename_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(filename_label_, "Gallery");
    lv_obj_align(filename_label_, LV_ALIGN_RIGHT_MID, -8, 0);
}

void GalleryView::build_bottom_bar_()
{
    bottom_bar_ = lv_obj_create(root_);
    lv_obj_set_size(bottom_bar_, LV_PCT(100), kBottomBarHeight);
    style_transparent_container(bottom_bar_);
    lv_obj_set_style_bg_color(bottom_bar_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_40, 0);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    build_key_action_(font::ICON_ARROW_LEFT,
                      Font::camera_icons(28),
                      nullptr,
                      LV_ALIGN_BOTTOM_LEFT,
                      26);

    build_key_action_(font::ICON_TRASH,
                      Font::camera_icons(28),
                      nullptr,
                      LV_ALIGN_CENTER,
                      0);
                      
    build_key_action_(font::ICON_ARROW_RIGHT,
                      Font::camera_icons(28),
                      nullptr,
                      LV_ALIGN_BOTTOM_RIGHT,
                      -26);
}

void GalleryView::build_delete_dialog_()
{
    dialog_scrim_ = lv_obj_create(root_);
    lv_obj_set_size(dialog_scrim_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(dialog_scrim_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dialog_scrim_, LV_OPA_70, 0);
    lv_obj_set_style_border_width(dialog_scrim_, 0, 0);
    lv_obj_set_style_radius(dialog_scrim_, 0, 0);
    lv_obj_set_style_pad_all(dialog_scrim_, 0, 0);
    lv_obj_clear_flag(dialog_scrim_, LV_OBJ_FLAG_SCROLLABLE);

    dialog_ = lv_obj_create(dialog_scrim_);
    lv_obj_set_size(dialog_, 236, 112);
    lv_obj_set_style_bg_color(dialog_, lv_color_hex(color::DARK_SURFACECONTAINERHIGH), 0);
    lv_obj_set_style_bg_opa(dialog_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dialog_, lv_color_hex(color::DARK_OUTLINEVARIANT), 0);
    lv_obj_set_style_border_width(dialog_, 1, 0);
    lv_obj_set_style_radius(dialog_, 8, 0);
    lv_obj_set_style_pad_all(dialog_, 10, 0);
    lv_obj_clear_flag(dialog_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(dialog_);

    dialog_title_label_ = lv_label_create(dialog_);
    lv_obj_set_style_text_font(dialog_title_label_, Font::inter_bold(15), 0);
    lv_obj_set_style_text_color(dialog_title_label_, lv_color_white(), 0);
    lv_label_set_text(dialog_title_label_, "Delete photo?");
    lv_obj_align(dialog_title_label_, LV_ALIGN_TOP_LEFT, 0, 0);

    dialog_body_label_ = lv_label_create(dialog_);
    lv_obj_set_style_text_font(dialog_body_label_, Font::inter_regular(12), 0);
    lv_obj_set_style_text_color(dialog_body_label_, lv_color_hex(color::DARK_ONSURFACEVARIANT), 0);
    lv_obj_set_width(dialog_body_label_, 216);
    lv_label_set_text(dialog_body_label_, "This cannot be undone.");
    lv_obj_align(dialog_body_label_, LV_ALIGN_TOP_LEFT, 0, 23);

    lv_obj_t* actions = lv_obj_create(dialog_);
    lv_obj_set_size(actions, 216, 34);
    style_transparent_container(actions);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(actions, LV_ALIGN_BOTTOM_MID, 0, 0);

    dialog_cancel_btn_ = build_dialog_action_(actions, "Cancel", font::ICON_CROSS);
    dialog_confirm_btn_ = build_dialog_action_(actions,"Confirm", font::ICON_CHECK);
    update_delete_choice_(0);

    lv_obj_add_flag(dialog_scrim_, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* GalleryView::build_dialog_action_(lv_obj_t* parent,
                                            const char* text,
                                            const char* action_icon)
{
    lv_obj_t* action = lv_obj_create(parent);
    lv_obj_set_size(action, 76, 30);
    lv_obj_set_style_radius(action, 6, 0);
    lv_obj_set_style_pad_hor(action, 6, 0);
    lv_obj_set_style_pad_ver(action, 0, 0);
    lv_obj_set_style_pad_column(action, 4, 0);
    lv_obj_clear_flag(action, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(action, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(action, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* action_label = lv_label_create(action);
    lv_obj_set_style_text_font(action_label, Font::camera_icons(16), 0);
    lv_label_set_text(action_label, action_icon ? action_icon : "");
    lv_obj_set_style_translate_y(action_label, -1, 0);

    lv_obj_t* text_label = lv_label_create(action);
    lv_obj_set_style_text_font(text_label, Font::inter_semibold(11), 0);
    lv_label_set_text(text_label, text ? text : "");

    return action;
}

void GalleryView::build_key_action_(const char* key_icon,
                                    lv_font_t* key_font,
                                    const char* action_icon,
                                    lv_align_t align,
                                    int32_t x_offset)
{
    lv_obj_t* key = lv_obj_create(bottom_bar_);
    lv_obj_set_size(key, 36, 36);
    style_transparent_container(key);
    lv_obj_align(key, align, x_offset, kActionLiftY);
    lv_obj_set_style_radius(key, 6, 0);

    lv_obj_t* key_label = lv_label_create(key);
    lv_obj_set_style_text_font(key_label, key_font, 0);
    lv_obj_set_style_text_color(key_label, lv_color_white(), 0);
    lv_label_set_text(key_label, key_icon ? key_icon : "");
    lv_obj_align(key_label, LV_ALIGN_CENTER, action_icon ? -7 : 0, -1);

    if (action_icon) {
        lv_obj_t* action_label = lv_label_create(key);
        lv_obj_set_style_text_font(action_label, Font::camera_icons(18), 0);
        lv_obj_set_style_text_color(action_label, lv_color_hex(color::DARK_ERROR), 0);
        lv_label_set_text(action_label, action_icon);
        lv_obj_align(action_label, LV_ALIGN_CENTER, 11, 0);
    }

    lv_obj_t* guide_line = lv_obj_create(bottom_bar_);
    lv_obj_set_size(guide_line, 2, kGuideLineHeight);
    lv_obj_set_style_bg_color(guide_line, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(guide_line, LV_OPA_60, 0);
    lv_obj_set_style_border_width(guide_line, 0, 0);
    lv_obj_set_style_radius(guide_line, 1, 0);
    lv_obj_set_style_pad_all(guide_line, 0, 0);
    lv_obj_clear_flag(guide_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align_to(guide_line, key, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
}

void GalleryView::set_observed_image_path(lv_obj_t* image, const char* path)
{
    if (!image) {
        return;
    }

    if (!path || path[0] == '\0') {
        lvgl_image_path_.clear();
        lv_image_set_scale(image, LV_SCALE_NONE);
        lv_image_set_src(image, nullptr);
        return;
    }

    lvgl_image_path_ = std::string("A:") + path;
    lv_image_set_src(image, lvgl_image_path_.c_str());

    lv_image_header_t header {};
    if (lv_image_decoder_get_info(lvgl_image_path_.c_str(), &header) != LV_RESULT_OK ||
        header.w == 0 ||
        header.h == 0) {
        return;
    }

    const uint32_t scale_x = static_cast<uint32_t>(kPreviewFitWidth * LV_SCALE_NONE / header.w);
    const uint32_t scale_y = static_cast<uint32_t>(kPreviewFitHeight * LV_SCALE_NONE / header.h);
    const uint32_t scale = std::max<uint32_t>(1, std::min({scale_x, scale_y, static_cast<uint32_t>(LV_SCALE_NONE)}));
    lv_image_set_scale(image, scale);
    lv_image_set_antialias(image, scale >= LV_SCALE_NONE);
}

void GalleryView::update_delete_choice_(int32_t choice)
{
    style_dialog_action(dialog_cancel_btn_, choice == 0, false);
    style_dialog_action(dialog_confirm_btn_, choice != 0, true);
}

} // namespace view
