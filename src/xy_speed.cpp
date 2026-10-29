// Standalone, read-only horizontal speed HUD for the supported CS:S x64 build.
// Gameplay input is never modified. Editing uses the game's own menu to release
// its cursor; mouse messages are consumed only while positioning the HUD.
#include "engine_bridge.hpp"
#include <MinHook.h>
#include <d3d9.h>
#include <imgui.h>
#include <backends/imgui_impl_dx9.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace {
using MoveFn = bool(__fastcall*)(void*, float, cssxy::UserCmd*);
using EndSceneFn = HRESULT(WINAPI*)(IDirect3DDevice9*);
using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
HMODULE selfModule{};
std::filesystem::path directory;
std::atomic_bool stopping{}, resetting{}, visible{true};
std::atomic_bool editing{}, closePending{}, savePending{}, saveFailed{};
std::atomic<HWND> gameWindow{};
std::atomic<WNDPROC> previousWndProc{};
constexpr UINT editMessage=WM_APP+0x583;
std::atomic_uint activeCallbacks{};
std::mutex sampleMutex, renderMutex, logMutex;
struct Sample { float xy{}; ULONGLONG time{}; bool valid{}; } sample;
struct Settings { float x=.5f, y=.72f, size=32.f; int decimals=2; } settings;
MoveFn originalMove{};
EndSceneFn originalEndScene{};
ResetFn originalReset{};
void *moveTarget{}, *endTarget{}, *resetTarget{};
bool moveCreated{}, endCreated{}, resetCreated{}, minhookReady{};
ImGuiContext* context{};
IDirect3DDevice9* actualDevice{}; // The ImGui backend holds a reference.
ULONGLONG previousFrame{};
bool dragging{}, previousLeft{};
ImVec2 dragOffset{};

