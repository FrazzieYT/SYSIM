#include "wndproc.h"
#include "globals.h"
#include "app.h"
#include "features/home.h"
#include "features/taskmgr.h"
#include "features/explorer.h"
#include "features/registry.h"
#include "features/settings.h"
#include "features/notepad.h"
#include "features/unlock.h"
#include "features/monitor.h"
#include "features/accounts.h"
#include "utils/unlock/unlock_tools.h"
#include "utils/history/system_monitor.h"
#include "ui/widgets.h"
#include <gdiplus.h>
#include <windowsx.h>
#include <algorithm>
#include "utils/registry/registry_editor.h"

#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

enum { IDM_TRAY_OPEN = 9001, IDM_TRAY_EXIT = 9002 };

using namespace Gdiplus;

const Gdiplus::Color COLOR_CLOSE_HOVER = Gdiplus::Color(180, 40, 40);

static bool IsTaskManagerHeaderTab(int mainTab) {
    return mainTab == 1 || mainTab == 9;
}

static void ApplyTaskManagerHeaderTab(int mainTab) {
    switch (mainTab) {
    case 1: g_activeSubTab = SUBTAB_PROCESSES; break;
    case 9: g_activeSubTab = SUBTAB_STARTUP; break;
    default: break;
    }
}

// Высота шапки
static const float HEADER_SIZE = 40.0f;
static const float TAB_THICKNESS = 26.0f;
static const float TAB_SPACING = 4.0f;

// Вертикальный сайдбар
static const float SIDEBAR_W = 200.0f;
static const float SIDEBAR_ITEM_H = 34.0f;
static const float SIDEBAR_ITEM_SPACING = 2.0f;
static const float SIDEBAR_PAD = 10.0f;

// Позиции: 0=Верх, 1=Лево, 2=Право, 3=Низ
static bool IsVerticalTabs(int pos) {
    return pos == 1 || pos == 2;
}

// Полоса шапки (табы, кнопки)
static RectF GetHeaderRect(const RectF& client, int pos) {
    switch (pos) {
    case 0: return RectF(0, 0, client.Width, HEADER_SIZE);
    case 3: return RectF(0, client.Height - HEADER_SIZE, client.Width, HEADER_SIZE);
    case 1: return RectF(0, 0, SIDEBAR_W, client.Height);
    case 2: return RectF(client.Width - SIDEBAR_W, 0, SIDEBAR_W, client.Height);
    }
    return RectF(0, 0, client.Width, HEADER_SIZE);
}

// Область контента
static RectF GetContentRect(const RectF& client, int pos) {
    switch (pos) {
    case 0: return RectF(0, HEADER_SIZE + 1, client.Width, client.Height - HEADER_SIZE - 1);
    case 3: return RectF(0, 0, client.Width, client.Height - HEADER_SIZE - 1);
    case 1: return RectF(SIDEBAR_W + 1, 0, client.Width - SIDEBAR_W - 1, client.Height);
    case 2: return RectF(0, 0, client.Width - SIDEBAR_W - 1, client.Height);
    }
    return RectF(0, HEADER_SIZE + 1, client.Width, client.Height - HEADER_SIZE - 1);
}

// Область для кнопок окна
static RectF GetWindowButtonsArea(const RectF& client, int pos, float btnSize, float btnCount) {
    RectF header = GetHeaderRect(client, pos);
    float total = btnSize * btnCount;
    switch (pos) {
    case 0: return RectF(header.X + header.Width - total, header.Y, total, header.Height);
    case 3: return RectF(header.X + header.Width - total, header.Y, total, header.Height);
    case 1: return RectF(header.X, header.Y + header.Height - total, header.Width, total);
    case 2: return RectF(header.X, header.Y + header.Height - total, header.Width, total);
    }
    return RectF();
}

static std::vector<bool> g_selectedSystemKeybinds;
static std::vector<RectF> g_systemKeybindRowRects;
static RectF g_systemKeybindViewport;
static RectF g_unlockAllSystemKeybindsRect;
static RectF g_unlockSelectedSystemKeybindsRect;
static std::vector<UnlockTools::Restriction> g_keybindRestrictions;
static std::vector<bool> g_keybindRestrictionActive;
static ULONGLONG g_keybindRestrictionScanTime = 0;

static void RefreshKeybindRestrictionStates(bool force = false) {
    ULONGLONG now = GetTickCount64();
    if (!force && now - g_keybindRestrictionScanTime < 2000) return;

    g_keybindRestrictions = UnlockTools::GetKnownRestrictions();
    g_keybindRestrictionActive.resize(g_keybindRestrictions.size());
    for (size_t i = 0; i < g_keybindRestrictions.size(); ++i) {
        g_keybindRestrictionActive[i] =
            UnlockTools::IsRestricted(g_keybindRestrictions[i]);
    }
    g_keybindRestrictionScanTime = now;
}

