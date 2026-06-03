#pragma once

#include "lvgl/lvgl.h"
#include "../views/base_view.h"
#include "../viewmodels/base_viewmodel.h"
#include <memory>
#include <utility>

namespace screen {

/**
 * @brief Screen 包装器
 * 
 * 一个 Screen 包含：
 * - ViewModel：业务逻辑和数据（可选，某些简单屏幕可不需要）
 * - View：UI 显示层
 * 
 * Screen 代表一个完整的屏幕/页面/视口，可以显示/隐藏/销毁
 */
class Screen {
public:
    /**
     * @brief 创建一个 Screen
     * @param parent LVGL 父对象
     * @param view_ptr View 实例
     * @param viewmodel_ptr ViewModel 实例（可选）
     */
    Screen(lv_obj_t* parent,
           std::unique_ptr<view::BaseView> view_ptr,
           std::shared_ptr<viewmodel::BaseViewModel> viewmodel_ptr = nullptr)
        : parent_(parent), view_(std::move(view_ptr)), 
          viewmodel_(std::move(viewmodel_ptr)) {}

    virtual ~Screen() {
        on_exit();
        view_.reset();
    }

    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    // ==================== Lifecycle ====================

    /**
     * @brief 屏幕显示
     */
    virtual void show() {
        on_enter();
        if (view_) {
            view_->show();
        }
    }

    /**
     * @brief 屏幕隐藏
     */
    virtual void hide() {
        if (view_) {
            view_->hide();
        }
        on_exit();
    }

    /**
     * @brief 获取屏幕的根 LVGL 对象
     */
    lv_obj_t* root() const {
        return view_ ? view_->root() : nullptr;
    }

    /**
     * @brief 获取 ViewModel
     */
    std::shared_ptr<viewmodel::BaseViewModel> viewmodel() const {
        return viewmodel_;
    }

    /**
     * @brief 获取 View
     */
    view::BaseView* view() const {
        return view_.get();
    }

    /**
     * @brief 屏幕更新逻辑
     */
    virtual void update(uint32_t delta_ms) {
        if (viewmodel_) {
            viewmodel_->update(delta_ms);
        }
    }

protected:
    /**
     * @brief 屏幕进入时的回调
     */
    virtual void on_enter() {
        if (viewmodel_) {
            viewmodel_->on_enter();
        }
    }

    /**
     * @brief 屏幕退出时的回调
     */
    virtual void on_exit() {
        if (viewmodel_) {
            viewmodel_->on_exit();
        }
    }

    lv_obj_t* parent_{nullptr};
    std::unique_ptr<view::BaseView> view_;
    std::shared_ptr<viewmodel::BaseViewModel> viewmodel_;
};

} // namespace screen
