#pragma once
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
using namespace Gdiplus;

void DrawSettingsContent(
    Graphics& g,
    const RectF& contentArea,
    Font& contentFont
);

bool OnSettingsClick(
    int x,
    int y,
    const RectF& contentArea
);

void ApplyDefaultSettings();

// Положение панели вкладок: 0=Верх, 1=Лево, 2=Право, 3=Низ
int GetSettingsTabPosition();

// Проверить, включён ли диск в слежку (передавать в формате "C:\\")
bool IsDriveMonitored(const std::wstring& drive);

// Получить список отслеживаемых дисков
std::vector<std::wstring> GetMonitoredDrives();