#pragma once
#include <windows.h>
#include <string>

namespace ServiceEditDialog {

    struct Config {
        std::wstring name;
        std::wstring displayName;
        std::wstring binaryPath;
        std::wstring description;
        std::wstring account;
        DWORD startType = SERVICE_DEMAND_START;
        DWORD serviceType = SERVICE_WIN32_OWN_PROCESS;
    };

    bool ShowEdit(HWND parent, const std::wstring& serviceName, bool isDriver = false);
    bool ShowCreate(HWND parent);
}