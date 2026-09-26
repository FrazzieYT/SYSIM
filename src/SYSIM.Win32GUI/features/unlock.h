#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <string>

using namespace Gdiplus;

extern bool g_unlockInProgress;
extern std::wstring g_lastReport;
inline constexpr UINT WM_UNLOCK_COMPLETE = WM_APP + 610;

void DrawUnlockContent(Graphics& g, const RectF& contentArea, Font& contentFont);
bool OnUnlockClick(int x, int y, const RectF& contentArea);
void RunFullRecovery(bool diagnosticOnly = false);
void RunWinPeFullDiagnosis();
void RunWinPeFullDiagnosis();
void RunWinPeScan();
void RunWinPeUnlockPolicies();
void RunWinPeFullRepair();
void RunWinPeLogonFilesRepair();