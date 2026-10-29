#include <Windows.h>
#include <TlHelp32.h>
#include <filesystem>
#include <iostream>
#include <string>

static std::uintptr_t RemoteBase(DWORD pid,const std::wstring& name){
    const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);if(snapshot==INVALID_HANDLE_VALUE)return 0;
    MODULEENTRY32W row{sizeof(row)};std::uintptr_t found=0;
    if(Module32FirstW(snapshot,&row))do{if(_wcsicmp(row.szModule,name.c_str())==0){found=reinterpret_cast<std::uintptr_t>(row.modBaseAddr);break;}}while(Module32NextW(snapshot,&row));CloseHandle(snapshot);return found;
}
int wmain(int argc,wchar_t** argv){
    if(argc!=3){std::wcerr<<L"Usage: loader.exe <cstrike_win64 PID> <absolute DLL path>\n";return 2;}
    wchar_t* end{};const auto raw=wcstoul(argv[1],&end,10);if(!raw||!end||*end){std::wcerr<<L"Invalid PID\n";return 2;}const DWORD pid=static_cast<DWORD>(raw);
    const auto dll=std::filesystem::absolute(argv[2]);if(!std::filesystem::is_regular_file(dll)||dll.extension()!=L".dll"){std::wcerr<<L"DLL file not found\n";return 2;}
    const auto process=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid);if(!process){std::wcerr<<L"OpenProcess failed: "<<GetLastError()<<L'\n';return 1;}
    wchar_t executable[32768]{};DWORD size=32768;USHORT machine{},native{};
    if(!QueryFullProcessImageNameW(process,0,executable,&size)||_wcsicmp(std::filesystem::path(executable).filename().c_str(),L"cstrike_win64.exe")!=0||!IsWow64Process2(process,&machine,&native)||machine!=IMAGE_FILE_MACHINE_UNKNOWN||native!=IMAGE_FILE_MACHINE_AMD64){CloseHandle(process);std::wcerr<<L"Target must be the 64-bit CS:S client\n";return 1;}
    if(RemoteBase(pid,dll.filename().wstring())){CloseHandle(process);std::wcerr<<L"DLL with this filename is already loaded; unload the existing instance first\n";return 1;}
    if(_wcsicmp(dll.filename().c_str(),L"CSSXYSpeed.dll")!=0){CloseHandle(process);std::wcerr<<L"This loader expects CSSXYSpeed.dll\n";return 1;}

    const auto text=dll.wstring();const auto bytes=(text.size()+1)*sizeof(wchar_t);void* remote=VirtualAllocEx(process,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!remote||!WriteProcessMemory(process,remote,text.c_str(),bytes,nullptr)){if(remote)VirtualFreeEx(process,remote,0,MEM_RELEASE);CloseHandle(process);std::wcerr<<L"Could not write DLL path\n";return 1;}
    const auto load=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");HMODULE owner{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(load),&owner);
    wchar_t ownerPath[32768]{};GetModuleFileNameW(owner,ownerPath,32768);const auto remoteOwner=RemoteBase(pid,std::filesystem::path(ownerPath).filename().wstring());
    if(!remoteOwner){VirtualFreeEx(process,remote,0,MEM_RELEASE);CloseHandle(process);std::wcerr<<L"Remote loader module unavailable\n";return 1;}
    const auto entry=remoteOwner+(reinterpret_cast<std::uintptr_t>(load)-reinterpret_cast<std::uintptr_t>(owner));
    const auto thread=CreateRemoteThread(process,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(entry),remote,0,nullptr);
    if(!thread){VirtualFreeEx(process,remote,0,MEM_RELEASE);CloseHandle(process);std::wcerr<<L"Load thread failed: "<<GetLastError()<<L'\n';return 1;}
    const auto wait=WaitForSingleObject(thread,10000);CloseHandle(thread);
    if(wait==WAIT_OBJECT_0)VirtualFreeEx(process,remote,0,MEM_RELEASE);CloseHandle(process);
    if(wait!=WAIT_OBJECT_0){std::wcerr<<L"Load still pending; path allocation retained until process exits. Do not retry blindly.\n";return 1;}
    if(!RemoteBase(pid,dll.filename().wstring())){std::wcerr<<L"DLL did not remain loaded; inspect the DLL log\n";return 1;}

    std::wcout<<L"Loaded "<<dll<<L" into CS:S PID "<<pid<<L". F8 positions HUD; Shift+F8 toggles HUD; F9 reloads INI; F11 unloads. Check xy-speed.log for READY.\n";

    return 0;
}