static bool IsRestrictionForSystemKeybind(
    const UnlockTools::Restriction& restriction,
    const SystemKeybindEntry& entry
) {
    if (restriction.description == L"Горячие клавиши Win")
        return entry.keys.find(L"Win + ") == 0 && entry.keys != L"Win + L";
    if (restriction.description == L"Окно 'Выполнить' (Win+R)")
        return entry.keys == L"Win + R";
    if (restriction.description == L"Поиск в Пуске")
        return entry.keys == L"Win + S";
    if (restriction.description == L"Блокировка рабочей станции")
        return entry.keys == L"Win + L";
    if (restriction.description == L"Диспетчер задач")
        return entry.keys == L"Ctrl + Shift + Esc";
    return false;
}

static bool IsSystemKeybindRestricted(size_t entryIndex, bool& isSupported) {
    isSupported = false;
    if (entryIndex >= g_systemKeybindCatalog.size()) return false;

    bool restricted = false;
    const auto& entry = g_systemKeybindCatalog[entryIndex];
    for (size_t i = 0; i < g_keybindRestrictions.size(); ++i) {
        if (!IsRestrictionForSystemKeybind(g_keybindRestrictions[i], entry)) continue;
        isSupported = true;
        restricted = restricted || g_keybindRestrictionActive[i];
    }
    return restricted;
}

static bool UnlockSystemKeybindRestrictions(HWND hwnd, bool selectedOnly) {
    RefreshKeybindRestrictionStates(true);
    bool foundRestriction = false;
    bool allSucceeded = true;

    for (size_t restrictionIndex = 0;
        restrictionIndex < g_keybindRestrictions.size(); ++restrictionIndex) {
        if (!g_keybindRestrictionActive[restrictionIndex]) continue;

        bool appliesToSelection = false;
        for (size_t entryIndex = 0; entryIndex < g_systemKeybindCatalog.size(); ++entryIndex) {
            if (selectedOnly && (entryIndex >= g_selectedSystemKeybinds.size() ||
                !g_selectedSystemKeybinds[entryIndex])) continue;
            if (IsRestrictionForSystemKeybind(
                g_keybindRestrictions[restrictionIndex], g_systemKeybindCatalog[entryIndex])) {
                appliesToSelection = true;
                break;
            }
        }
        if (!appliesToSelection) continue;

        foundRestriction = true;
        if (!UnlockTools::UnlockRestriction(g_keybindRestrictions[restrictionIndex]))
            allSucceeded = false;
    }

    RefreshKeybindRestrictionStates(true);
    InvalidateRect(hwnd, nullptr, TRUE);
    const wchar_t* message = !foundRestriction
        ? L"Для выбранных сочетаний нет обнаруженных поддерживаемых блокировок."
        : (allSucceeded
            ? L"Обнаруженные ограничения сняты."
            : L"Не удалось снять часть ограничений. Проверьте права доступа.");
    MessageBoxW(hwnd, message, L"Системные кейбинды", MB_OK |
        (allSucceeded ? MB_ICONINFORMATION : MB_ICONWARNING));
    return true;
}

