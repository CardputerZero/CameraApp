#include "viewmodels/gallery_viewmodel.h"

#include <filesystem>
#include <utility>

namespace viewmodel {

GalleryViewModel::GalleryViewModel(std::shared_ptr<service::AppServices> services)
    : services_(std::move(services))
{
}

void GalleryViewModel::on_enter()
{
    if (services_ && services_->gallery) {
        services_->gallery->start();
    }
    confirm_delete_ = false;
    delete_choice_ = 0;
    refresh_snapshot_();
}

void GalleryViewModel::on_exit()
{
    if (services_ && services_->gallery) {
        services_->gallery->stop();
    }
}

bool GalleryViewModel::handle_action(app::AppAction action)
{
    if (!services_ || !services_->gallery) {
        return false;
    }

    if (confirm_delete_) {
        if (action == app::AppAction::PanLeft) {
            delete_choice_ = 0;
            refresh_snapshot_();
            return true;
        }

        if (action == app::AppAction::PanRight) {
            delete_choice_ = 1;
            refresh_snapshot_();
            return true;
        }

        if (action == app::AppAction::Confirm) {
            if (delete_choice_ == 1) {
                services_->gallery->delete_current();
            }
            confirm_delete_ = false;
            delete_choice_ = 0;
            refresh_snapshot_();
            return true;
        }

        if (action == app::AppAction::Exit) {
            confirm_delete_ = false;
            delete_choice_ = 0;
            refresh_snapshot_();
            return true;
        }

        return true;
    }

    if (action == app::AppAction::ZoomOut || action == app::AppAction::PanLeft) {
        services_->gallery->previous();
        refresh_snapshot_();
        return true;
    }

    if (action == app::AppAction::ToggleCaptureMode || action == app::AppAction::PanRight) {
        services_->gallery->next();
        refresh_snapshot_();
        return true;
    }

    if (action == app::AppAction::Capture || action == app::AppAction::Delete) {
        if (!services_->gallery->has_items()) {
            refresh_snapshot_();
            return true;
        }
        confirm_delete_ = true;
        delete_choice_ = 0;
        refresh_snapshot_();
        return true;
    }

    if (action == app::AppAction::Exit) {
        request_transition(app::AppState::Camera);
        return true;
    }

    return false;
}

GallerySnapshot GalleryViewModel::snapshot() const
{
    return snapshot_;
}

void GalleryViewModel::refresh_snapshot_()
{
    snapshot_ = {};
    snapshot_.confirm_delete = confirm_delete_;
    snapshot_.delete_choice = delete_choice_;

    if (!services_ || !services_->gallery) {
        snapshot_.status = "Gallery unavailable";
        image_path_subject_.set("");
        counter_subject_.set("0 / 0");
        title_subject_.set(snapshot_.status);
        status_subject_.set(snapshot_.status);
        empty_visible_subject_.set(true);
        confirm_delete_subject_.set(false);
        delete_choice_subject_.set(0);
        return;
    }

    snapshot_.path = services_->gallery->current_path();
    snapshot_.preview_path = services_->gallery->current_preview_path();
    snapshot_.status = services_->gallery->status_message();
    snapshot_.index = services_->gallery->has_items() ? services_->gallery->current_index() + 1 : 0;
    snapshot_.count = services_->gallery->count();

    image_path_subject_.set(snapshot_.preview_path.empty() ? snapshot_.path : snapshot_.preview_path);
    counter_subject_.set(snapshot_.count == 0
                             ? "0 / 0"
                             : std::to_string(snapshot_.index) + " / " + std::to_string(snapshot_.count));
    title_subject_.set(snapshot_.path.empty()
                           ? snapshot_.status
                           : std::filesystem::path(snapshot_.path).filename().string());
    status_subject_.set(snapshot_.status.empty() ? "No photos" : snapshot_.status);
    empty_visible_subject_.set(snapshot_.path.empty());
    confirm_delete_subject_.set(snapshot_.confirm_delete);
    delete_choice_subject_.set(snapshot_.delete_choice);
}

} // namespace viewmodel
