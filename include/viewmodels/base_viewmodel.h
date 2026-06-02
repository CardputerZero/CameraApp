#pragma once

#include "app/app_action.h"
#include "app/app_state.h"

#include <cstdint>
#include <string>

namespace viewmodel {

/**
 * @brief ViewModel 基类
 * 
 * 每个 Screen 对应一个 ViewModel，管理该屏幕的数据和业务逻辑
 * ViewModel 应该独立于 UI 框架（LVGL），便于测试和复用
 */
class BaseViewModel {
public:
    virtual ~BaseViewModel() = default;

    BaseViewModel(const BaseViewModel&) = delete;
    BaseViewModel& operator=(const BaseViewModel&) = delete;

    /**
     * @brief 屏幕即将显示时调用
     * 用于初始化数据或订阅事件
     */
    virtual void on_enter() {}

    /**
     * @brief 屏幕即将隐藏时调用
     * 用于保存状态或取消订阅
     */
    virtual void on_exit() {}

    /**
     * @brief 获取屏幕标题/名称
     */
    virtual std::string get_title() const { return ""; }

    /**
     * @brief 屏幕更新逻辑，可选
     */
    virtual void update(uint32_t delta_ms) {}

    /**
     * @brief 处理输入层转换出的应用动作
     *
     * 全局动作可以在入口层拦截；页面相关动作交给当前 ViewModel 消费。
     */
    virtual bool handle_action(app::AppAction action) { return false; }

    app::AppState consume_transition_request() {
        app::AppState requested = pending_transition_;
        pending_transition_ = app::AppState::None;
        return requested;
    }

protected:
    BaseViewModel() = default;

    void request_transition(app::AppState state) {
        pending_transition_ = state;
    }

private:
    app::AppState pending_transition_{app::AppState::None};
};

} // namespace viewmodel
