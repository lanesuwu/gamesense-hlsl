#pragma once
#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <cwchar>

namespace startup_log {
    inline wchar_t path[MAX_PATH]{};
    inline void begin(HMODULE module) {
        const auto count=GetModuleFileNameW(module,path,MAX_PATH);
        if(!count || count>=MAX_PATH){path[0]=0;return;}
        auto slash=wcsrchr(path,L'\\');
        if(!slash || size_t(slash-path)+20>=MAX_PATH){path[0]=0;return;}
        wcscpy_s(slash+1,MAX_PATH-size_t(slash+1-path),L"skeet-startup.log");
    }
    inline void write(const char* format,...) {
        const auto saved=GetLastError();
        char message[1024]{};va_list args;va_start(args,format);
        vsnprintf_s(message,sizeof(message),_TRUNCATE,format,args);va_end(args);
        OutputDebugStringA(message);OutputDebugStringA("\n");
        if(path[0]) {
            auto file=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file!=INVALID_HANDLE_VALUE) {
                SYSTEMTIME time{};GetLocalTime(&time);char line[1200]{};
                const int count=sprintf_s(line,"%04u-%02u-%02u %02u:%02u:%02u pid=%lu %s\r\n",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,GetCurrentProcessId(),message);
                DWORD written=0;if(count>0)WriteFile(file,line,count,&written,nullptr);CloseHandle(file);
            }
        }
        SetLastError(saved);
    }
}
