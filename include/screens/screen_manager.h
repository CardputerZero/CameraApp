#pragma once

#include "screen.h"
#include <stack>
#include <unordered_map>
#include <functional>
#include <utility>

namespace screen {

/**
 * @brief 屏幕管理器
 * 
 * 负责：
 * - 管理所有 Screen 的生命周期
 * - 处理屏幕间的导航（push/pop）
 * - 维护导航栈
 * - 处理屏幕的显示/隐藏
 */
class ScreenManager {
public:
    using ScreenFactory = std::function<std::shared_ptr<Screen>(lv_obj_t*)>;

    explicit ScreenManager(lv_obj_t* root)
        : root_(root) {}

    ~ScreenManager() {
        // 清空导航栈
        while (!screen_stack_.empty()) {
            screen_stack_.pop();
        }
        screens_.clear();
    }

    ScreenManager(const ScreenManager&) = delete;
    ScreenManager& operator=(const ScreenManager&) = delete;

    /**
     * @brief 注册一个屏幕工厂
     * 
     * @param screen_id 屏幕唯一标识
     * @param factory 屏幕工厂函数
     * 
     * 示例：
     * manager.register_screen("auto_test", [](lv_obj_t* parent) {
     *     auto viewmodel = std::make_shared<TestViewModel>();
     *     auto view = std::make_unique<TestView>(parent);
     *     return std::make_shared<Screen>(parent, std::move(view), viewmodel);
     * });
     */
    void register_screen(const std::string& screen_id, ScreenFactory factory) {
        factories_[screen_id] = std::move(factory);
    }

    /**
     * @brief 导航到屏幕（压入栈）
     * 
     * 如果屏幕已存在，直接显示；否则通过工厂创建
     */
    bool push_screen(const std::string& screen_id) {
        if (factories_.find(screen_id) == factories_.end()) {
            return false;
        }

        // 隐藏当前屏幕
        if (!screen_stack_.empty()) {
            auto current = screen_stack_.top();
            current->hide();
        }

        // 查找或创建屏幕
        std::shared_ptr<Screen> screen;
        auto it = screens_.find(screen_id);
        if (it != screens_.end()) {
            screen = it->second;
            screens_.erase(it);
        } else {
            screen = factories_[screen_id](root_);
        }

        if (!screen) {
            return false;
        }

        // 显示新屏幕
        screen->show();
        screen_stack_.push(std::move(screen));
        return true;
    }

    /**
     * @brief 返回到上一个屏幕（弹出栈）
     */
    bool pop_screen() {
        if (screen_stack_.empty()) {
            return false;
        }

        auto current = std::move(screen_stack_.top());
        screen_stack_.pop();

        current->hide();
        // 可选：缓存屏幕或销毁
        // screens_[id] = std::move(current);

        // 显示前一个屏幕
        if (!screen_stack_.empty()) {
            screen_stack_.top()->show();
            return true;
        }

        return false;
    }

    /**
     * @brief 替换当前屏幕
     */
    bool replace_screen(const std::string& screen_id) {
        pop_screen();
        return push_screen(screen_id);
    }

    /**
     * @brief 获取当前屏幕
     */
    Screen* current_screen() const {
        return screen_stack_.empty() ? nullptr : screen_stack_.top().get();
    }

    /**
     * @brief 屏幕栈深度
     */
    size_t stack_size() const {
        return screen_stack_.size();
    }

    /**
     * @brief 更新当前屏幕
     */
    void update(uint32_t delta_ms) {
        if (!screen_stack_.empty()) {
            screen_stack_.top()->update(delta_ms);
        }
    }

    /**
     * @brief 清空所有屏幕
     */
    void clear() {
        while (!screen_stack_.empty()) {
            screen_stack_.pop();
        }
        screens_.clear();
    }

private:
    lv_obj_t* root_{nullptr};
    std::stack<std::shared_ptr<Screen>> screen_stack_;
    std::unordered_map<std::string, std::shared_ptr<Screen>> screens_; // 缓存的屏幕
    std::unordered_map<std::string, ScreenFactory> factories_;
};

} // namespace ui::screen
