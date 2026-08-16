#pragma once
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <array>
#include "utils/file_system/file_explorer.h"

using namespace Gdiplus;

// ===== НАВИГАЦИЯ =====
extern std::vector<std::wstring> g_mainTabs;
extern int g_activeMainTab;
extern int g_previousMainTab;

extern int g_activeSubTab;
extern int g_activeTab;

extern bool g_draggingFromTab;
extern POINT g_dragStartPoint;
extern int g_dragTabIndex;

extern bool g_subTabsExpanded;
extern RectF g_subTabsToggleRect;

// ===== ГОРИЗОНТАЛЬНЫЕ ТАБЫ =====
extern RectF g_horizontalTabRects[9];
extern int g_horizontalTabHover;

// ===== СКРОЛЛ КОНТЕНТА =====
extern int g_scrollOffset[9];
extern int g_maxScroll[9];
extern int g_scrollBarWidth;
extern bool g_scrollBarDragging;
extern int g_scrollBarDragStartY;
extern int g_scrollBarDragStartOffset;

// ===== КНОПКИ УПРАВЛЕНИЯ ОКНОМ =====
extern RectF g_tabBtnSettings;
extern RectF g_tabBtnMinimize;
extern RectF g_tabBtnClose;
extern bool g_tabBtnSettingsHover;
extern bool g_tabBtnMinimizeHover;
extern bool g_tabBtnCloseHover;

// ===== ДОПОЛНИТЕЛЬНЫЕ КНОПКИ =====
extern RectF g_settingsButtonRect;
extern RectF g_notepadButtonRect;

extern std::array<Gdiplus::RectF, 10> g_sidebarTabRects;
extern std::array<Gdiplus::RectF, 10> g_subTabRects;

// ===== ТЕМА =====
extern std::wstring g_fontFamilyName;
extern const Color COLOR_BG;
extern const Color COLOR_HEADER_BG;
extern const Color COLOR_TAB_BG;
extern const Color COLOR_TAB_ACTIVE;
extern const Color COLOR_TAB_HOVER;
extern const Color COLOR_TEXT;
extern const Color COLOR_TEXT_MUTED;
extern const Color COLOR_BORDER;
extern const Color COLOR_BUTTON_BG;

// ===== ПРОВОДНИК =====
extern std::wstring g_explorerPath;
extern int g_explorerSelectedIndex;
extern std::vector<FileExplorer::FileItem> g_explorerItems;

// ===== LAYOUT =====
extern bool g_useVerticalLayout;