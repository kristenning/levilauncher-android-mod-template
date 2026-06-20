#pragma once
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "pl/cpp/Config.hpp"

namespace pl::mod {
class NativeMod;
}

namespace my_mod {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Waypoint {
    std::string name;
    float x = 0.0f;
    float y = 64.0f;
    float z = 0.0f;
    int dimension = 0;
    std::string color = "#FF4444";
};

struct ModConfig {
    int version = 1;
    bool enabled = true;
    float mapSize = 160.0f;
    float mapScale = 2.0f;
    float mapAlpha = 0.6f;
    bool rotateWithPlayer = true;
    float btnX = 0.91f;
    float btnY = 0.07f;
    float btnSize = 42.0f;
    float btnAlpha = 0.5f;
    std::vector<Waypoint> waypoints;
};

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

class MyMod {
public:
    static MyMod &getInstance();
    [[nodiscard]] pl::mod::NativeMod &getSelf() const;

    bool load();
    bool enable();
    bool disable();
    bool unload();

    void onPlayerPos(float x, float y, float z, float yaw);
    void onScreenSize(int w, int h);
    void onTouch(float sx, float sy, bool down);
    void drawUI();

    void addWaypoint(const std::string &name, float x, float y, float z);
    void removeWaypoint(size_t i);
    void teleportToWaypoint(const Waypoint &wp);

    const ModConfig &cfg() const { return config; }
    const Vec3 &pos() const { return playerPos; }
    float yaw() const { return playerYaw; }
    bool mapOpen() const { return minimapOpen; }

private:
    ModConfig config;
    std::mutex mtx;
    Vec3 playerPos;
    float playerYaw = 0.0f;
    bool minimapOpen = false;
    int screenW = 1080;
    int screenH = 1920;
    void *hookPos = nullptr;
    void *hookRender = nullptr;
    void *hookTouch = nullptr;
    void *origPos = nullptr;
    void *origRender = nullptr;
    void *origTouch = nullptr;

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

template <>
struct pl::config::Schema<my_mod::ModConfig> {
    static constexpr std::string_view title = "MiniMap Config";
    static constexpr std::string_view description = "On-screen minimap with waypoints.";
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
