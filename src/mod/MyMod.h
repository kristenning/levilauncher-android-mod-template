#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "pl/cpp/Config.hpp"

namespace pl::mod {
class NativeMod;
}

namespace my_mod {

// ============================================================================
// 三维坐标
// ============================================================================
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// ============================================================================
// 传送点
// ============================================================================
struct Waypoint {
    std::string name;
    float x = 0.0f;
    float y = 64.0f;
    float z = 0.0f;
    int dimension = 0;
    std::string color = "#FF4444";
};

// ============================================================================
// 持久化配置（改 version 号后旧配置自动升级）
// ============================================================================
struct MinimapConfig {
    int version = 1;

    bool enabled = true;            // 总开关
    float mapSize = 160.0f;         // 小地图直径（像素）
    float mapScale = 2.0f;          // 每个像素=多少方块（值越大视野越广）
    float mapAlpha = 0.6f;          // 背景透明度
    bool rotateWithPlayer = true;   // 跟随玩家朝向旋转

    float btnX = 0.91f;             // 开关按钮 X（屏幕比例 0~1）
    float btnY = 0.07f;             // 开关按钮 Y
    float btnSize = 42.0f;          // 按钮直径（像素）
    float btnAlpha = 0.5f;          // 按钮透明度

    std::vector<Waypoint> waypoints; // 传送点列表

    // ---- 运行时状态（$ 前缀 = 不保存到文件） ----
    bool $minimapOpen = false;
    int $screenW = 1080;
    int $screenH = 1920;
};

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

// ============================================================================
// 小地图模组主类
// ============================================================================
class MyMod {
public:
    static MyMod &getInstance();
    [[nodiscard]] pl::mod::NativeMod &getSelf() const;

    bool load();
    bool enable();
    bool disable();
    bool unload();

    // 供 Hook 回调使用
    void onPlayerPos(float x, float y, float z, float yaw);
    void onScreenSize(int w, int h);
    void onTouch(float sx, float sy, bool down);
    void drawUI();

    void addWaypoint(const std::string &name, float x, float y, float z);
    void removeWaypoint(size_t i);
    void teleportToWaypoint(const Waypoint &wp);

    const MinimapConfig &cfg() const { return config; }
    const Vec3 &pos() const { return playerPos; }
    float yaw() const { return playerYaw; }
    bool mapOpen() const { return config.$minimapOpen; }

private:
    MinimapConfig config;
    std::mutex mtx;
    Vec3 playerPos;
    float playerYaw = 0.0f;

    // ---- Hook 句柄 ----
    void *hookPos = nullptr;      // 玩家位置 Hook
    void *hookRender = nullptr;   // 渲染 Hook
    void *hookTouch = nullptr;    // 触摸 Hook

    // ---- 原始函数指针（trampoline） ----
    void *origPos = nullptr;
    void *origRender = nullptr;
    void *origTouch = nullptr;

    // ---- 内部 ----
    bool installHooks();
    bool uninstallHooks();
    void saveCfg();
    void drawCircle(float cx, float cy, float r, uint8_t R, uint8_t G, uint8_t B, uint8_t A);
    void drawFillCircle(float cx, float cy, float r, uint8_t R, uint8_t G, uint8_t B, uint8_t A);
    static bool inCircle(float px, float py, float cx, float cy, float r);
    void screenToWorld(float sx, float sy, float &wx, float &wz);
    static bool hexToRGB(const std::string &h, uint8_t &R, uint8_t &G, uint8_t &B);
};

} // namespace my_mod

// ============================================================================
// JSON Schema（给 config.schema.json 用的元数据）
// ============================================================================
template <>
struct pl::config::Schema<my_mod::MinimapConfig> {
    static constexpr std::string_view title = "MiniMap";
    static constexpr std::string_view description = "On‑screen minimap + waypoints + teleport.";
    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version")       return {.title = "Version", .readOnly = true};
        if (name == "enabled")       return {.title = "Enabled"};
        if (name == "mapSize")       return {.title = "Map Size (px)", .minimum = 60.0, .maximum = 400.0};
        if (name == "mapScale")      return {.title = "Scale (block/px)", .minimum = 0.5, .maximum = 16.0};
        if (name == "mapAlpha")      return {.title = "Opacity", .minimum = 0.1, .maximum = 1.0};
        if (name == "rotateWithPlayer") return {.title = "Rotate with Player"};
        if (name == "btnX")          return {.title = "Button X", .minimum = 0.0, .maximum = 1.0};
        if (name == "btnY")          return {.title = "Button Y", .minimum = 0.0, .maximum = 1.0};
        if (name == "btnSize")       return {.title = "Button Size", .minimum = 24.0, .maximum = 96.0};
        if (name == "btnAlpha")      return {.title = "Button Alpha", .minimum = 0.1, .maximum = 1.0};
        if (name == "waypoints")     return {.title = "Waypoints"};
        return {};
    }
};
