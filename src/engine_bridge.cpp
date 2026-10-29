#include "engine_bridge.hpp"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

namespace cssxy {
Engine engine;
namespace {
template<class T> T Read(const void* address,T fallback={}) {
    __try { return *static_cast<const T*>(address); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return fallback; }
}
template<class F> F Slot(void* object,int index) {
    return object?reinterpret_cast<F>(Read<void*>(static_cast<std::byte*>(Read<void*>(object))+index*sizeof(void*))):nullptr;
}
bool Executable(void* address) {
    MEMORY_BASIC_INFORMATION memory{};
    return address && VirtualQuery(address,&memory,sizeof(memory)) && memory.State==MEM_COMMIT &&
        (memory.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) &&
        !(memory.Protect&(PAGE_GUARD|PAGE_NOACCESS));
}
bool Readable(const void* address,std::size_t bytes) {
    MEMORY_BASIC_INFORMATION memory{};
    return address && VirtualQuery(address,&memory,sizeof(memory)) && memory.State==MEM_COMMIT &&
        !(memory.Protect&(PAGE_NOACCESS|PAGE_GUARD)) &&
        reinterpret_cast<std::uintptr_t>(address)+bytes<=reinterpret_cast<std::uintptr_t>(memory.BaseAddress)+memory.RegionSize;
}
bool InModule(void* address,HMODULE module) {
    MEMORY_BASIC_INFORMATION memory{};
    return module && Executable(address) && VirtualQuery(address,&memory,sizeof(memory)) && memory.AllocationBase==module;
}
bool ForeignDetour(void* address,HMODULE owner) {
    auto code=static_cast<std::byte*>(address);
    if (!Readable(code,14)) return true;
    void* destination{};
    if (Read<unsigned char>(code)==0xe9) destination=code+5+Read<std::int32_t>(code+1);
    else if (Read<unsigned char>(code)==0xeb) destination=code+2+Read<std::int8_t>(code+1);
    else if (Read<unsigned char>(code)==0xff && Read<unsigned char>(code+1)==0x25)
        destination=Read<void*>(code+6+Read<std::int32_t>(code+2));
    return destination && !InModule(destination,owner);
}
void* Interface(const wchar_t* module,const char* version) {
    const auto handle=GetModuleHandleW(module);
    const auto create=handle?reinterpret_cast<void*(*)(const char*,int*)>(GetProcAddress(handle,"CreateInterface")):nullptr;
    return create?create(version,nullptr):nullptr;
}
struct RecvTable;
struct RecvProp {
    const char* name; int type,flags,stringSize; bool insideArray; std::byte pad[3];
    void *extra,*array,*arrayLength,*proxy,*dataProxy; RecvTable* table;
    int offset,stride,count; std::byte pad2[4]; const char* parent;
};
struct RecvTable {
    RecvProp* props; int count; std::byte pad[4]; void* decoder; const char* name;
    bool initialized,mainList; std::byte tail[6];
};
struct ClientClass {
    void *create,*event; const char* name; RecvTable* table; ClientClass* next; int id; std::byte pad[4];
};
static_assert(sizeof(RecvProp)==96 && offsetof(RecvProp,offset)==72 && sizeof(RecvTable)==40 && sizeof(ClientClass)==48);
void Walk(RecvTable* table,int base,std::unordered_map<std::string,int>& fields,std::set<RecvTable*>& stack,int depth=0) {
    if (!table || depth>16 || !Readable(table,sizeof(*table)) || stack.contains(table)) return;
    const int count=Read<int>(&table->count);
    auto props=Read<RecvProp*>(&table->props);
    if (count<0 || count>4096 || !Readable(props,static_cast<std::size_t>(count)*sizeof(RecvProp))) return;
    stack.insert(table);
    for (int i=0;i<count;++i) {
        const auto prop=Read<RecvProp>(props+i);
        if (!Readable(prop.name,1) || prop.offset<0 || prop.offset>0x20000) continue;
        char name[256]{};
        for (int n=0;n<255;++n) { name[n]=Read<char>(prop.name+n); if (!name[n]) break; }
        if (prop.table) Walk(prop.table,base+prop.offset,fields,stack,depth+1);
        fields.try_emplace(name,base+prop.offset);
    }
    stack.erase(table);
}
}
bool Engine::Initialize(std::string& error) {
    initialized_=false; offsets_.clear();
    const auto module=GetModuleHandleW(L"client.dll");
    wchar_t clientPath[32768]{};
    if (!module || !GetModuleFileNameW(module,clientPath,32768)) {
        error="CS:S client module is not available"; return false;
    }
    const auto steamInfo=std::filesystem::path(clientPath).parent_path().parent_path().parent_path()/"steam.inf";
    std::ifstream info(steamInfo); std::string line; bool supported=false;
    while (std::getline(info,line)) if (line.rfind("PatchVersion=",0)==0) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        supported=line.substr(13)=="11003710"; break;
    }
    if (!supported) { error="Unsupported CS:S build; expected 11003710"; return false; }
    client_=Interface(L"client.dll","VClient017");
    entities_=Interface(L"client.dll","VClientEntityList003");
    engine_=Interface(L"engine.dll","VEngineClient014");
    if (!client_ || !entities_ || !engine_) { error="Required client interfaces unavailable"; return false; }
    const auto hud=Slot<std::byte*>(client_,10);
    if (!Executable(hud) || !Readable(hud,16) || Read<unsigned char>(hud)!=0x48 ||
        Read<unsigned char>(hud+1)!=0x8b || Read<unsigned char>(hud+2)!=0x0d) {
        error="Unverified client input interface; hooks remain disabled"; return false;
    }
    const auto mode=Read<void*>(hud+7+Read<std::int32_t>(hud+3));
    createMove_=Slot<void*>(mode,21);
    if (!InModule(createMove_,module) || ForeignDetour(createMove_,module)) {
        error="Input callback unavailable or owned by another client modification"; return false;
    }
    const auto getClasses=Slot<ClientClass*(*)(void*)>(client_,8);
    if (!InModule(reinterpret_cast<void*>(getClasses),module)) { error="Client classes unavailable"; return false; }
    auto cls=getClasses(client_);
    std::set<ClientClass*> visited;
    for (int i=0;cls && i<4096;++i) {
        if (!Readable(cls,sizeof(*cls)) || !visited.insert(cls).second) break;
        const auto row=Read<ClientClass>(cls);
        char name[128]{};
        for (int n=0;row.name && n<127;++n) { name[n]=Read<char>(row.name+n); if(!name[n]) break; }
        if (std::strcmp(name,"CCSPlayer")==0) { std::set<RecvTable*> stack; Walk(row.table,0,offsets_,stack); break; }
        cls=row.next;
    }
    for (const char* property:{"m_vecVelocity[0]","m_iHealth","m_lifeState"}) if (!offsets_.contains(property)) {
        error=std::string("Required player field missing: ")+property; return false;
    }
    initialized_=true; error.clear(); return true;
}
bool Engine::ReadPlayer(PlayerState& state,int& health) {
    if (!initialized_) return false;
    const auto inGame=Slot<bool(*)(void*)>(engine_,26);
    if (!inGame || !inGame(engine_)) return false;
    const auto localIndex=Slot<int(*)(void*)>(engine_,12);
    const auto getEntity=Slot<void*(*)(void*,int)>(entities_,3);
    if (!localIndex || !getEntity) return false;
    auto player=static_cast<std::byte*>(getEntity(entities_,localIndex(engine_)));
    if (!player) return false;
    state.velocity=Read<Vec3>(player+offsets_.at("m_vecVelocity[0]"),{NAN,NAN,NAN});
    health=Read<int>(player+offsets_.at("m_iHealth"));
    const auto life=Read<std::uint8_t>(player+offsets_.at("m_lifeState"),255);
    return life==0 && health>0 && health<100000 && std::isfinite(state.velocity.x) &&
        std::isfinite(state.velocity.y) && std::isfinite(state.velocity.z);
}
bool Engine::ClientCommand(const char* command) const {
    if (!initialized_ || !command || (std::strcmp(command,"gameui_activate")!=0 && std::strcmp(command,"gameui_hide")!=0)) return false;
    if (!Readable(engine_,sizeof(void*)) || !Readable(Read<void*>(engine_),107*sizeof(void*))) return false;
    const auto queue=Slot<void(*)(void*,const char*)>(engine_,106);
    if (!InModule(reinterpret_cast<void*>(queue),GetModuleHandleW(L"engine.dll"))) return false;
    queue(engine_,command); return true;
}
}