void Log(const std::string& message) {
    std::lock_guard lock(logMutex);
    std::ofstream out(directory / "xy-speed.log", std::ios::app);
    out << GetTickCount64() << " " << message << '\n';
}
struct Callback {
    Callback() { activeCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~Callback() { activeCallbacks.fetch_sub(1, std::memory_order_acq_rel); }
};
struct CurrentContext {
    ImGuiContext* previous=ImGui::GetCurrentContext();
    explicit CurrentContext(ImGuiContext* value) { ImGui::SetCurrentContext(value); }
    ~CurrentContext() { ImGui::SetCurrentContext(previous); }
};
bool ForegroundGame() {
    DWORD owner{};
    const HWND window=GetForegroundWindow();
    return window && GetWindowThreadProcessId(window, &owner) && owner==GetCurrentProcessId();
}
float ReadSetting(const wchar_t* key, const wchar_t* fallback, float minimum, float maximum) {
    wchar_t text[64]{};
    GetPrivateProfileStringW(L"HUD", key, fallback, text, 64, (directory / "xy-speed.ini").c_str());
    wchar_t* end{};
    const float value=wcstof(text, &end);
    while (end && (*end==L' ' || *end==L'\t')) ++end;
    if (end==text || !end || *end || !std::isfinite(value)) return wcstof(fallback, nullptr);
    return std::clamp(value, minimum, maximum);
}
void ReloadSettings() {
    Settings next;
    next.x=ReadSetting(L"X", L"0.50", 0.f, 1.f);
    next.y=ReadSetting(L"Y", L"0.72", 0.f, 1.f);
    next.size=ReadSetting(L"FontSize", L"32", 12.f, 96.f);
    next.decimals=static_cast<int>(ReadSetting(L"Decimals", L"2", 0.f, 2.f));
    { std::lock_guard lock(renderMutex); settings=next; }
    Log("HUD settings loaded (F8 position editor, Shift+F8 visibility, F9 reload, F11 unload)");
}

void SavePosition() {
    Settings copy;
    { std::lock_guard lock(renderMutex); copy=settings; }
    wchar_t x[32]{}, y[32]{};
    swprintf_s(x,L"%.6f",copy.x); swprintf_s(y,L"%.6f",copy.y);
    const auto file=directory/"xy-speed.ini";
    const bool xSaved=WritePrivateProfileStringW(L"HUD",L"X",x,file.c_str())!=FALSE;
    const bool ySaved=WritePrivateProfileStringW(L"HUD",L"Y",y,file.c_str())!=FALSE;
    saveFailed.store(!xSaved || !ySaved);
    Log(xSaved && ySaved?"HUD position saved":"Could not save HUD position; check INI file write permissions");
}
void EndEditing() {
    closePending.store(false);
    if (editing.exchange(false)) savePending.store(true);
}
LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    Callback callback;
    const auto forward=previousWndProc.load();
    if (!stopping.load(std::memory_order_acquire)) {
        if (message==editMessage) {
            if (editing.load()) {
                if ((GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0) {
                    closePending.store(true); return 0;
                }
                EndEditing();
                cssxy::engine.ClientCommand("gameui_hide");
            } else if (ForegroundGame()) {
                // The existing game menu releases raw/legacy mouse capture and
                // stops camera input. No cursor detours or sensitivity changes.
                if (cssxy::engine.ClientCommand("gameui_activate")) {
                    visible.store(true); editing.store(true); saveFailed.store(false);
                } else Log("Could not open the game menu for HUD positioning");
            }
            return 0;
        }
        if (message==WM_KILLFOCUS || (message==WM_ACTIVATEAPP && !wparam)) EndEditing();
        const bool keyboard=(message>=WM_KEYFIRST && message<=WM_KEYLAST);
        if (editing.load()) {
            if (keyboard && wparam==VK_ESCAPE) {
                if ((GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0) {
                    closePending.store(true); return 0;
                }
                // Let the actual menu process Esc and return control normally.
                if (message==WM_KEYDOWN) EndEditing();
            } else {
                if (keyboard || (message>=WM_MOUSEFIRST && message<=WM_MOUSELAST)) return 0;
                if (message==WM_INPUT) return DefWindowProcW(window,message,wparam,lparam);
            }
        }
        // These keys are handled by the worker, and must not also trigger a
        // user game binding (especially while entering/exiting the editor).
        if (keyboard && (wparam==VK_F8 || wparam==VK_F9 || wparam==VK_F11)) return 0;
    }
    return forward?CallWindowProcW(forward,window,message,wparam,lparam):DefWindowProcW(window,message,wparam,lparam);
}
bool InstallWindowProc(HWND window) {
    if (gameWindow.load()==window && previousWndProc.load())
        return reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC))==&WindowProc;
    previousWndProc.store(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC)));
    if (!previousWndProc.load()) return false;
    SetLastError(0);
    const auto replaced=SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&WindowProc));
    if (!replaced) { previousWndProc.store(nullptr); return false; }
    previousWndProc.store(reinterpret_cast<WNDPROC>(replaced));
    gameWindow.store(window);
    return true;
}

bool __fastcall OnMove(void* mode, float seconds, cssxy::UserCmd* command) {
    Callback callback;
    const bool result=originalMove(mode, seconds, command);
    if (!stopping.load(std::memory_order_acquire)) {
        // Sample on the client input thread. Preserve every input byte and the
        // engine's original return value; this hook never owns player control.
        cssxy::PlayerState state{}; int health{};
        Sample next;
        next.time=GetTickCount64();
        next.valid=cssxy::engine.ReadPlayer(state, health);
        if (next.valid) {
            next.xy=std::hypot(state.velocity.x, state.velocity.y);
            next.valid=std::isfinite(next.xy);
        }
        std::lock_guard lock(sampleMutex);
        sample=next;
    }
    return result;
}

