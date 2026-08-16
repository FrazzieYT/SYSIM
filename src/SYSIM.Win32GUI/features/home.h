#pragma once
#include <windows.h>
#include <gdiplus.h>

using namespace Gdiplus;

void InitHomeButtons();
void DrawHomeContent(Graphics& g, const RectF& contentArea, Font& contentFont);
bool OnHomeClick(int x, int y, const RectF& contentArea);

bool IsHomeRunEditing();
bool OnHomeRunKey(UINT msg, WPARAM wParam, LPARAM lParam);
void CancelHomeRunEdit();