static void DrawKeybindsContent(Graphics& g, const RectF& area, const Font& font) {
    SolidBrush bgBrush(COLOR_BG);
    g.FillRectangle(&bgBrush, area);

    Font titleFont(L"Segoe UI", 20.0f, FontStyleBold, UnitPixel);
    Font groupFont(L"Segoe UI", 15.0f, FontStyleBold, UnitPixel);
    Font rowFont(L"Segoe UI", 12.5f, FontStyleRegular, UnitPixel);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentNear);
    fmt.SetLineAlignment(StringAlignmentCenter);

    SolidBrush textBrush(COLOR_TEXT);
    SolidBrush mutedBrush(COLOR_TEXT_MUTED);
    SolidBrush panelBrush(COLOR_TAB_BG);
    Pen borderPen(COLOR_BORDER, 1.0f);
    RefreshKeybindRestrictionStates();
    g_selectedSystemKeybinds.resize(g_systemKeybindCatalog.size(), false);

    RectF headerRect(area.X + 18.0f, area.Y + 16.0f, 260.0f, 30.0f);
    g.DrawString(L"Кейбинды", -1, &titleFont, headerRect, &fmt, &textBrush);

    float controlsY = area.Y + 54.0f;
    RectF statusTitle(area.X + 18.0f, controlsY, 260.0f, 28.0f);
    g.DrawString(L"Системные ограничения", -1, &groupFont,
        statusTitle, &fmt, &textBrush);
    float buttonsX = area.X + area.Width - 370.0f;
    g_unlockAllSystemKeybindsRect = RectF(buttonsX, controlsY, 170.0f, 28.0f);
    g_unlockSelectedSystemKeybindsRect = RectF(buttonsX + 178.0f, controlsY, 192.0f, 28.0f);
    StringFormat buttonFormat;
    buttonFormat.SetAlignment(StringAlignmentCenter);
    buttonFormat.SetLineAlignment(StringAlignmentCenter);
    g.FillRectangle(&panelBrush, g_unlockAllSystemKeybindsRect);
    g.DrawRectangle(&borderPen, g_unlockAllSystemKeybindsRect);
    g.FillRectangle(&panelBrush, g_unlockSelectedSystemKeybindsRect);
    g.DrawRectangle(&borderPen, g_unlockSelectedSystemKeybindsRect);
    g.DrawString(L"Снять все найденные", -1, &rowFont,
        g_unlockAllSystemKeybindsRect, &buttonFormat, &textBrush);
    g.DrawString(L"Снять у выбранных", -1, &rowFont,
        g_unlockSelectedSystemKeybindsRect, &buttonFormat, &textBrush);

    float contentHeight = 0.0f;
    std::wstring measuredGroup;
    for (const auto& entry : g_systemKeybindCatalog) {
        if (measuredGroup != entry.group) {
            measuredGroup = entry.group;
            contentHeight += 30.0f;
        }
        contentHeight += 34.0f;
    }
    float listTop = area.Y + 92.0f;
    float viewportHeight = (std::max)(0.0f, area.Height - (listTop - area.Y) - 8.0f);
    g_maxScroll[10] = (std::max)(0, (int)(contentHeight - viewportHeight));
    g_scrollOffset[10] = (std::max)(0, (std::min)(g_scrollOffset[10], g_maxScroll[10]));

    GraphicsState clipState = g.Save();
    g.SetClip(RectF(area.X, listTop, area.Width, viewportHeight), CombineModeIntersect);
    g_systemKeybindViewport = RectF(area.X, listTop, area.Width, viewportHeight);
    g_systemKeybindRowRects.resize(g_systemKeybindCatalog.size());
    float y = listTop - (float)g_scrollOffset[10];
    std::wstring currentGroup;
    for (size_t entryIndex = 0; entryIndex < g_systemKeybindCatalog.size(); ++entryIndex) {
        const auto& entry = g_systemKeybindCatalog[entryIndex];
        if (currentGroup != entry.group) {
            currentGroup = entry.group;
            RectF groupRect(area.X + 18.0f, y, 260.0f, 24.0f);
            g.DrawString(currentGroup.c_str(), -1, &groupFont, groupRect, &fmt, &textBrush);
            y += 30.0f;
        }

        RectF rowRect(area.X + 18.0f, y, area.Width - 36.0f, 30.0f);
        g_systemKeybindRowRects[entryIndex] = rowRect;
        g.FillRectangle(&panelBrush, rowRect);
        g.DrawRectangle(&borderPen, rowRect);

        RectF checkRect(rowRect.X + 8.0f, rowRect.Y + 8.0f, 14.0f, 14.0f);
        g.DrawRectangle(&borderPen, checkRect);
        if (g_selectedSystemKeybinds[entryIndex]) {
            SolidBrush selectedBrush(COLOR_TAB_ACTIVE);
            g.FillRectangle(&selectedBrush, RectF(
                checkRect.X + 3.0f, checkRect.Y + 3.0f, 8.0f, 8.0f));
        }

        RectF keysRect(rowRect.X + 32.0f, rowRect.Y,
            rowRect.Width * 0.28f, rowRect.Height);
        RectF actionRect(rowRect.X + rowRect.Width * 0.31f + 18.0f, rowRect.Y,
            rowRect.Width * 0.48f, rowRect.Height);
        RectF statusRect(rowRect.X + rowRect.Width - 140.0f, rowRect.Y,
            132.0f, rowRect.Height);
        bool isSupported = false;
        bool isRestricted = IsSystemKeybindRestricted(entryIndex, isSupported);
        SolidBrush statusBrush(isRestricted
            ? Color(255, 240, 115, 105) : Color(255, 100, 210, 130));

        g.DrawString(entry.keys.c_str(), -1, &rowFont, keysRect, &fmt, &textBrush);
        g.DrawString(entry.action.c_str(), -1, &rowFont, actionRect, &fmt, &mutedBrush);
        g.DrawString(isSupported ? (isRestricted ? L"Заблокировано" : L"Политика не блокирует")
            : L"Не проверяется", -1, &rowFont, statusRect, &fmt,
            isSupported ? &statusBrush : &mutedBrush);

        y += 34.0f;
    }
    g.Restore(clipState);
}