ImFont* LoadHudFont(ImGuiIO& io) {
    ImFontConfig fontConfig;
    fontConfig.SizePixels=32.f;
    fontConfig.OversampleH=3; fontConfig.OversampleV=2;
    fontConfig.PixelSnapH=false;
    // The embedded default is ProggyClean, a deliberately pixel-style font.
    // Load a normal Windows outline font; don't redistribute system font files.
    wchar_t windowsPath[MAX_PATH]{};
    GetWindowsDirectoryW(windowsPath,MAX_PATH);
    ImFont* smoothFont{};
    for (const auto name:{L"segoeui.ttf",L"tahoma.ttf",L"arial.ttf"}) {
        const auto file=std::filesystem::path(windowsPath)/L"Fonts"/name;
        if (!std::filesystem::is_regular_file(file)) continue;
        const auto utf8=file.u8string();
        smoothFont=io.Fonts->AddFontFromFileTTF(reinterpret_cast<const char*>(utf8.c_str()),32.f,&fontConfig);
        if (smoothFont) break;
    }
    if (!smoothFont) {
        smoothFont=io.Fonts->AddFontDefault(&fontConfig);
        Log("Windows outline font unavailable; using embedded font fallback");
    }
    return smoothFont;
}
bool InitializeRenderer(IDirect3DDevice9* device) {
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (FAILED(device->GetCreationParameters(&creation))) return false;
    DWORD owner{}; RECT client{};
    if (!creation.hFocusWindow || !GetWindowThreadProcessId(creation.hFocusWindow, &owner) ||
        owner!=GetCurrentProcessId() || !GetClientRect(creation.hFocusWindow, &client) ||
        client.right<200 || client.bottom<200) return false;
    if (!InstallWindowProc(creation.hFocusWindow)) {
        Log("Could not install HUD editor window handler"); return false;
    }
    IMGUI_CHECKVERSION();
    const auto previous=ImGui::GetCurrentContext();
    context=ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    auto& io=ImGui::GetIO();
    io.IniFilename=nullptr; io.LogFilename=nullptr;
    LoadHudFont(io);
    const bool initialized=ImGui_ImplDX9_Init(device);
    if (!initialized) { ImGui::DestroyContext(context); context=nullptr; }
    ImGui::SetCurrentContext(previous);
    if (initialized) {
        actualDevice=device; previousFrame=GetTickCount64();
        Log("D3D9 HUD initialized; F8 drag editor available; no cursor detours installed");
    }
    return initialized;
}
void Draw(IDirect3DDevice9* device) {
    if (resetting.load() || !visible.load() || !ForegroundGame() || FAILED(device->TestCooperativeLevel())) return;
    Sample current;
    { std::lock_guard lock(sampleMutex); current=sample; }
    const auto now=GetTickCount64();
    D3DVIEWPORT9 viewport{};
    if (FAILED(device->GetViewport(&viewport)) || viewport.Width<200 || viewport.Height<200) return;
    if (!context && !InitializeRenderer(device)) return;
    if (device!=actualDevice) return;
    const bool edit=editing.load();
    if (!edit && (!current.valid || now-current.time>500)) return;
    CurrentContext use(context);
    auto& io=ImGui::GetIO();
    io.DisplaySize={static_cast<float>(viewport.Width), static_cast<float>(viewport.Height)};
    io.DeltaTime=std::clamp(static_cast<float>(now-previousFrame)*.001f, .001f, .25f);
    previousFrame=now;
    POINT cursor{}; RECT client{};
    ImVec2 mouse{-10000.f,-10000.f};
    const bool mouseValid=edit && GetCursorPos(&cursor) && ScreenToClient(gameWindow.load(),&cursor) &&
        GetClientRect(gameWindow.load(),&client) && client.right>0 && client.bottom>0;
    if (mouseValid) {
        mouse={cursor.x*io.DisplaySize.x/client.right,cursor.y*io.DisplaySize.y/client.bottom};
    }
    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    char text[96]{};
    std::snprintf(text, sizeof(text), "%.*f u/s", settings.decimals, current.valid?current.xy:0.f);
    const auto font=ImGui::GetFont();
    const auto extent=font->CalcTextSizeA(settings.size, FLT_MAX, 0, text);
    ImVec2 position{
        std::clamp(io.DisplaySize.x*settings.x-extent.x*.5f, 0.f, std::max(0.f, io.DisplaySize.x-extent.x)),
        std::clamp(io.DisplaySize.y*settings.y-extent.y*.5f, 0.f, std::max(0.f, io.DisplaySize.y-extent.y))};
    const bool left=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
    const bool hover=mouse.x>=position.x-10 && mouse.x<=position.x+extent.x+10 &&
        mouse.y>=position.y-8 && mouse.y<=position.y+extent.y+8;
    if (edit) {
        if (left && !previousLeft && hover) {
            dragging=true; dragOffset={mouse.x-position.x,mouse.y-position.y};
        }
        if (dragging && left && mouseValid) {
            position.x=std::clamp(mouse.x-dragOffset.x,0.f,std::max(0.f,io.DisplaySize.x-extent.x));
            position.y=std::clamp(mouse.y-dragOffset.y,0.f,std::max(0.f,io.DisplaySize.y-extent.y));
            settings.x=(position.x+extent.x*.5f)/io.DisplaySize.x;
            settings.y=(position.y+extent.y*.5f)/io.DisplaySize.y;
        }
        if (dragging && !left) { dragging=false; savePending.store(true); }
    } else dragging=false;
    previousLeft=left;
    auto draw=ImGui::GetBackgroundDrawList();
    if (edit) {
        draw->AddRectFilled({position.x-10,position.y-8},{position.x+extent.x+10,position.y+extent.y+8},IM_COL32(12,18,28,220),6.f);
        draw->AddRect({position.x-10,position.y-8},{position.x+extent.x+10,position.y+extent.y+8},IM_COL32(80,220,200,255),6.f,0,2.f);
        const char* hint=saveFailed.load()?"Position save failed - check xy-speed.ini permissions":"Drag speed text | Release to save | F8 / Esc: done";
        draw->AddText({16,16},saveFailed.load()?IM_COL32(255,100,100,255):IM_COL32(255,255,255,255),hint);
        // A visible pointer even when the game uses a software VGUI cursor.
        draw->AddTriangleFilled(mouse,{mouse.x+3,mouse.y+17},{mouse.x+13,mouse.y+12},IM_COL32(255,255,255,255));
    }
    draw->AddText(font, settings.size, {position.x+2, position.y+2}, IM_COL32(0,0,0,230), text);
    draw->AddText(font, settings.size, position, IM_COL32(255,255,255,255), text);
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}
HRESULT WINAPI OnEndScene(IDirect3DDevice9* device) {
    Callback callback;
    if (!stopping.load(std::memory_order_acquire)) {
        std::lock_guard lock(renderMutex);
        if (!stopping.load(std::memory_order_acquire)) Draw(device);
    }
    return originalEndScene(device);
}
HRESULT WINAPI OnReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters) {
    Callback callback;
    {
        std::lock_guard lock(renderMutex);
        resetting.store(true);
        if (context && device==actualDevice) {
            CurrentContext use(context); ImGui_ImplDX9_InvalidateDeviceObjects();
        }
    }
    // Do not hold the render mutex while the original Reset sends window messages.
    const auto result=originalReset(device, parameters);
    {
        std::lock_guard lock(renderMutex);
        if (SUCCEEDED(result) && !stopping.load(std::memory_order_acquire) && context && device==actualDevice) {
            CurrentContext use(context); ImGui_ImplDX9_CreateDeviceObjects();
        }
        resetting.store(false);
    }
    return result;
}

