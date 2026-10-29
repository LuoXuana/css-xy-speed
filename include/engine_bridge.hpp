#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace cssxy {
struct Vec3 { float x{},y{},z{}; };
struct PlayerState { Vec3 velocity; };
// CS:S x64 build 11003710 input ABI. The HUD never writes these fields.
struct UserCmd {
    void* vptr; int commandNumber,tickCount; Vec3 angles;
    float forward,side,up; std::uint32_t buttons; std::uint8_t impulse,pad[3];
    int weaponSelect,weaponSubtype,randomSeed; short mouseX,mouseY;
    bool predicted; std::uint8_t tail[7];
};
static_assert(sizeof(UserCmd)==72 && offsetof(UserCmd,angles)==16 && offsetof(UserCmd,buttons)==40);
class Engine {
public:
    bool Initialize(std::string& error);
    bool ReadPlayer(PlayerState& state,int& health);
    // Queue only the local menu commands used by the position editor.
    bool ClientCommand(const char* command) const;
    void* CreateMoveAddress() const { return createMove_; }
private:
    void *client_{},*entities_{},*engine_{},*createMove_{};
    std::unordered_map<std::string,int> offsets_;
    bool initialized_{};
};
extern Engine engine;
}