static bool OnKeybindsClick(HWND hwnd, int x, int y) {
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    auto hit = [fx, fy](const RectF& rect) {
        return fx >= rect.X && fx < rect.X + rect.Width &&
            fy >= rect.Y && fy < rect.Y + rect.Height;
    };

    if (hit(g_unlockAllSystemKeybindsRect))
        return UnlockSystemKeybindRestrictions(hwnd, false);
    if (hit(g_unlockSelectedSystemKeybindsRect))
        return UnlockSystemKeybindRestrictions(hwnd, true);
    if (!hit(g_systemKeybindViewport)) return false;

    for (size_t i = 0; i < g_systemKeybindRowRects.size(); ++i) {
        if (!hit(g_systemKeybindRowRects[i])) continue;
        g_selectedSystemKeybinds[i] = !g_selectedSystemKeybinds[i];
        InvalidateRect(hwnd, nullptr, TRUE);
        return true;
    }
    return false;
}

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_UNLOCK_COMPLETE: {
        auto* report = reinterpret_cast<std::wstring*>(lParam);
        if (report) {
            g_lastReport = *report;
            delete report;
        }
        g_unlockInProgress = false;
        InvalidateRect(hWnd, nullptr, TRUE);
        if (wParam == 2) {
            MessageBoxW(hWnd, g_lastReport.c_str(), L"WINPE-RE · результат",
                MB_OK | MB_ICONINFORMATION);
        }
        else if (wParam == 0) {
            MessageBoxW(hWnd,
                L"Разблокировка выполнена.\r\nПодробности в отчёте.",
                L"Разблокировка", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }
    case WM_TASKMGR_SIG_READY:
        OnTaskManagerMessage(msg, wParam, lParam);
        return 0;
    case WM_TASKMGR_PROC_READY:
        OnTaskManagerMessage(msg, wParam, lParam);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdcScreen = BeginPaint(hWnd, &ps);

        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        int w = rcClient.right - rcClient.left;
        int h = rcClient.bottom - rcClient.top;
        if (w <= 0 || h <= 0) { EndPaint(hWnd, &ps); break; }

        App::Instance()->EnsureBackBuffer(w, h);
        HDC hdcMem = App::Instance()->GetBackBufferDC();
        if (!hdcMem) { EndPaint(hWnd, &ps); break; }

        {
            Graphics g(hdcMem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

            RectF clientRect(0.0f, 0.0f, (REAL)w, (REAL)h);

        SolidBrush bgBrush(COLOR_BG);
        g.FillRectangle(&bgBrush, clientRect);

        FontFamily ff(g_fontFamilyName.c_str());
        Font tabFont(&ff, 12.0f, FontStyleRegular, UnitPixel);
        Font contentFont(&ff, 12.0f, FontStyleRegular, UnitPixel);
        Font iconFont(&ff, 14.0f, FontStyleRegular, UnitPixel);

        // === Шапка и табы ===
        int tabPos = GetSettingsTabPosition();
        bool vertical = IsVerticalTabs(tabPos);

        RectF headerRect = GetHeaderRect(clientRect, tabPos);
        SolidBrush headerBrush(COLOR_HEADER_BG);
        g.FillRectangle(&headerBrush, headerRect);

        // === Управление окном ===
        float btnSize = 42.0f;
        const float BTN_COUNT = 2.0f;
        RectF btnArea = GetWindowButtonsArea(clientRect, tabPos, btnSize, BTN_COUNT);

        if (!vertical) {
            float btnY = headerRect.Y + (headerRect.Height - btnSize) / 2.0f;
            g_tabBtnClose = RectF(btnArea.X + btnSize, btnY, btnSize, btnSize);
            g_tabBtnMinimize = RectF(btnArea.X, btnY, btnSize, btnSize);
        }
        else {
            // Верхний правый угол сайдбара
            float btnX = headerRect.X + headerRect.Width - btnSize * 2.0f - 6.0f;
            float btnY = headerRect.Y + 4.0f;
            g_tabBtnMinimize = RectF(btnX, btnY, btnSize, btnSize);
            g_tabBtnClose = RectF(btnX + btnSize, btnY, btnSize, btnSize);
        }

        auto DrawWindowButton = [&](const RectF& r, bool hover, const wchar_t* symbol, bool isClose = false) {
            Color bg = COLOR_HEADER_BG;
            if (hover) bg = isClose ? COLOR_CLOSE_HOVER : COLOR_TAB_HOVER;
            SolidBrush b(bg);
            g.FillRectangle(&b, r);
            StringFormat fmt;
            fmt.SetAlignment(StringAlignmentCenter);
            fmt.SetLineAlignment(StringAlignmentCenter);
            SolidBrush sb(COLOR_TEXT);
            g.DrawString(symbol, -1, &iconFont, r, &fmt, &sb);
            };
        DrawWindowButton(g_tabBtnMinimize, g_tabBtnMinimizeHover, L"─");
        DrawWindowButton(g_tabBtnClose, g_tabBtnCloseHover, L"✕", true);

        // ===== Табы =====
        RectF tabsArea;
        if (!vertical) {
            tabsArea = RectF(headerRect.X + 8.0f, headerRect.Y,
                btnArea.X - headerRect.X - 12.0f, headerRect.Height);
        }
        else {
            tabsArea = RectF(headerRect.X, headerRect.Y + 8.0f,
                headerRect.Width, btnArea.Y - headerRect.Y - 12.0f);
        }

        int tabCount = (int)g_mainTabs.size();

        if (!vertical) {
            // Горизонтальные табы
            float tabHeight = TAB_THICKNESS;
            float tabY = headerRect.Y + (headerRect.Height - tabHeight) / 2.0f;

            std::vector<float> widths;
            float totalW = 0.0f;
            for (int i = 0; i < tabCount; ++i) {
                RectF b;
                g.MeasureString(g_mainTabs[i].c_str(), -1, &tabFont, PointF(0, 0), &b);
                float w = b.Width + 14.0f;
                if (w < 42.0f) w = 42.0f;
                widths.push_back(w);
                totalW += w;
            }
            totalW += (tabCount - 1) * TAB_SPACING;
            float avail = tabsArea.Width;
            if (totalW > avail) {
                float scale = (avail - (tabCount - 1) * TAB_SPACING) / (totalW - (tabCount - 1) * TAB_SPACING);
                if (scale < 0.3f) scale = 0.3f;
                totalW = 0.0f;
                for (auto& w : widths) {
                    w *= scale;
                    if (w < 36.0f) w = 36.0f;
                    totalW += w;
                }
                totalW += (tabCount - 1) * TAB_SPACING;
            }
            float cx = tabsArea.X;
            for (int i = 0; i < tabCount; ++i) {
                g_horizontalTabRects[i] = RectF(cx, tabY, widths[i], tabHeight);

                Color tc = COLOR_HEADER_BG;
                if (i == g_activeMainTab) tc = COLOR_TAB_ACTIVE;
                else if (i == g_horizontalTabHover) tc = COLOR_TAB_HOVER;
                SolidBrush tb(tc);
                g.FillRectangle(&tb, g_horizontalTabRects[i]);

                StringFormat fmt;
                fmt.SetAlignment(StringAlignmentCenter);
                fmt.SetLineAlignment(StringAlignmentCenter);
                SolidBrush tbr(COLOR_TEXT);
                g.DrawString(g_mainTabs[i].c_str(), -1, &tabFont, g_horizontalTabRects[i], &fmt, &tbr);

                cx += widths[i] + TAB_SPACING;
            }
        }
        else {
            // === Вертикальный сайдбар ===
            const Color ITEM_HOVER(255, 52, 52, 56);
            const Color ITEM_ACTIVE(255, 0, 120, 212);

            Font sideFont(&ff, 12.5f, FontStyleRegular, UnitPixel);
            Font sideSmall(&ff, 10.5f, FontStyleRegular, UnitPixel);

            StringFormat lf;
            lf.SetAlignment(StringAlignmentNear);
            lf.SetLineAlignment(StringAlignmentCenter);
            lf.SetTrimming(StringTrimmingEllipsisCharacter);

            float itemW = headerRect.Width - SIDEBAR_PAD * 2.0f;
            float cx = headerRect.X + SIDEBAR_PAD;
            float cy = headerRect.Y + 48.0f;

            auto drawSideItem = [&](int tabIdx) {
                RectF r(cx, cy, itemW, SIDEBAR_ITEM_H);
                g_horizontalTabRects[tabIdx] = r;

                bool active = (tabIdx == g_activeMainTab);
                bool hover = (tabIdx == g_horizontalTabHover);

                if (active) {
                    SolidBrush b(ITEM_ACTIVE);
                    g.FillRectangle(&b, r);
                    SolidBrush ind(Color(255, 255, 255, 255));
                    g.FillRectangle(&ind, RectF(r.X, r.Y + 4.0f, 3.0f, r.Height - 8.0f));
                }
                else if (hover) {
                    SolidBrush b(ITEM_HOVER);
                    g.FillRectangle(&b, r);
                }

                RectF textR(r.X + 14.0f, r.Y, r.Width - 18.0f, r.Height);
                SolidBrush txtBrush(active ? Color(255, 255, 255, 255) : COLOR_TEXT);
                g.DrawString(g_mainTabs[tabIdx].c_str(), -1, &sideFont, textR, &lf, &txtBrush);

                cy += SIDEBAR_ITEM_H + SIDEBAR_ITEM_SPACING;
                };

            for (int i = 0; i <= 5 && i < tabCount; ++i) drawSideItem(i);

            cy += 8.0f;
            {
                Pen sep(COLOR_BORDER, 1.0f);
                g.DrawLine(&sep, cx + 6.0f, cy, cx + itemW - 6.0f, cy);
            }
            cy += 10.0f;

            if (tabCount > 7) drawSideItem(7);   // Блокнот
            if (tabCount > 8) drawSideItem(8);   // Настройки
            if (tabCount > 9) drawSideItem(9);   // Подозрительные

            float userH = 76.0f;
            float userY = headerRect.Y + headerRect.Height - userH - 12.0f;
            RectF ub(cx, userY, itemW, userH);

            bool userActive = (g_activeMainTab == 6);
            bool userHover = (g_horizontalTabHover == 6);

            Color ubBgColor = userActive ? ITEM_ACTIVE
                : (userHover ? ITEM_HOVER : Color(255, 30, 30, 32));
            SolidBrush ubBg(ubBgColor);
            g.FillRectangle(&ubBg, ub);

            if (userActive) {
                SolidBrush ind(Color(255, 255, 255, 255));
                g.FillRectangle(&ind, RectF(ub.X, ub.Y + 4.0f, 3.0f, ub.Height - 8.0f));
            }

            Pen ubBorder(COLOR_BORDER, 1.0f);
            g.DrawRectangle(&ubBorder, ub);
            
            g_horizontalTabRects[6] = ub;

            wchar_t userName[256] = L"?";
            DWORD unSize = 256;
            GetUserNameW(userName, &unSize);
            bool isAdmin = IsUserAnAdmin() != FALSE;

            SolidBrush uNameBrush(COLOR_TEXT);
            SolidBrush uRoleBrush(isAdmin ? Color(255, 255, 193, 7) : COLOR_TEXT_MUTED);
            SolidBrush uRightsBrush(COLOR_TEXT_MUTED);

            RectF uNameR(ub.X + 12.0f, ub.Y + 8.0f, ub.Width - 24.0f, 22.0f);
            g.DrawString(userName, -1, &sideFont, uNameR, &lf, &uNameBrush);

            RectF uRoleR(ub.X + 12.0f, ub.Y + 32.0f, ub.Width - 24.0f, 18.0f);
            g.DrawString(isAdmin ? L"Администратор" : L"Пользователь",
                -1, &sideSmall, uRoleR, &lf, &uRoleBrush);

            RectF uRightsR(ub.X + 12.0f, ub.Y + 52.0f, ub.Width - 24.0f, 18.0f);
            g.DrawString(isAdmin ? L"Полные права" : L"Ограниченные права",
                -1, &sideSmall, uRightsR, &lf, &uRightsBrush);
        }

        // === Разделитель шапки и контента ===
        Pen separatorPen(COLOR_BORDER, 1.0f);
        if (tabPos == 0) {
            g.DrawLine(&separatorPen, 0.0f, HEADER_SIZE, clientRect.Width, HEADER_SIZE);
        }
        else if (tabPos == 3) {
            g.DrawLine(&separatorPen, 0.0f, clientRect.Height - HEADER_SIZE, clientRect.Width, clientRect.Height - HEADER_SIZE);
        }
        else if (tabPos == 1) {
            g.DrawLine(&separatorPen, SIDEBAR_W, 0.0f, SIDEBAR_W, clientRect.Height);
        }
        else if (tabPos == 2) {
            g.DrawLine(&separatorPen, clientRect.Width - SIDEBAR_W, 0.0f, clientRect.Width - SIDEBAR_W, clientRect.Height);
        }

        // ===== Контент =====
        RectF contentArea = GetContentRect(clientRect, tabPos);

        switch (g_activeMainTab) {
        case 0: DrawHomeContent(g, contentArea, contentFont); break;
        case 1:
        case 9:
            ApplyTaskManagerHeaderTab(g_activeMainTab);
            DrawTaskManagerContent(g, contentArea, contentFont);
            break;
        case 2: DrawExplorerContent(g, contentArea, contentFont); break;
        case 3: DrawRegistryContent(g, contentArea, contentFont); break;
        case 4: DrawUnlockContent(g, contentArea, contentFont); break;
        case 5: DrawMonitorContent(g, contentArea, contentFont); break;
        case 6: DrawAccountsContent(g, contentArea, contentFont); break;
        case 7: Notepad::Draw(g, contentArea, contentFont); break;
        case 8: DrawSettingsContent(g, contentArea, contentFont); break;
        case 10: DrawKeybindsContent(g, contentArea, contentFont); break;
        }

        if (g_activeMainTab != 7 && Notepad::IsActive()) {
            Notepad::Hide();
        }
     } // Graphics g(hdcMem)

        BitBlt(hdcScreen, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);

        EndPaint(hWnd, &ps);
        break;
    }

        case WM_SIZE: {
            RECT rc;
            GetClientRect(hWnd, &rc);
            App::Instance()->EnsureBackBuffer(rc.right - rc.left, rc.bottom - rc.top);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

    case WM_TIMER:
        if (wParam == 1001) {
            if (g_activeMainTab == 5 && ActivityMonitor::IsRunning())
                InvalidateRect(hWnd, nullptr, FALSE);
            else
                KillTimer(hWnd, 1001);
        }
        if (wParam == 2002 && (IsTaskManagerHeaderTab(g_activeMainTab) || g_blockProcessLaunches)) {
            TaskManagerOnTimer();
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_MOUSEWHEEL: {
        int activeTab = g_activeMainTab;
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);

        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        RectF clientRect(0, 0, (REAL)rcClient.right, (REAL)rcClient.bottom);
        int tabPos = GetSettingsTabPosition();
        RectF contentArea = GetContentRect(clientRect, tabPos);

        // Слежка отдаём событие панели монитора
        if (activeTab == 5) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);
            if (OnMonitorWheel(pt.x, pt.y, delta)) return 0;
        }

        if (activeTab == 7) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (Notepad::OnMouseWheel(pt.x, pt.y, delta)) return 0;
        }

        // Проводник кастомный скроллбар
        if (activeTab == 2) {
            if (ExplorerMouseWheel(delta, contentArea)) break;
        }

        if (IsTaskManagerHeaderTab(activeTab)) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);
            if (OnTaskManagerWheel(pt.x, pt.y, delta, contentArea)) return 0;
        }

        // Остальные вкладки стандартный скролл
        if (activeTab == 0 || activeTab == 1 ||
            activeTab == 4 || activeTab == 6 || activeTab == 10) {
            int scrollDelta = activeTab == 10
                ? (delta / WHEEL_DELTA) * 90 : delta / 30;
            int newPos = g_scrollOffset[activeTab] - scrollDelta;
            if (newPos < 0) newPos = 0;
            if (newPos > g_maxScroll[activeTab]) newPos = g_maxScroll[activeTab];
            if (newPos != g_scrollOffset[activeTab]) {
                g_scrollOffset[activeTab] = newPos;
                InvalidateRect(hWnd, nullptr, TRUE);
            }
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        float fx = static_cast<float>(x);
        float fy = static_cast<float>(y);

        RECT rcClient;
        GetClientRect(hWnd, &rcClient);

        // Клик по табам
        for (size_t i = 0; i < g_mainTabs.size(); ++i) {
            if (g_horizontalTabRects[i].Contains(fx, fy)) {
                g_draggingFromTab = true;
                g_dragStartPoint = { x, y };
                g_dragTabIndex = (int)i;
                SetCapture(hWnd);
                return 0;
            }
        }

        // Клик по кнопкам окна
        if (g_tabBtnClose.Contains(fx, fy)) {
            PostMessage(hWnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (g_tabBtnMinimize.Contains(fx, fy)) {
            ShowWindow(hWnd, SW_MINIMIZE);
            return 0;
        }

        // Перетаскивание окна
        RectF clientRect(0, 0, (REAL)rcClient.right, (REAL)rcClient.bottom);
        int tabPos = GetSettingsTabPosition();
        RectF headerRect = GetHeaderRect(clientRect, tabPos);
        if (headerRect.Contains(fx, fy)) {
            SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(x, y));
            return 0;
        }

        // Клик по контенту
        RectF contentArea = GetContentRect(clientRect, tabPos);

        switch (g_activeMainTab) {
        case 0: OnHomeClick(x, y, contentArea); break;
        case 1:
        case 9: OnTaskManagerClick(x, y, contentArea); break;
        case 2:
            if (ExplorerLeftButtonDown(x, y, contentArea)) return 0;
            OnExplorerClick(x, y, contentArea);
            break;
        case 3: OnRegistryClick(x, y, contentArea); break;
        case 4: OnUnlockClick(x, y, contentArea); break;
        case 5: OnMonitorClick(x, y, contentArea); break;
        case 6: OnAccountsClick(x, y, contentArea); break;
        case 8: OnSettingsClick(x, y, contentArea); break;
        case 7: Notepad::OnClick(x, y, contentArea); break;
        case 10: OnKeybindsClick(hWnd, x, y); break;
        }
        break;
    }

    case WM_RBUTTONUP: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        RectF clientRect(0, 0, (REAL)rcClient.right, (REAL)rcClient.bottom);
        int tabPos = GetSettingsTabPosition();
        RectF contentArea = GetContentRect(clientRect, tabPos);

        if (IsTaskManagerHeaderTab(g_activeMainTab)) {
            if (OnTaskManagerRightClick(x, y, contentArea)) return 0;
        }
        if (g_activeMainTab == 2) {
            OnExplorerRightClick(x, y, contentArea);
            return 0;
        }
        if (g_activeMainTab == 3) {
            if (OnRegistryRightClick(x, y, contentArea)) return 0;
        }
        if (g_activeMainTab == 5) {
            OnMonitorRightClick(x, y, contentArea);
            return 0;
        }
        if (g_activeMainTab == 6) {
            OnAccountsRightClick(x, y, contentArea);
            return 0;
        }
        break;
    }

    case WM_LBUTTONDBLCLK: {
        if (g_activeMainTab == 2) {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            RectF clientRect(0, 0, (REAL)rcClient.right, (REAL)rcClient.bottom);
            int tabPos = GetSettingsTabPosition();
            RectF contentArea = GetContentRect(clientRect, tabPos);
            if (OnExplorerDoubleClick(x, y, contentArea)) return 0;
        }
        if (IsTaskManagerHeaderTab(g_activeMainTab)) {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            RectF clientRect(0, 0, (REAL)rcClient.right, (REAL)rcClient.bottom);
            int tabPos = GetSettingsTabPosition();
            RectF contentArea = GetContentRect(clientRect, tabPos);
            if (OnTaskManagerDblClick(x, y, contentArea)) return 0;
        }
        if (g_activeMainTab == 5) {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            RectF clientRect(0, 0, (REAL)rcClient.right, (REAL)rcClient.bottom);
            int tabPos = GetSettingsTabPosition();
            RectF contentArea = GetContentRect(clientRect, tabPos);
            OnMonitorDblClick(x, y, contentArea);
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        float fx = static_cast<float>(x);
        float fy = static_cast<float>(y);

        if (g_draggingFromTab) {
            int dx = x - g_dragStartPoint.x;
            int dy = y - g_dragStartPoint.y;
            if (abs(dx) > 5 || abs(dy) > 5) {
                ReleaseCapture();
                SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(x, y));
                g_draggingFromTab = false;
                return 0;
            }
        }

        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        RectF clientRect(0, 0, (REAL)rcClient.right, (REAL)rcClient.bottom);
        int tabPos = GetSettingsTabPosition();
        RectF contentArea = GetContentRect(clientRect, tabPos);

        if (IsTaskManagerHeaderTab(g_activeMainTab)) {
            OnTaskManagerMouseMove(x, y, contentArea);
        }

        if (g_activeMainTab == 2) {
            ExplorerMouseMove(x, y, contentArea);
        }

        if (g_activeMainTab == 5) {
            if (OnMonitorMouseMove(x, y)) return 0;
        }
        if (g_activeMainTab == 7) {
            if (Notepad::OnMouseMove(x, y, contentArea)) return 0;
        }

        // Hover табов
        int newHover = -1;
        for (size_t i = 0; i < g_mainTabs.size(); ++i) {
            if (g_horizontalTabRects[i].Contains(fx, fy)) {
                newHover = (int)i;
                break;
            }
        }
        if (newHover != g_horizontalTabHover) {
            g_horizontalTabHover = newHover;
            InvalidateRect(hWnd, nullptr, TRUE);
        }

        // Hover кнопок окна
        bool hoverMin = g_tabBtnMinimize.Contains(fx, fy);
        bool hoverClose = g_tabBtnClose.Contains(fx, fy);

        if (hoverMin != g_tabBtnMinimizeHover ||
            hoverClose != g_tabBtnCloseHover) {
            g_tabBtnMinimizeHover = hoverMin;
            g_tabBtnCloseHover = hoverClose;
            InvalidateRect(hWnd, nullptr, TRUE);
        }
        break;
    }

    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hWnd, &pt);
        RECT rc;
        GetClientRect(hWnd, &rc);
        const int border = 5;

        bool onLeft = pt.x < border;
        bool onRight = pt.x > rc.right - border;
        bool onTop = pt.y < border;
        bool onBottom = pt.y > rc.bottom - border;

        if (onLeft && onTop) return HTTOPLEFT;
        if (onLeft && onBottom) return HTBOTTOMLEFT;
        if (onRight && onTop) return HTTOPRIGHT;
        if (onRight && onBottom) return HTBOTTOMRIGHT;
        if (onLeft) return HTLEFT;
        if (onRight) return HTRIGHT;
        if (onTop) return HTTOP;
        if (onBottom) return HTBOTTOM;

        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    case WM_LBUTTONUP: {
        if (g_draggingFromTab) {
            ReleaseCapture();
            if (g_dragTabIndex != -1 && g_activeMainTab != g_dragTabIndex) {
                g_activeMainTab = g_dragTabIndex;
                InvalidateRect(hWnd, nullptr, TRUE);
            }
            g_draggingFromTab = false;
            g_dragTabIndex = -1;
            return 0;
        }
        if (OnTaskManagerLButtonUp()) return 0;
        if (ExplorerLeftButtonUp()) return 0;
        if (RegistryLeftButtonUp()) return 0;
        if (OnMonitorMouseUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) return 0;
        Notepad::OnLButtonUp();
        break;
    }

    case WM_COMMAND: {
        if (HIWORD(wParam) == EN_CHANGE && Notepad::IsActive()) {
            Notepad::OnEditChanged();
            return 0;
        }
        break;
    }

    case WM_CTLCOLOREDIT: {
        if (Notepad::IsActive()) {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(230, 230, 230));
            SetBkColor(hdc, RGB(30, 30, 30));
            return (LRESULT)Notepad::GetDarkBrush();
        }
        break;
    }

    case Notepad::WM_NP_APPEND: {
        Notepad::AppendOutput((wchar_t*)wParam);
        return 0;
    }

    case WM_CHAR: {
        if (IsTaskManagerHeaderTab(g_activeMainTab)) {
            if (OnTaskManagerKey(msg, wParam, lParam)) {
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }
        }
        if (IsExplorerAddressBarEditing() && g_activeMainTab == 2) {
            if (ExplorerAddressBarProcessKey(msg, wParam, lParam)) {
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }
        }
        if (g_activeMainTab == 2) {
            if (ExplorerProcessKey(msg, wParam, lParam)) {
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }
        }
        if (g_activeMainTab == 3) {
            if (OnRegistryKey(msg, wParam, lParam)) {
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }
        }
        break;
    }

    case WM_KEYDOWN: {
        if (IsTaskManagerHeaderTab(g_activeMainTab)) {
            if (OnTaskManagerKey(msg, wParam, lParam)) {
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }
        }
        if (IsExplorerAddressBarEditing() && g_activeMainTab == 2) {
            if (ExplorerAddressBarProcessKey(msg, wParam, lParam)) {
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }
        }
        if (g_activeMainTab == 2) {
            if (ExplorerProcessKey(msg, wParam, lParam)) {
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }
        }
        if (g_activeMainTab == 3) {
            if (OnRegistryKey(msg, wParam, lParam)) {
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }
        }
        break;
    }

    case App::WM_TRAYICON: {
        if (lParam == WM_LBUTTONDBLCLK) {
            App::Instance()->RestoreFromTray();
        }
        else if (lParam == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN, L"Открыть");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Выход");
            POINT pt{};
            GetCursorPos(&pt);
            SetForegroundWindow(hWnd);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(menu);
            if (cmd == IDM_TRAY_OPEN) App::Instance()->RestoreFromTray();
            else if (cmd == IDM_TRAY_EXIT) App::Instance()->Quit();
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CLOSE:
        if (RegistryEditor::IsLikelyRecoveryEnvironment()) {
            DestroyWindow(hWnd);
        }
        else {
            App::Instance()->MinimizeToTray();
        }
        return 0;

    case WM_DESTROY:
        TaskManagerShutdown();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}