bool RuntimeMethod(void* address, HMODULE runtime) {
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || !runtime || VirtualQuery(address,&memory,sizeof(memory))!=sizeof(memory) ||
        memory.State!=MEM_COMMIT || memory.Type!=MEM_IMAGE || memory.AllocationBase!=runtime ||
        (memory.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
    const DWORD flags=memory.Protect&0xffu;
    return flags==PAGE_EXECUTE || flags==PAGE_EXECUTE_READ ||
        flags==PAGE_EXECUTE_READWRITE || flags==PAGE_EXECUTE_WRITECOPY;
}
bool FindD3DMethods() {
    const wchar_t* className=L"CSS_XY_SPEED_D3D9_PROBE";
    WNDCLASSEXW wc{};
    wc.cbSize=sizeof(wc); wc.lpfnWndProc=DefWindowProcW; wc.hInstance=selfModule; wc.lpszClassName=className;
    if (!RegisterClassExW(&wc)) return false;
    const HWND window=CreateWindowExW(0,className,L"XY HUD probe",WS_OVERLAPPEDWINDOW,
        0,0,128,128,nullptr,nullptr,selfModule,nullptr); // Never shown or focused.
    if (!window) { UnregisterClassW(className,selfModule); return false; }
    const auto runtime=GetModuleHandleW(L"d3d9.dll");
    auto parameters=[window] {
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=window;
        pp.BackBufferFormat=D3DFMT_UNKNOWN; pp.BackBufferWidth=pp.BackBufferHeight=32;
        pp.BackBufferCount=1; pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
        return pp;
    };
    auto accept=[runtime](IDirect3DDevice9* device) {
        const auto table=*reinterpret_cast<void***>(device);
        if (!RuntimeMethod(table[16],runtime) || !RuntimeMethod(table[42],runtime)) return false;
        resetTarget=table[16]; endTarget=table[42]; return true;
    };
    struct Attempt { D3DDEVTYPE type; DWORD flags; };
    constexpr Attempt attempts[]{
        {D3DDEVTYPE_HAL,D3DCREATE_SOFTWARE_VERTEXPROCESSING},
        {D3DDEVTYPE_HAL,D3DCREATE_HARDWARE_VERTEXPROCESSING},
        {D3DDEVTYPE_NULLREF,D3DCREATE_SOFTWARE_VERTEXPROCESSING},
        {D3DDEVTYPE_REF,D3DCREATE_SOFTWARE_VERTEXPROCESSING}};
    if (auto api=Direct3DCreate9(D3D_SDK_VERSION)) {
        for (const auto& attempt:attempts) {
            auto pp=parameters(); IDirect3DDevice9* device{};
            api->CreateDevice(D3DADAPTER_DEFAULT,attempt.type,window,
                attempt.flags|D3DCREATE_NOWINDOWCHANGES,&pp,&device);
            if (device) { const bool valid=accept(device); device->Release(); if(valid) break; }
        }
        api->Release();
    }
    if (!endTarget) {
        using CreateExFn=HRESULT(WINAPI*)(UINT,IDirect3D9Ex**);
        const auto createEx=runtime?reinterpret_cast<CreateExFn>(GetProcAddress(runtime,"Direct3DCreate9Ex")):nullptr;
        IDirect3D9Ex* api{};
        if (createEx && SUCCEEDED(createEx(D3D_SDK_VERSION,&api)) && api) {
            for (const auto& attempt:attempts) {
                auto pp=parameters(); IDirect3DDevice9Ex* device{};
                api->CreateDeviceEx(D3DADAPTER_DEFAULT,attempt.type,window,
                    attempt.flags|D3DCREATE_NOWINDOWCHANGES,&pp,nullptr,&device);
                if (device) { const bool valid=accept(device); device->Release(); if(valid) break; }
            }
            api->Release();
        }
    }
    DestroyWindow(window); UnregisterClassW(className,selfModule);
    return endTarget && resetTarget;
}
bool CreateHook(void* target, void* callback, void** original, bool& created) {
    const auto status=MH_CreateHook(target,callback,original);
    created=status==MH_OK;
    if (!created) Log(std::string("Hook creation failed: ")+MH_StatusToString(status));
    return created;
}
bool Cleanup() {
    stopping.store(true,std::memory_order_release);
    EndEditing();
    if (savePending.exchange(false)) SavePosition();
    bool safe=true;
    const auto window=gameWindow.load();
    const auto forward=previousWndProc.load();
    if (window && forward && IsWindow(window)) {
        const auto current=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC));
        if (current==&WindowProc) {
            if (!SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(forward))) safe=false;
        } else if (current!=forward) safe=false; // Another subclass may retain our callback.
    }
    const std::pair<void*,bool> hooks[]{{moveTarget,moveCreated},{endTarget,endCreated},{resetTarget,resetCreated}};
    for (const auto& [target,created]:hooks) if (created) {
        const auto status=MH_DisableHook(target);
        if (status!=MH_OK && status!=MH_ERROR_DISABLED) safe=false;
    }
    if (!safe) return false;
    // All entry detours are now disabled. Give callbacks already inside the DLL
    // time to return, including their final epilogues, before freeing code.
    unsigned quiet=0;
    for (unsigned i=0;i<400 && quiet<20;++i) {
        Sleep(5);
        quiet=activeCallbacks.load(std::memory_order_acquire)==0?quiet+1:0;
    }
    if (quiet<20) return false;
    {
        std::lock_guard lock(renderMutex);
        if (context) {
            const auto previous=ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(context);
            ImGui_ImplDX9_Shutdown();
            ImGui::DestroyContext(context);
            if (previous!=context) ImGui::SetCurrentContext(previous);
            context=nullptr; actualDevice=nullptr;
        }
    }
    for (const auto& [target,created]:hooks) if (created && MH_RemoveHook(target)!=MH_OK) safe=false;
    if (safe && minhookReady && MH_Uninitialize()!=MH_OK) safe=false;
    return safe;
}
DWORD WINAPI Worker(void*) {
    wchar_t path[32768]{};
    GetModuleFileNameW(selfModule,path,32768);
    directory=std::filesystem::path(path).parent_path();
    Log("CSS XY Speed 0.1.1 starting; client velocity, XY=hypot(vx,vy), no VZ");
    bool ready=false;
    std::string error;
    if (!cssxy::engine.Initialize(error)) { /* Bridge reports the exact unsupported-build/interface reason. */ }
    else if (MH_Initialize()!=MH_OK) error="MinHook initialization failed";
    else {
        minhookReady=true;
        ReloadSettings();
        moveTarget=cssxy::engine.CreateMoveAddress();
        ready=FindD3DMethods() &&
            CreateHook(moveTarget,reinterpret_cast<void*>(&OnMove),reinterpret_cast<void**>(&originalMove),moveCreated) &&
            CreateHook(resetTarget,reinterpret_cast<void*>(&OnReset),reinterpret_cast<void**>(&originalReset),resetCreated) &&
            CreateHook(endTarget,reinterpret_cast<void*>(&OnEndScene),reinterpret_cast<void**>(&originalEndScene),endCreated);
        // Enable as a batch only after every original trampoline is available.
        if (ready) ready=MH_QueueEnableHook(moveTarget)==MH_OK && MH_QueueEnableHook(resetTarget)==MH_OK &&
            MH_QueueEnableHook(endTarget)==MH_OK && MH_ApplyQueued()==MH_OK;
        if (!ready) error="D3D9/input observation hook setup failed";
    }
    if (ready) {
        Log("READY; enter a map as a living player to see XY speed");
        bool oldToggle=true, oldReload=true, oldUnload=true;
        for (;;) {
            const bool toggle=(GetAsyncKeyState(VK_F8)&0x8000)!=0;
            const bool reload=(GetAsyncKeyState(VK_F9)&0x8000)!=0;
            const bool unload=(GetAsyncKeyState(VK_F11)&0x8000)!=0;
            if (ForegroundGame()) {
                if (toggle && !oldToggle) {
                    if ((GetAsyncKeyState(VK_SHIFT)&0x8000)!=0) {
                        if (!editing.load()) visible.store(!visible.load());
                    } else if (const auto window=gameWindow.load()) PostMessageW(window,editMessage,0,0);
                }
                if (reload && !oldReload) ReloadSettings();
                if (unload && !oldUnload) break;
            }
            oldToggle=toggle; oldReload=reload; oldUnload=unload;
            if (closePending.load() && (GetAsyncKeyState(VK_LBUTTON)&0x8000)==0) {
                if (const auto window=gameWindow.load()) {
                    closePending.store(false); PostMessageW(window,editMessage,0,0);
                }
            }
            if (savePending.exchange(false)) SavePosition();
            Sleep(25);
        }
    } else Log("FAILED: "+error);
    if (!Cleanup()) { Log("Unload deferred: hooks/callbacks still active; DLL retained until game exit"); return 0; }
    Log("UNLOADED; no control settings were changed");
    FreeLibraryAndExitThread(selfModule,0);
}
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,void*) {
    if (reason==DLL_PROCESS_ATTACH) {
        selfModule=module;
        const HANDLE thread=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
        if (!thread) return FALSE;
        CloseHandle(thread);
    }
    return TRUE;
}
