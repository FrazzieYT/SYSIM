#pragma once
#include <windows.h>
#include <gdiplus.h>
using namespace Gdiplus;

void DrawExplorerContent(Graphics& g, const RectF& contentArea, Font& contentFont);
bool OnExplorerClick(int x, int y, const RectF& contentArea);
bool OnExplorerRightClick(int x, int y, const RectF& contentArea);

// Address bar inline editing
bool IsExplorerAddressBarEditing();
bool ExplorerAddressBarProcessKey(UINT msg, WPARAM wParam, LPARAM lParam);
void CancelExplorerAddressBarEdit();

// Manual column resizing
bool ExplorerLeftButtonDown(int x, int y, const RectF& contentArea);
bool ExplorerMouseMove(int x, int y, const RectF& contentArea);
bool ExplorerLeftButtonUp();

// Scrollbar
bool ExplorerMouseWheel(int delta, const RectF& contentArea);