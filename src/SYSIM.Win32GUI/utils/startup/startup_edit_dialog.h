#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace StartupEditDialog {

    struct Location {
        HKEY root = HKEY_CURRENT_USER;
        std::wstring subKey;
        std::wstring label;
        REGSAM view = KEY_WOW64_64KEY;
    };

    bool ShowEdit(HWND parent, const Location& loc, const std::wstring& valueName);
    
    bool ShowCreate(HWND parent);
    bool ShowCreate(HWND parent, const std::vector<Location>& locations);
}