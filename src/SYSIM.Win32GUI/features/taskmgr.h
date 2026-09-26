#pragma once
#include <windows.h>
#include <gdiplus.h>
using namespace Gdiplus;

#define WM_TASKMGR_SIG_READY (WM_APP + 101)
#define WM_TASKMGR_PROC_READY (WM_APP + 102)

// Индексы подвкладок
enum {
    SUBTAB_PROCESSES = 0,
    SUBTAB_STARTUP = 1,
    SUBTAB_SUSPICIOUS = 2,
    SUBTAB_SERVICES = 3,
    SUBTAB_DRIVERS = 4,
    SUBTAB_COUNT = 5
};

bool OnTaskManagerMessage(UINT msg, WPARAM wParam, LPARAM lParam);

void DrawTaskManagerContent(Graphics& g, const RectF& contentArea, Font& contentFont);
bool OnTaskManagerRightClick(int x, int y, const RectF& contentArea);
bool OnTaskManagerMouseMove(int x, int y, const RectF& contentArea);
bool OnTaskManagerDblClick(int x, int y, const RectF& contentArea);
bool OnTaskManagerClick(int x, int y, const RectF& contentArea);
bool OnTaskManagerKey(UINT msg, WPARAM wParam, LPARAM lParam);
bool OnTaskManagerLButtonUp();
bool OnTaskManagerWheel(int x, int y, int delta, const RectF& contentArea);
bool HasFrozenProcesses();
void ResumeFrozenProcesses();
bool TaskManagerDeleteRecentStartupEntries();
void TaskManagerShutdown();
void TaskManagerOnTimer();