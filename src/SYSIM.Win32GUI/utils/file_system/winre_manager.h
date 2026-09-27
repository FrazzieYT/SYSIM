#pragma once
#include <windows.h>
#include <string>

enum class WinRESource {
    System,
    File,
};

bool ObtainCleanWinRE(const std::wstring& outPath,
    const std::wstring& exeSourcePath = L"",
    const std::wstring& exeNameInImage = L"");
bool ReplaceWinRE(WinRESource source, const std::wstring& localWimPath = L"");
bool ReplaceWinREWithInjectedApp();
bool ShowReplaceWinREDialog(HWND owner);