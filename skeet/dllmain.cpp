#include "pch.h"
#include "skCrypter.h"
#include "startup_log.h"

static DWORD WINAPI thread(void* argument) {
    auto base=static_cast<HMODULE>(argument);
    startup_log::begin(base);
    startup_log::write("Startup worker entered (renderer.blur fix).");
#ifdef DLOG
    system("cls");
#endif
    skeet_t* skeet = skeet_t::getInstance(base);
    if (!skeet->map()) {
        startup_log::write("FAILED: embedded image mapping.");
        std::cout << skCrypt("[INFO] failed to allocate memory, reboot PC\n");
        return 1;
    }
    startup_log::write("Image mapped; waiting for serverbrowser.dll.");
    while (GetModuleHandleA("serverbrowser.dll") == 0) Sleep(50);
    startup_log::write("Game modules ready; resolving imports.");
    if(!skeet->fix_imports()) {
        startup_log::write("FAILED: import resolution (Win32 error %lu).",GetLastError());return 2;
    }
    std::cout << skCrypt("[INFO] fixed!\n");
    if (!skeet->extra()) {
        startup_log::write("FAILED: core image initialization; embedded entry cancelled.");
        OutputDebugStringA("[INFO] Image patch validation failed; embedded entry cancelled.\n");
        std::cout << skCrypt("[INFO] image patch validation failed\n");
        return 3;
    }
    startup_log::write("Core initialization complete; starting embedded entry.");
    if(!skeet->entry()){startup_log::write("FAILED: entry thread setup (Win32 error %lu).",GetLastError());return 4;}
    startup_log::write("Embedded entry thread resumed.");
    return 0;
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
#ifdef DLOG
        AllocConsole();
        SetConsoleTitleA("SkeetCrack");
        freopen("CONOUT$", "w", stdout);
#endif
        DisableThreadLibraryCalls(hModule);
        if(auto worker=CreateThread(0,0,thread,hModule,0,nullptr))CloseHandle(worker);
        break;
    }
    return TRUE;
}

