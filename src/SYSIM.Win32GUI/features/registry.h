#pragma once

#include <windows.h>
#include <gdiplus.h>

using namespace Gdiplus;

void DrawRegistryContent(
    Graphics& g,
    const RectF& contentArea,
    Font& contentFont
);

bool OnRegistryClick(
    int x,
    int y,
    const RectF& contentArea
);

bool OnRegistryRightClick(
    int x,
    int y,
    const RectF& contentArea
);

bool OnRegistryKey(
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
);

// User registry panel & column resizing
bool RegistryLeftButtonDown(
    int x,
    int y,
    const RectF& contentArea
);

bool RegistryMouseMove(
    int x,
    int y,
    const RectF& contentArea
);

bool RegistryLeftButtonUp();