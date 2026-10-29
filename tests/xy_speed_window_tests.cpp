// Exercise the actual native window handler in an invisible test window. No
// input is sent to the desktop/game, and the Computer Use helper is not used.
#include "../src/xy_speed.cpp"
#include <iostream>
#include <stdexcept>

namespace {
int forwardedMouse{}, forwardedKeys{};
void Require(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}
LRESULT CALLBACK HostWindow(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if (message==WM_MOUSEMOVE) ++forwardedMouse;
    if (message==WM_KEYDOWN && wparam=='W') ++forwardedKeys;
    return DefWindowProcW(window,message,wparam,lparam);
}
}
int wmain() {
    HWND window{};
    try {
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
        std::cout<<"PASS: input forwarding/capture/focus/teardown, INI persistence and system font\n";
        return 0;
    } catch (const std::exception& error) {
        if (window) { Cleanup(); DestroyWindow(window); }
        std::cerr<<error.what()<<'\n'; return 1;
    }
}
