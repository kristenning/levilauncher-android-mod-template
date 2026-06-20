#include "mod/MyMod.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <dlfcn.h>         // ← dlsym 在这里
#include <filesystem>
#include <sstream>

#include "pl/cpp/Config.hpp"
#include "pl/cpp/Hook.hpp"
#include "pl/cpp/Mod.hpp"

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  游戏函数符号名（dlsym 自动查找 — 无需手动找偏移量）                   ║
// ║  如果某个符号在你的游戏版本中不存在，日志会明确提示，不会崩溃          ║
// ╚══════════════════════════════════════════════════════════════════════════╝

// 玩家位置获取函数（libminecraftpe.so 导出符号，MSVC 风格 mangling）
// 如果这个找不到，可以试试 GCC 风格的：_ZN5Actor11getPositionEv
#define SYM_ACTOR_GETPOS  "?getPosition@Actor@@UEA?AVVec3@@XZ"

// 渲染帧结束（Android 系统库 libEGL.so，100% 存在）
#define SYM_EGL_SWAP      "eglSwapBuffers"

// 玩家朝向获取
#define SYM_ACTOR_GETROT  "?getRotation@Actor@@UEA?AVVec2@@XZ"

// ============================================================================
// 动态符号查找工具
// ============================================================================
namespace {

// 从指定库或全局符号表中查找函数地址
// handle = nullptr  → 搜索所有已加载的 .so
// handle = 具体句柄 → 只搜索该库
void *findSymbol(const char *name, void *handle = nullptr) {
    void *addr = dlsym(handle ? handle : RTLD_DEFAULT, name);
    return addr;
}

// 尝试多个可能的符号名，返回第一个找到的
void *findSymbolAny(std::initializer_list<const char *> names) {
    for (auto name : names) {
        void *addr = dlsym(RTLD_DEFAULT, name);
        if (addr) return addr;
    }
    return nullptr;
}

} // anonymous namespace

