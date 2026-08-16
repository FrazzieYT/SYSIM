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

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
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
        float btnSize = 36.0f;
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
        case 1: DrawTaskManagerContent(g, contentArea, contentFont); break;
        case 2: DrawExplorerContent(g, contentArea, contentFont); break;
        case 3: DrawRegistryContent(g, contentArea, contentFont); break;
        case 4: DrawUnlockContent(g, contentArea, contentFont); break;
        case 5: DrawMonitorContent(g, contentArea, contentFont); break;
        case 6: DrawAccountsContent(g, contentArea, contentFont); break;
        case 7: Notepad::Draw(g, contentArea, contentFont); break;
        case 8: DrawSettingsContent(g, contentArea, contentFont); break;
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

        // Проводник кастомный скроллбар
        if (activeTab == 2) {
            if (ExplorerMouseWheel(delta, contentArea)) break;
        }

        // Остальные вкладки стандартный скролл
        if (activeTab == 0 || activeTab == 1 ||
            activeTab == 4 || activeTab == 6) {
            int newPos = g_scrollOffset[activeTab] - delta / 30;
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
        case 1: OnTaskManagerClick(x, y, contentArea); break;
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

        if (g_activeMainTab == 1) {
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
        if (g_activeMainTab == 1) {
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

        if (g_activeMainTab == 1) {
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
        if (IsHomeRunEditing() && g_activeMainTab == 0) {
            if (OnHomeRunKey(msg, wParam, lParam)) return 0;
        }
        if (g_activeMainTab == 1) {
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
        if (g_activeMainTab == 3) {
            if (OnRegistryKey(msg, wParam, lParam)) {
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }
        }
        break;
    }

    case WM_KEYDOWN: {
        if (IsHomeRunEditing() && g_activeMainTab == 0) {
            if (OnHomeRunKey(msg, wParam, lParam)) return 0;
        }
        if (g_activeMainTab == 1) {
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