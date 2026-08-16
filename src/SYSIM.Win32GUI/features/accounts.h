#pragma once
#include <windows.h>
#include <gdiplus.h>
using namespace Gdiplus;

void DrawAccountsContent(Graphics& g, const RectF& contentArea, Font& contentFont);
bool OnAccountsClick(int x, int y, const RectF& contentArea);
bool OnAccountsRightClick(int x, int y, const RectF& contentArea);