namespace my_mod {

// ============================================================================
// 单例
// ============================================================================
MyMod &MyMod::getInstance() {
    static MyMod instance;
    return instance;
}

pl::mod::NativeMod &MyMod::getSelf() const {
    return *pl::mod::NativeMod::current();
}

// ============================================================================
// load()
// ============================================================================
bool MyMod::load() {
    auto &s = getSelf();
    s.getLogger().debug("[MiniMap] load() start");

    // 1. 创建目录
    std::error_code ec;
    std::filesystem::create_directories(s.getDataDir(), ec);
    std::filesystem::create_directories(s.getConfigDir(), ec);

    // 2. 加载配置
    pl::config::ConfigFile<MinimapConfig> cf;
    if (!cf.load()) {
        s.getLogger().warn("[MiniMap] config load failed — using defaults");
        config = MinimapConfig{};
    } else {
        config = cf.value();
    }

    config.$minimapOpen = false;
    s.getLogger().info("[MiniMap] load() ok — waypoints={}", config.waypoints.size());
    return true;
}

// ============================================================================
// enable() — 自动查找符号 + 安装 Hook
// ============================================================================
bool MyMod::enable() {
    auto &s = getSelf();

    if (!config.enabled) {
        s.getLogger().info("[MiniMap] disabled by config");
        return true;
    }

    // ---------- 自动查找符号 ----------
    s.getLogger().debug("[MiniMap] searching symbols with dlsym...");

    // ① 玩家位置函数
    //    尝试 MSVC 和 GCC 两种 mangling 风格
    void *addrGetPos = findSymbolAny({
        SYM_ACTOR_GETPOS,                    // MSVC:  ?getPosition@Actor@@UEA?AVVec3@@XZ
        "_ZN5Actor11getPositionEv",          // GCC:   Actor::getPosition()
        "_ZN5Actor6getPosEv"                 // 缩写:  Actor::getPos()
    });
    if (addrGetPos) {
        s.getLogger().info("[MiniMap] ✓ Actor::getPosition @ {}", addrGetPos);
    } else {
        s.getLogger().warn("[MiniMap] ✗ Actor::getPosition NOT FOUND — "
                           "player tracking disabled");
    }

    // ② 渲染帧尾（eglSwapBuffers 一定存在）
    void *addrSwap = findSymbol(SYM_EGL_SWAP);
    if (addrSwap) {
        s.getLogger().info("[MiniMap] ✓ eglSwapBuffers @ {}", addrSwap);
    } else {
        s.getLogger().error("[MiniMap] ✗ eglSwapBuffers NOT FOUND — fatal");
        return false;
    }

    // ③ 触摸函数（通常需要找游戏内部的 InputHandler）
    //    先在全局符号里搜，搜不到就跳过（不影响小地图基本功能）
    void *addrTouch = findSymbolAny({
        "_ZN11InputHandler9onTouchUpEiff",   // 示例，实际以反编译为准
        "_ZN11InputHandler11onTouchDownEiff",
        "_ZN9Minecraft10onTouchEndEii"
    });
    if (addrTouch) {
        s.getLogger().info("[MiniMap] ✓ Touch handler @ {}", addrTouch);
    } else {
        s.getLogger().warn("[MiniMap] ✗ Touch handler NOT FOUND — "
                           "button interaction disabled");
    }

    // ---------- 安装 Hook ----------
    if (!installHooks()) {
        s.getLogger().error("[MiniMap] hook install failed");
        return false;
    }

    s.getLogger().info("[MiniMap] enabled. Tap top‑right button to open minimap.");
    return true;
}

// ============================================================================
// disable()
// ============================================================================
bool MyMod::disable() {
    getSelf().getLogger().debug("[MiniMap] disable()");
    {
        std::lock_guard<std::mutex> lk(mtx);
        config.$minimapOpen = false;
    }
    uninstallHooks();
    return true;
}

// ============================================================================
// unload()
// ============================================================================
bool MyMod::unload() {
    getSelf().getLogger().debug("[MiniMap] unload()");
    uninstallHooks();
    return true;
}

// ============================================================================
// 安装 Hook（使用 dlsym 找到的地址）
// ============================================================================
bool MyMod::installHooks() {
    auto &s = getSelf();

    // ---- ① 玩家位置 Hook ----
    void *addrGetPos = findSymbolAny({
        SYM_ACTOR_GETPOS, "_ZN5Actor11getPositionEv", "_ZN5Actor6getPosEv"
    });
    if (addrGetPos) {
        // ★★★ 这是你需要理解的 Hook 模板 ★★★
        // pl::hook::hook(目标地址, 你的替换函数, 保存原函数的指针, 优先级)
        //
        // 原理：当游戏调用 Actor::getPosition() 时，
        // 会先跳转到你写的 lambda 里，
        // lambda 里可以记录坐标，然后调用原始函数返回正常值。
        //
        // 如果游戏版本更新后符号名变了：
        //   1. 在日志里看 "✗ Actor::getPosition NOT FOUND"
        //   2. 把上面 SYM_ACTOR_GETPOS 改成新版本的符号名
        //   3. 或者在这里直接填入用 IDA 找到的地址：
        //      addrGetPos = (void*)0x12345678;
        int rc = pl::hook::hook(
            reinterpret_cast<pl::hook::FuncPtr>(addrGetPos),
            // ↓ 你的替换函数（lambda）↓
            reinterpret_cast<pl::hook::FuncPtr>(
                +[](void *actor) -> Vec3 {
                    // 注意：这个 lambda 签名必须和原函数完全一致
                    // Vec3 (*)(void *actor)
                    auto &mod = MyMod::getInstance();

                    // 先调用原函数拿到真实坐标
                    using GetPosFn = Vec3 (*)(void *);
                    auto orig = reinterpret_cast<GetPosFn>(mod.origPos);
                    Vec3 result = orig(actor);

                    // 记录坐标（供小地图显示）
                    // yaw 从别处获取，这里先传 0
                    mod.onPlayerPos(result.x, result.y, result.z, 0.0f);

                    return result; // 不修改，原样返回
                }),
            reinterpret_cast<pl::hook::FuncPtr *>(&origPos),
            pl::hook::PriorityNormal);
        if (rc == 0) {
            s.getLogger().info("[MiniMap] ✓ pos hook installed");
        } else {
            s.getLogger().error("[MiniMap] ✗ pos hook failed: {}", rc);
        }
    }

    // ---- ② 渲染 Hook（eglSwapBuffers） ----
    void *addrSwap = findSymbol(SYM_EGL_SWAP);
    if (addrSwap) {
        // EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surf)
        // 注意：这里的函数签名是关键 — 必须和原函数完全一致
        // 如果你不确定返回类型，可以先用 void* 占位再调试
        int rc = pl::hook::hook(
            reinterpret_cast<pl::hook::FuncPtr>(addrSwap),
            reinterpret_cast<pl::hook::FuncPtr>(
                +[](void *dpy, void *surf) -> int {
                    auto &mod = MyMod::getInstance();

                    // 先调用原始 swap（让游戏正常渲染）
                    using SwapFn = int (*)(void *, void *);
                    auto orig = reinterpret_cast<SwapFn>(mod.origRender);
                    int ret = orig(dpy, surf);

                    // 然后绘制我们的 UI 覆盖层
                    mod.drawUI();

                    return ret;
                }),
            reinterpret_cast<pl::hook::FuncPtr *>(&origRender),
            pl::hook::PriorityNormal);
        if (rc == 0) {
            s.getLogger().info("[MiniMap] ✓ render hook installed");
        }
    }

    // ---- ③ 触摸 Hook（占位，不阻塞） ----
    //    Android 的触摸事件通常在 Java 层处理。
    //    如果找不到 C++ 层的触摸函数，可以通过 JNI 在 Java 层拦截。
    //    这里留空，不影响小地图显示。
    s.getLogger().debug("[MiniMap] touch hook skipped (needs JNI or game-specific handler)");

    return true;
}

// ============================================================================
// 卸载 Hook
// ============================================================================
bool MyMod::uninstallHooks() {
    // pl::hook::unhook(目标, 替换)
    if (origPos && hookPos) {
        void *addr = findSymbolAny({SYM_ACTOR_GETPOS, "_ZN5Actor11getPositionEv"});
        if (addr) pl::hook::unhook(
            reinterpret_cast<pl::hook::FuncPtr>(addr),
            reinterpret_cast<pl::hook::FuncPtr>(hookPos));
    }
    if (origRender && hookRender) {
        void *addr = findSymbol(SYM_EGL_SWAP);
        if (addr) pl::hook::unhook(
            reinterpret_cast<pl::hook::FuncPtr>(addr),
            reinterpret_cast<pl::hook::FuncPtr>(hookRender));
    }
    hookPos = hookRender = hookTouch = nullptr;
    origPos = origRender = origTouch = nullptr;
    return true;
}

// ============================================================================
// 回调：玩家位置更新
// ============================================================================
void MyMod::onPlayerPos(float x, float y, float z, float yaw) {
    std::lock_guard<std::mutex> lk(mtx);
    playerPos = {x, y, z};
    playerYaw = yaw;
}

void MyMod::onScreenSize(int w, int h) {
    std::lock_guard<std::mutex> lk(mtx);
    config.$screenW = w;
    config.$screenH = h;
}

// ============================================================================
// 回调：触摸事件
// ============================================================================
void MyMod::onTouch(float sx, float sy, bool down) {
    if (!down) return;
    std::lock_guard<std::mutex> lk(mtx);

    int W = config.$screenW, H = config.$screenH;
    if (W == 0 || H == 0) return;

    // ① 按钮区域检测
    float btnCx = config.btnX * W;
    float btnCy = config.btnY * H;
    float btnR  = config.btnSize * 0.5f;
    if (inCircle(sx, sy, btnCx, btnCy, btnR)) {
        config.$minimapOpen = !config.$minimapOpen;
        getSelf().getLogger().info("[MiniMap] {}", config.$minimapOpen ? "OPEN" : "CLOSED");
        return;
    }

    // ② 小地图未打开 → 忽略
    if (!config.$minimapOpen) return;

    // ③ 小地图区域检测
    float mcX = W - config.mapSize * 0.5f - 20.0f;
    float mcY = config.mapSize * 0.5f + 60.0f;
    float mR  = config.mapSize * 0.5f;
    if (!inCircle(sx, sy, mcX, mcY, mR)) return;

    // ④ 屏幕坐标 → 世界坐标
    float wx, wz;
    screenToWorld(sx, sy, wx, wz);
    float wy = playerPos.y;

    // ⑤ 检查是否点到传送点
    for (const auto &wp : config.waypoints) {
        float dx = wp.x - playerPos.x;
        float dz = wp.z - playerPos.z;
        if (config.rotateWithPlayer) {
            float rad = playerYaw * (M_PI / 180.0f);
            float rx = dx * std::cos(rad) - dz * std::sin(rad);
            float ry = dx * std::sin(rad) + dz * std::cos(rad);
            dx = rx; dz = ry;
        }
        float psx = mcX + dx / config.mapScale;
        float psy = mcY - dz / config.mapScale;
        if (std::hypot(sx - psx, sy - psy) < 18.0f) {
            getSelf().getLogger().info("[MiniMap] teleport -> {}", wp.name);
            teleportToWaypoint(wp);
            return;
        }
    }

    // ⑥ 空白区域 → 创建新传送点
    char buf[64];
    std::snprintf(buf, sizeof(buf), "WP_%.0f_%.0f_%.0f", wx, wy, wz);
    addWaypoint(std::string(buf), wx, wy, wz);
}

// ============================================================================
// 屏幕坐标 → 世界坐标
// ============================================================================
void MyMod::screenToWorld(float sx, float sy, float &wx, float &wz) {
    int W = config.$screenW, H = config.$screenH;
    float mcX = W - config.mapSize * 0.5f - 20.0f;
    float mcY = config.mapSize * 0.5f + 60.0f;
    float dx = (sx - mcX) * config.mapScale;
    float dz = (mcY - sy) * config.mapScale;

    if (config.rotateWithPlayer) {
        float rad = -playerYaw * (M_PI / 180.0f);
        wx = playerPos.x + dx * std::cos(rad) - dz * std::sin(rad);
        wz = playerPos.z + dx * std::sin(rad) + dz * std::cos(rad);
    } else {
        wx = playerPos.x + dx;
        wz = playerPos.z + dz;
    }
}

// ============================================================================
// 绘制 UI（由 eglSwapBuffers Hook 调用）
// ============================================================================
void MyMod::drawUI() {
    std::lock_guard<std::mutex> lk(mtx);
    int W = config.$screenW, H = config.$screenH;
    if (W == 0 || H == 0) return;

    // ---- 保存 GL 状态 ----
    // glPushAttrib / glEnable(GL_BLEND) 等（占位，实际需要 GLES 调用）

    // ====== 按钮 ======
    float bx = config.btnX * W;
    float by = config.btnY * H;
    float br = config.btnSize * 0.5f;
    uint8_t ba = (uint8_t)(config.btnAlpha * 255);
    drawFillCircle(bx, by, br, 50, 50, 50, ba);
    drawCircle(bx, by, br, 180, 180, 180, ba);
    // 图标：空心框 / X
    float sq = br * 0.35f;
    if (config.$minimapOpen) {
        drawFillCircle(bx - sq, by - sq, br * 0.12f, 255, 80, 80, 200);
        drawFillCircle(bx + sq, by - sq, br * 0.12f, 255, 80, 80, 200);
        drawFillCircle(bx - sq, by + sq, br * 0.12f, 255, 80, 80, 200);
        drawFillCircle(bx + sq, by + sq, br * 0.12f, 255, 80, 80, 200);
    } else {
        drawFillCircle(bx, by, br * 0.2f, 100, 200, 255, 200);
    }

    if (!config.$minimapOpen) return;

    // ====== 小地图 ======
    float mx = W - config.mapSize * 0.5f - 20.0f;
    float my = config.mapSize * 0.5f + 60.0f;
    float mr = config.mapSize * 0.5f;
    uint8_t ma = (uint8_t)(config.mapAlpha * 255);

    drawFillCircle(mx, my, mr, 10, 10, 20, ma);   // 背景
    drawCircle(mx, my, mr, 200, 200, 200, ma);      // 边框

    // 方向标
    float dlen = mr * 0.85f;
    float baseA = config.rotateWithPlayer ? (playerYaw * M_PI / 180.0f) : 0.0f;
    for (int i = 0; i < 4; ++i) {
        float a = baseA + i * M_PI / 2.0f;
        drawFillCircle(mx + std::sin(a) * dlen, my - std::cos(a) * dlen,
                       3.0f, 180, 180, 180, 180);
    }
    // 北（红点）
    drawFillCircle(mx + std::sin(baseA) * dlen, my - std::cos(baseA) * dlen,
                   5.0f, 255, 50, 50, 220);

    // 玩家三角
    float angle = config.rotateWithPlayer ? 0.0f : (playerYaw * M_PI / 180.0f);
    float tipX = mx + std::sin(angle) * 8.0f;
    float tipY = my - std::cos(angle) * 8.0f;
    drawFillCircle(tipX, tipY, 4.5f, 255, 255, 255, 240);

    // 传送点
    for (const auto &wp : config.waypoints) {
        float dx = wp.x - playerPos.x;
        float dz = wp.z - playerPos.z;
        if (config.rotateWithPlayer) {
            float rad = playerYaw * (M_PI / 180.0f);
            float rx = dx * std::cos(rad) - dz * std::sin(rad);
            float ry = dx * std::sin(rad) + dz * std::cos(rad);
            dx = rx; dz = ry;
        }
        float sx = mx + dx / config.mapScale;
        float sy = my - dz / config.mapScale;
        // 裁剪
        float dist = std::hypot(sx - mx, sy - my);
        if (dist > mr - 6.0f) {
            float ea = std::atan2(sy - my, sx - mx);
            sx = mx + std::cos(ea) * (mr - 6.0f);
            sy = my + std::sin(ea) * (mr - 6.0f);
        }
        uint8_t cr = 255, cg = 80, cb = 80;
        hexToRGB(wp.color, cr, cg, cb);
        drawFillCircle(sx, sy, 6.0f, cr, cg, cb, 140);
        drawFillCircle(sx, sy, 3.0f, cr, cg, cb, 230);
    }
}

// ============================================================================
// 传送点管理
// ============================================================================
void MyMod::addWaypoint(const std::string &name, float x, float y, float z) {
    std::lock_guard<std::mutex> lk(mtx);
    config.waypoints.push_back({name, x, y, z, 0, "#FF4444"});
    saveCfg();
}

void MyMod::removeWaypoint(size_t i) {
    std::lock_guard<std::mutex> lk(mtx);
    if (i < config.waypoints.size()) {
        config.waypoints.erase(config.waypoints.begin() + i);
        saveCfg();
    }
}

void MyMod::teleportToWaypoint(const Waypoint &wp) {
    // ============================================================
    // 传送实现 — 两个方案任选其一：
    //
    // 方案①（推荐）：Hook 命令系统，发送 /tp 指令
    //   符号名：?executeCommand@ServerCommands@@...
    //   或：   _ZN15ServerCommands14executeCommandE...
    //   然后调用：executeCommand("/tp @p x y z")
    //
    // 方案②：直接修改玩家坐标
    //   找到 LocalPlayer 指针，调用 Actor::setPos(Vec3{x,y,z})
    //   符号名：?setPos@Actor@@UEAAXAEBVVec3@@@Z
    // ============================================================
    getSelf().getLogger().info(
        "[MiniMap] TELEPORT to {} ({:.0f}, {:.0f}, {:.0f}) — "
        "[需要接入命令Hook或setPos Hook]",
        wp.name, wp.x, wp.y, wp.z);
}

// ============================================================================
// 保存配置（$ 前缀字段不保存）
// ============================================================================
void MyMod::saveCfg() {
    MinimapConfig save = config;
    save.$minimapOpen = false;
    save.$screenW = 1080;
    save.$screenH = 1920;
    pl::config::ConfigFile<MinimapConfig> cf(save);
    cf.save();
}

// ============================================================================
// 简单数学工具
// ============================================================================
bool MyMod::inCircle(float px, float py, float cx, float cy, float r) {
    return (px - cx) * (px - cx) + (py - cy) * (py - cy) <= r * r;
}

bool MyMod::hexToRGB(const std::string &h, uint8_t &R, uint8_t &G, uint8_t &B) {
    if (h.size() != 7 || h[0] != '#') return false;
    unsigned int v;
    std::stringstream ss;
    ss << std::hex << h.substr(1);
    ss >> v;
    if (ss.fail()) return false;
    R = (v >> 16) & 0xFF;
    G = (v >> 8) & 0xFF;
    B = v & 0xFF;
    return true;
}

// ============================================================================
// 绘制占位（需替换为真实 GLES 调用）
// ============================================================================
void MyMod::drawCircle(float, float, float, uint8_t, uint8_t, uint8_t, uint8_t) {}
void MyMod::drawFillCircle(float, float, float, uint8_t, uint8_t, uint8_t, uint8_t) {}

} // namespace my_mod
