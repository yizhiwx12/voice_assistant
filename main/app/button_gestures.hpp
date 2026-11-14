#pragma once

#include <array>
#include <cstdint>

namespace voice {

enum class ButtonEvent : uint8_t { None = 0, FirstShort = 1, FirstLong = 2, SecondShort = 4, SecondLong = 8 };

// 调用者提供采样到的按下电平和单调递增的毫秒时钟。
// 本识别器不运行回调、不分配内存、不处理中断或音频。
class ButtonGestures {
public:
    static constexpr uint32_t kDebounceMs = 30;
    static constexpr uint32_t kLongPressMs = 1000;
    static constexpr uint32_t kPollMs = 20;

    void reset(bool first_pressed, bool second_pressed, uint32_t now_ms)
    {
        buttons_ = {};
        initialize(buttons_[0], first_pressed, now_ms);
        initialize(buttons_[1], second_pressed, now_ms);
    }

    uint8_t poll(bool first_pressed, bool second_pressed, uint32_t now_ms)
    {
        const bool pressed[] = {first_pressed, second_pressed};
        // 两个原始触点同时闭合时立即抑制重叠按键，
        // 避免去抖或长按边界先发出单键事件。
        if (first_pressed && second_pressed) {
            buttons_[0].suppressed = true;
            buttons_[1].suppressed = true;
        }
        uint8_t events = 0;
        for (unsigned i = 0; i < buttons_.size(); ++i) {
            Button &button = buttons_[i];
            if (button.raw_pressed != pressed[i]) {
                button.raw_pressed = pressed[i];
                button.raw_since_ms = now_ms;
            }
            if (button.stable_pressed != button.raw_pressed &&
                now_ms - button.raw_since_ms >= kDebounceMs) {
                button.stable_pressed = button.raw_pressed;
                if (button.stable_pressed) {
                    button.press_since_ms = now_ms;
                    button.long_emitted = false;
                } else {
                    if (!button.suppressed && !button.long_emitted)
                        events |= static_cast<uint8_t>(i == 0 ? ButtonEvent::FirstShort : ButtonEvent::SecondShort);
                    button.suppressed = false;
                    button.long_emitted = false;
                }
            }
            // 松开电平尚在去抖确认期间时，
            // 不能把原来的短按误判为长按。
            if (button.stable_pressed && button.raw_pressed && !button.suppressed &&
                !button.long_emitted && now_ms - button.press_since_ms >= kLongPressMs) {
                button.long_emitted = true;
                events |= static_cast<uint8_t>(i == 0 ? ButtonEvent::FirstLong : ButtonEvent::SecondLong);
            }
            // 重叠触点即使因抖动而从未形成稳定按下，
            // 也要经过完整的稳定松开间隔才能重新启用。
            if (!button.stable_pressed && !button.raw_pressed &&
                now_ms - button.raw_since_ms >= kDebounceMs)
                button.suppressed = false;
        }
        return events;
    }

    static constexpr bool has(uint8_t events, ButtonEvent event)
    {
        return (events & static_cast<uint8_t>(event)) != 0;
    }

private:
    struct Button {
        bool raw_pressed = false;
        bool stable_pressed = false;
        // 启动时已按住的按钮必须先松开；
        // 重叠按键也要等涉及的各按钮都稳定松开后才解除抑制。
        bool suppressed = true;
        bool long_emitted = false;
        uint32_t raw_since_ms = 0;
        uint32_t press_since_ms = 0;
    };
    static void initialize(Button &button, bool pressed, uint32_t now_ms)
    {
        button.raw_pressed = pressed;
        button.stable_pressed = pressed;
        button.suppressed = pressed;
        button.raw_since_ms = now_ms;
        button.press_since_ms = now_ms;
    }
    std::array<Button, 2> buttons_{};
};

}  // 命名空间 voice 结束
