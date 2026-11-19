// Exercise the actual native window handler in an invisible test window. No
// input is sent to the desktop/game, and the Computer Use helper is not used.
#include "../src/xy_speed.cpp"
#include <iostream>
#include <stdexcept>

namespace {
int forwardedMouse{}, forwardedKeys{}, forwardedHotkeys{};
std::wstring forwardedText;
void Require(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}
LRESULT CALLBACK HostWindow(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if (message==WM_MOUSEMOVE) ++forwardedMouse;
    if (message==WM_KEYDOWN && wparam=='W') ++forwardedKeys;
    if (message==WM_CHAR || message==WM_SYSCHAR || message==WM_DEADCHAR ||
        message==WM_SYSDEADCHAR || message==WM_UNICHAR)
        forwardedText.push_back(static_cast<wchar_t>(wparam));
    if ((message==WM_KEYDOWN || message==WM_KEYUP || message==WM_SYSKEYDOWN || message==WM_SYSKEYUP) &&
        (wparam==VK_F8 || wparam==VK_F9 || wparam==VK_F11)) ++forwardedHotkeys;
    return DefWindowProcW(window,message,wparam,lparam);
}
}
int wmain() {
    HWND window{};
    try {
        const auto white=IM_COL32(255,255,255,255), red=IM_COL32(255,80,80,255);
        auto speed=TrackPhysicsSample({1000.f,1000,true,100},{});
        Require(SpeedColor(speed,1000)==white,"first physics sample is white");
        speed=TrackPhysicsSample({999.5f,1010,true,101},speed);
        Require(SpeedColor(speed,1010)==white,"physics loss below one unit stays white");
        speed=TrackPhysicsSample({998.5f,1020,true,102},speed);
        Require(SpeedColor(speed,1020)==red,"physics loss of exactly one unit turns red");
        // 100 Hz physics with several input/render callbacks per movement tick.
        // Red belongs to that tick, not to a single 600 Hz render frame.
        for (ULONGLONG frame=1021;frame<1030;++frame) {
            speed=TrackPhysicsSample({998.5f,frame,true,102},speed);
            Require(SpeedColor(speed,frame)==red,"repeated frames and input calls preserve current physics tick color");
        }
        speed=TrackPhysicsSample({998.5f,1030,true,103},speed);
        Require(SpeedColor(speed,1030)==white,"next physics tick at the same speed is immediately white");
        speed=TrackPhysicsSample({1000.f,1040,true,104},speed);
        Require(SpeedColor(speed,1040)==white,"next physics tick with increased speed is white");
        speed=TrackPhysicsSample({999.5f,1050,true,105},speed);
        Require(SpeedColor(speed,1050)==white,"small loss is not accumulated across physics ticks");
        speed=TrackPhysicsSample({998.5f,1060,true,106},speed);
        Require(SpeedColor(speed,1060)==red,"a new qualifying physics loss turns red");
        speed=TrackPhysicsSample({0.f,1070,false,-1},speed);
        Require(SpeedColor(speed,1070)==white,"invalid player clears loss indication");
        speed=TrackPhysicsSample({900.f,1080,true,1},speed);
        Require(SpeedColor(speed,1080)==white,"respawn starts a fresh physics baseline");
        speed=TrackPhysicsSample({800.f,1700,true,2},speed);
        Require(SpeedColor(speed,1700)==white,"sampling gap resets trend");
        speed=TrackPhysicsSample({700.f,1710,true,1},speed);
        Require(SpeedColor(speed,1710)==white,"physics clock rollback resets trend");
        Require(SpeedColor(speed,2300)==white,"stale samples never turn red");

        selfModule=GetModuleHandleW(nullptr);
        wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH,temp);
        directory=std::filesystem::path(temp)/(L"css-xy-speed-test-"+std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(directory);
        WNDCLASSW wc{};
        wc.lpfnWndProc=HostWindow; wc.hInstance=selfModule; wc.lpszClassName=L"XY_SPEED_HIDDEN_TEST";
        Require(RegisterClassW(&wc)!=0,"register hidden window");
        window=CreateWindowW(wc.lpszClassName,L"HUD callback test",WS_OVERLAPPEDWINDOW,0,0,640,480,
            nullptr,nullptr,selfModule,nullptr);
        Require(window!=nullptr,"create hidden window");
        Require(InstallWindowProc(window),"subclass hidden window");
        Require(InstallWindowProc(window),"repeated initialization must not chain handler to itself");
        SendMessageW(window,WM_MOUSEMOVE,0,0);
        SendMessageW(window,WM_KEYDOWN,'W',0);
        Require(forwardedMouse==1 && forwardedKeys==1,"normal input reaches host");
        // Character codes overlap virtual key codes: w/x/z are F8/F9/F11.
        // Text messages must reach the host even when those hotkeys are owned.
        for (const UINT message : {WM_CHAR, WM_SYSCHAR, WM_DEADCHAR, WM_SYSDEADCHAR, WM_UNICHAR}) {
            forwardedText.clear();
            for (wchar_t character=32;character<127;++character)
                SendMessageW(window,message,character,0);
            std::wstring expected;
            for (wchar_t character=32;character<127;++character) expected.push_back(character);
            Require(forwardedText==expected,"all printable ASCII including w/x/z reaches host unchanged");
        }
        for (const UINT message : {WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_SYSKEYUP})
            for (const WPARAM key : {VK_F8, VK_F9, VK_F11}) SendMessageW(window,message,key,0);
        Require(forwardedHotkeys==0,"real HUD hotkey down/up messages remain consumed");
        editing.store(true);
        SendMessageW(window,WM_MOUSEMOVE,0,0);
        SendMessageW(window,WM_KEYDOWN,'W',0);
        Require(forwardedMouse==1 && forwardedKeys==1,"editor blocks menu clicks and keys");
        SendMessageW(window,WM_KILLFOCUS,0,0);
        Require(!editing.load() && savePending.load(),"focus loss closes editor and requests save");
        SendMessageW(window,WM_KEYDOWN,'W',0);
        Require(forwardedKeys==2,"input resumes after focus loss");

        const auto ini=directory/"xy-speed.ini";
        WritePrivateProfileStringW(L"HUD",L"FontSize",L"48",ini.c_str());
        WritePrivateProfileStringW(L"HUD",L"Decimals",L"1",ini.c_str());
        settings.x=.75f; settings.y=.25f;
        SavePosition();
        Require(!saveFailed.load(),"save position");
        ReloadSettings();
        Require(std::abs(settings.x-.75f)<.00001f && std::abs(settings.y-.25f)<.00001f,"position survives reload");
        Require(settings.size==48 && settings.decimals==1,"saving position preserves other settings");

        auto fontContext=ImGui::CreateContext();
        auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.LogFilename=nullptr;
        auto font=LoadHudFont(io);
        Require(font!=nullptr && io.Fonts->Build(),"system font atlas builds");
        Require(font->FindGlyphNoFallback('8')!=nullptr && font->FindGlyphNoFallback('/')!=nullptr,"HUD glyphs present");
        Require(std::string(font->GetDebugName()).find(".ttf")!=std::string::npos,"outline font loaded rather than pixel fallback");
        // Verify the same drawing operation used by the HUD emits red glyph
        // vertices, without touching a game window or the desktop input.
        io.DisplaySize={800,600}; io.DeltaTime=1.f/60.f;
        auto checkGlyphFrame=[&](const Sample& observed,ULONGLONG now,ImU32 expected) {
            ImGui::NewFrame();
            auto glyphs=ImGui::GetBackgroundDrawList();
            glyphs->AddText(font,32.f,{30,30},SpeedColor(observed,now),"950.00 u/s");
            Require(!glyphs->VtxBuffer.empty(),"speed glyph vertices generated");
            for (const auto& vertex:glyphs->VtxBuffer) Require(vertex.col==expected,"physics tick color reaches every rendered glyph frame");
            ImGui::EndFrame();
        };
        const auto loss=TrackPhysicsSample({950.f,2010,true,201},{1000.f,2000,true,200});
        for (ULONGLONG now=2010;now<2020;now+=2) checkGlyphFrame(loss,now,red);
        const auto equal=TrackPhysicsSample({950.f,2020,true,202},loss);
        checkGlyphFrame(equal,2020,white);
        ImGui::DestroyContext(fontContext);

        Require(Cleanup(),"teardown succeeds without game hooks");
        Require(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC))==&HostWindow,"original window handler restored");
        SendMessageW(window,WM_MOUSEMOVE,0,0);
        Require(forwardedMouse==2,"host mouse input works after teardown");
        DestroyWindow(window); window=nullptr;
        UnregisterClassW(wc.lpszClassName,selfModule);
        // Delete only the three files produced by this test; no recursive delete.
        std::filesystem::remove(ini);
        std::filesystem::remove(directory/"xy-speed.log");
        std::filesystem::remove(directory);
        std::cout<<"PASS: ASCII text/hotkey separation, physics-tick loss/equal-speed/high-FPS rendering, input forwarding/capture/focus/teardown, INI persistence and system font\n";
        return 0;
    } catch (const std::exception& error) {
        if (window) { Cleanup(); DestroyWindow(window); }
        std::cerr<<error.what()<<'\n'; return 1;
    }
}
