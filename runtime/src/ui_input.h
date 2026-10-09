#pragma once
// Debug input transport. The game WndProc appends these messages to its input queue;
// it does not call widget handlers. The caller runs before the engine snapshots that queue.
#include <windows.h>
#include <cmath>
#include <string>

namespace twui {
inline bool clickPoint(float x, float y, float width, float height, const RECT& client, LPARAM& point) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
        width <= 0 || height <= 0) return false;
    const double cx = double(x) + double(width) * 0.5, cy = double(y) + double(height) * 0.5;
    // WM_MOUSE coordinates are signed 16-bit client pixels. Never wrap, clamp or guess.
    if (cx < client.left || cy < client.top || cx >= client.right || cy >= client.bottom ||
        cx < 0 || cy < 0 || cx > 32767 || cy > 32767) return false;
    const int px = int(cx), py = int(cy);
    if (px < x || py < y || px >= double(x) + width || py >= double(y) + height) return false;
    point = MAKELPARAM(px, py); return true;
}

struct WindowInput {
    virtual ~WindowInput() = default;
    virtual bool send(UINT message, WPARAM key, LPARAM detail, unsigned type, unsigned event) = 0;
};
inline bool windowClick(WindowInput& input, LPARAM point, std::string& error) {
    // Normal hover/hit testing precedes down/up; no widget flag or capture list is edited.
    if (!input.send(WM_MOUSEMOVE, 0, point, 1, 12)) {
        error = "window-click-queue-rejected-effect-uncertain-never-retry"; return false;
    }
    const bool down=input.send(WM_LBUTTONDOWN, MK_LBUTTON, point, 1, 1);
    const bool up=input.send(WM_LBUTTONUP, 0, point, 1, 0);
    // An ambiguous down still needs a release. This is cleanup of the same sequence, no retry.
    if (!down || !up) { error = "window-click-queue-rejected-effect-uncertain-never-retry"; return false; }
    return true;
}
inline bool windowKey(WindowInput& input, const std::string& key, std::string& error) {
    if (key != "esc") { error = "unsupported-key"; return false; }
    // The game's converter reads the scan code in lParam, not wParam alone.
    const LPARAM down = 1 | (LPARAM(1) << 16); // DIK_ESCAPE=1 -> engine key id 0
    const bool pressed=input.send(WM_KEYDOWN, VK_ESCAPE, down, 2, 1);
    const bool released=input.send(WM_KEYUP, VK_ESCAPE, down | LPARAM(0xC0000000u), 2, 0);
    if (!pressed || !released) {
        error = "window-key-queue-rejected-effect-uncertain-never-retry"; return false;
    }
    return true;
}
} // namespace twui
