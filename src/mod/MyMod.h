#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "pl/cpp/Config.hpp"

namespace pl::mod {
class NativeMod;
}

namespace my_mod {

// ============================================================================
// Vec3 — 简单的三维向量
// ============================================================================
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// ============================================================================
// Waypoint — 单个传送点
// ============================================================================
struct Waypoint {
    std::string name;           // 名称
    float x = 0.0f;
    float y = 64.0f;
    float z = 0.0f;
    int dimension = 0;          // 0=主世界 1=下界 2=末地
    std::string color = "#FF4444"; // 显示颜色
    bool $selected = false;     // $前缀：运行时状态，不持久化
};

// ============================================================================
// MinimapConfig — 持久化配置
// 修改 version 号后旧配置自动合并升级
// ============================================================================
struct MinimapConfig {
    int version = 1;

    // --- 主开关 ---
    bool enabled = true;

    // --- 小地图外观 ---
    float mapSize = 180.0f;         // 小地图直径（像素）
    float mapScale = 2.0f;         // 每像素对应多少方块（值越大视野越大）
    float mapAlpha = 0.65f;        // 背景透明度 0~1
    bool rotateWithPlayer = true;  // 小地图是否跟随玩家朝向旋转

    // --- 开关按钮 ---
    float btnX = 0.92f;            // 按钮水平位置（屏幕比例，0~1）
    float btnY = 0.08f;            // 按钮垂直位置（屏幕比例，0~1）
    float btnSize = 44.0f;         // 按钮直径（像素）
    float btnAlpha = 0.55f;        // 按钮透明度

    // --- 传送点列表 ---
    std::vector<Waypoint> waypoints;

    // --- 玩家状态（运行时覆盖，不持久化） ---
    bool $minimapOpen = false;     // 小地图是否展开
    int $screenW = 1080;          // 屏幕宽（运行时更新）
    int $screenH = 1920;          // 屏幕高
};

// 供 config_generator.cpp 调用
nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

// ============================================================================
// MyMod — 小地图模组主类
// ============================================================================
class MyMod {
public:
    static MyMod &getInstance();
    [[nodiscard]] pl::mod::NativeMod &getSelf() const;

    // ---- 生命周期 ----
    bool load();
    bool enable();
    bool disable();
    bool unload();

    // ---- 运行时接口（供 Hook 回调使用） ----
    void onPlayerPositionUpdated(float x, float y, float z, float yaw);
    void onScreenSizeChanged(int w, int h);
    void onTouchEvent(float screenX, float screenY, bool down);
    void drawOverlay();           // 由渲染 Hook 调用

    // ---- 传送点管理 ----
    void addWaypoint(const std::string &name, float x, float y, float z);
    void removeWaypoint(size_t index);
    bool teleportToWaypoint(const Waypoint &wp);

    // 只读访问
    const MinimapConfig &getConfig() const { return config; }
    const Vec3 &getPlayerPos() const { return playerPos; }
    float getPlayerYaw() const { return playerYaw; }
    bool isMinimapOpen() const { return config.$minimapOpen; }

private:
    MinimapConfig config;
    std::mutex stateMutex;        // 保护运行时状态

    // 玩家状态
    Vec3 playerPos;
    float playerYaw = 0.0f;

    // ---- Hook 句柄（占位） ----
    void *mPosHookHandle = nullptr;
    void *mRenderHookHandle = nullptr;
    void *mTouchHookHandle = nullptr;

    // ---- 内部辅助 ----
    bool installHooks();
    bool uninstallHooks();
    void drawCircle(float cx, float cy, float r, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int segments);
    void drawFilledCircle(float cx, float cy, float r, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int segments);
    void drawTriangle(float cx, float cy, float size, float angle, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    bool isInsideCircle(float px, float py, float cx, float cy, float r);
    void screenToMinimap(float sx, float sy, float &mx, float &my);
    bool parseHexColor(const std::string &hex, uint8_t &r, uint8_t &g, uint8_t &b);
    bool saveConfig();
};

} // namespace my_mod

// ============================================================================
// pl::config::Schema 特化 — config.schema.json 生成
// ============================================================================
template <>
struct pl::config::Schema<my_mod::MinimapConfig> {
    static constexpr std::string_view title = "MiniMap Mod Config";
    static constexpr std::string_view description =
        "On‑screen minimap with waypoints and teleport.";

    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version")
            return {.title = "Version", .readOnly = true};
        if (name == "enabled")
            return {.title = "Enabled", .description = "Master toggle."};
        if (name == "mapSize")
            return {.title = "Map Size (px)", .minimum = 60.0, .maximum = 400.0};
        if (name == "mapScale")
            return {.title = "Scale (blocks/px)", .minimum = 0.5, .maximum = 16.0};
        if (name == "mapAlpha")
            return {.title = "Opacity", .minimum = 0.1, .maximum = 1.0};
        if (name == "rotateWithPlayer")
            return {.title = "Rotate With Player"};
        if (name == "btnX")
            return {.title = "Button X", .minimum = 0.0, .maximum = 1.0};
        if (name == "btnY")
            return {.title = "Button Y", .minimum = 0.0, .maximum = 1.0};
        if (name == "btnSize")
            return {.title = "Button Size (px)", .minimum = 24.0, .maximum = 96.0};
        if (name == "btnAlpha")
            return {.title = "Button Opacity", .minimum = 0.1, .maximum = 1.0};
        if (name == "waypoints")
            return {.title = "Waypoints"};
        return {};
    }
};
