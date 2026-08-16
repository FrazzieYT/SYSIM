#pragma once

#include <windows.h>
#include <gdiplus.h>

using namespace Gdiplus;

void DrawUnlockContent(Graphics& g, const RectF& contentArea, Font& contentFont);
bool OnUnlockClick(int x, int y, const RectF& contentArea);