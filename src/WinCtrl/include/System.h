#pragma once
#include <windows.h>
#include <string>

namespace WinCtrl::System {
inline std::wstring GetOfflineWindowsPath(const std::wstring& preferredDrive = L"") {
    wchar_t activeWindows[MAX_PATH] = {};
    std::wstring activeDrive;
    if (GetWindowsDirectoryW(activeWindows, ARRAYSIZE(activeWindows)) &&
        activeWindows[1] == L':' && activeWindows[2] == L'\\') {
        activeDrive.assign(activeWindows, 3);
    }

    auto isOfflineWindows = [&](std::wstring driveRoot) {
        if (driveRoot.size() == 2 && driveRoot[1] == L':') driveRoot += L'\\';
        if (driveRoot.size() < 3 ||
            (!activeDrive.empty() && _wcsicmp(driveRoot.c_str(), activeDrive.c_str()) == 0))
            return false;
        return GetFileAttributesW((driveRoot + L"Windows\\System32\\config\\SOFTWARE").c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW((driveRoot + L"Windows\\System32\\config\\SYSTEM").c_str()) != INVALID_FILE_ATTRIBUTES;
    };

    if (!preferredDrive.empty()) {
        std::wstring root = preferredDrive;
        if (root.size() == 2 && root[1] == L':') root += L'\\';
        return isOfflineWindows(root) ? root + L"Windows" : L"";
    }

    for (wchar_t drive = L'C'; drive <= L'Z'; ++drive) {
        std::wstring root{ drive, L':', L'\\' };
        if (isOfflineWindows(root)) return root + L"Windows";
    }
    return {};
}
}
