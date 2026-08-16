#include "globals.h"

// ===== НАВИГАЦИЯ =====
std::vector<std::wstring> g_mainTabs = {
    L"Главная",          // 0
    L"Диспетчер задач",  // 1
    L"Проводник",        // 2
    L"Реестр",           // 3
    L"Разблокировка",    // 4
    L"Слежка",           // 5
    L"Учётные записи",   // 6
    L"Блокнот",          // 7
    L"Настройки"         // 8
};
int g_activeMainTab = 0;
int g_previousMainTab = 0;

int g_activeSubTab = 0;
int g_activeTab = 0;

bool g_draggingFromTab = false;
POINT g_dragStartPoint = {};
int g_dragTabIndex = -1;

bool g_subTabsExpanded = true;
RectF g_subTabsToggleRect;

// ===== ГОРИЗОНТАЛЬНЫЕ ТАБЫ =====
RectF g_horizontalTabRects[9] = {};
int g_horizontalTabHover = -1;

// ===== СКРОЛЛ КОНТЕНТА =====
int g_scrollOffset[9] = { 0 };
int g_maxScroll[9] = { 0 };
int g_scrollBarWidth = 16;
bool g_scrollBarDragging = false;
int g_scrollBarDragStartY = 0;
int g_scrollBarDragStartOffset = 0;

std::array<Gdiplus::RectF, 10> g_sidebarTabRects{};
std::array<Gdiplus::RectF, 10> g_subTabRects{};

// ===== КНОПКИ УПРАВЛЕНИЯ ОКНОМ =====
RectF g_tabBtnSettings;
RectF g_tabBtnMinimize;
RectF g_tabBtnClose;
bool g_tabBtnSettingsHover = false;
bool g_tabBtnMinimizeHover = false;
bool g_tabBtnCloseHover = false;

// ===== ДОПОЛНИТЕЛЬНЫЕ КНОПКИ =====
RectF g_settingsButtonRect;
RectF g_notepadButtonRect;

// ===== ТЕМА =====
std::wstring g_fontFamilyName = L"Segoe UI";
const Color COLOR_BG(255, 24, 24, 24);
const Color COLOR_HEADER_BG(255, 32, 32, 32);
const Color COLOR_TAB_BG(255, 34, 34, 34);
const Color COLOR_TAB_ACTIVE(255, 0, 120, 212);
const Color COLOR_TAB_HOVER(255, 45, 45, 48);
const Color COLOR_TEXT(255, 235, 235, 235);
const Color COLOR_TEXT_MUTED(255, 180, 180, 180);
const Color COLOR_BORDER(255, 70, 70, 70);
const Color COLOR_BUTTON_BG(255, 50, 50, 50);

// ===== ПРОВОДНИК =====
std::wstring g_explorerPath = L"C:\\";
int g_explorerSelectedIndex = -1;
std::vector<FileExplorer::FileItem> g_explorerItems;

// ===== LAYOUT =====
bool g_useVerticalLayout = true;