#include <iostream>
#include <Windows.h>
#include <TlHelp32.h>
#include <string>
#include <filesystem>
#include <fstream>

static DWORD findGame() {
    const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)return 0;
    PROCESSENTRY32W process{};process.dwSize=sizeof(process);DWORD result=0;
    if(Process32FirstW(snapshot,&process))do {
        if(_wcsicmp(process.szExeFile,L"csgo.exe")==0){result=process.th32ProcessID;break;}
    }while(Process32NextW(snapshot,&process));
    CloseHandle(snapshot);return result;
}
static std::string utf8(const std::wstring& text) {
    const int length=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(length,'\0');
    if(length)WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),length,nullptr,nullptr);
    return result;
}
int main(int argc,char** argv) {
    SetConsoleTitleW(L"FemboySense loader");SetConsoleOutputCP(CP_UTF8);
    wchar_t executable[32768]{};const auto count=GetModuleFileNameW(nullptr,executable,32768);
    if(!count || count>=32768){std::cerr<<"Cannot resolve loader directory.\n";return 1;}
    const auto directory=std::filesystem::path(executable).parent_path();
    const auto dll=directory/L"skeet.dll";
    std::ofstream log(directory/L"loader-startup.log",std::ios::app);
    auto report=[&](const std::string& message){std::cout<<message<<std::endl;if(log){log<<message<<std::endl;}};
    auto error=[&](const char* stage){const auto code=GetLastError();report(std::string("[ERROR] ")+stage+" (Win32 error "+std::to_string(code)+").");};
    report("[INFO] DLL: "+utf8(dll.wstring()));
    const auto attributes=GetFileAttributesW(dll.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_DIRECTORY)) {
        report("[ERROR] Put skeet.dll beside this loader. No game memory was changed.");return 1;
    }
    // Allows checking deployment/path resolution without opening a game process.
    if(argc==2 && std::string(argv[1])=="--check") {
        report("[INFO] Bundle path check passed; no process was opened.");return 0;
    }
    report("[INFO] Use -insecure in CS:GO launch options. Waiting for csgo.exe...");
    DWORD pid=0;while(!(pid=findGame()))Sleep(100);
    auto process=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid);
    if(!process){error("OpenProcess");return 1;}
    const auto image=VirtualAllocEx(process,reinterpret_cast<void*>(0x43310000),0x2FC000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    if(!image) {
        error("Fixed image allocation");report("[INFO] Exit CS:GO and start a fresh process before retrying. Do not rerun against a partially initialized session.");CloseHandle(process);return 1;
    }
    const auto path=dll.wstring();const auto bytes=(path.size()+1)*sizeof(wchar_t);
    auto argument=VirtualAllocEx(process,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!argument){error("DLL-path allocation");VirtualFreeEx(process,image,0,MEM_RELEASE);CloseHandle(process);return 1;}
    SIZE_T written=0;
    if(!WriteProcessMemory(process,argument,path.c_str(),bytes,&written) || written!=bytes) {
        error("Writing DLL path");VirtualFreeEx(process,argument,0,MEM_RELEASE);VirtualFreeEx(process,image,0,MEM_RELEASE);CloseHandle(process);return 1;
    }
    auto load=reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW"));
    auto thread=load ? CreateRemoteThread(process,nullptr,0,load,argument,0,nullptr) : nullptr;
    if(!thread) {
        error("LoadLibrary thread creation");VirtualFreeEx(process,argument,0,MEM_RELEASE);VirtualFreeEx(process,image,0,MEM_RELEASE);CloseHandle(process);return 1;
    }
    const auto waited=WaitForSingleObject(thread,30000);
    if(waited!=WAIT_OBJECT_0) {
        report("[ERROR] DLL loading did not finish within 30 seconds. See loader-startup.log; restart CS:GO before retrying.");
        // The thread may still read its argument; retain remote allocations.
        CloseHandle(thread);CloseHandle(process);return 1;
    }
    DWORD module=0;const bool queried=GetExitCodeThread(thread,&module)!=0;
    if(!queried)error("Reading LoadLibrary result");
    if(queried && !module)report("[ERROR] LoadLibraryW returned NULL. Check DLL dependencies and this bundle's path; no initialization worker ran.");
    VirtualFreeEx(process,argument,0,MEM_RELEASE);
    if(queried && !module)VirtualFreeEx(process,image,0,MEM_RELEASE);
    CloseHandle(thread);CloseHandle(process);
    if(!queried || !module)return 1;
    report("[INFO] DLL loaded. Initialization is asynchronous; see skeet-startup.log beside this loader for the exact stage.");
    return 0;
}
