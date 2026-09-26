#pragma once
#include <windows.h>
#include <string>

#define WINRE_URL L"https://server/winre.wim"

bool ObtainCleanWinRE(bool useSystemSource, const std::wstring& outPath,
    const std::wstring& exeSourcePath = L"",
    const std::wstring& exeNameInImage = L"");
bool ReplaceWinRE(bool useSystemSource);
bool ReplaceWinREWithInjectedApp();
bool ShowReplaceWinREDialog(HWND owner);