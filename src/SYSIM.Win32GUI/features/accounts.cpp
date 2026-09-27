#include "accounts.h"
#include "core/globals.h"
#include "core/app.h"
#include "utils/accounts/account_manager.h"
#include <string>
#include <vector>
#include <algorithm>

using namespace Gdiplus;

static std::vector<AccountManager::AccountInfo> g_accounts;
static std::vector<AccountManager::OfflineAccount> g_offAccounts;
static bool g_loaded = false;
static int g_selected = -1;
static std::wstring g_offlineLog;

// Оставляем только кнопку Создать
static RectF g_btnCreate;
static RectF g_btnBackdoor, g_btnRestore;  // для WinRE
static std::vector<RectF> g_rowRects;
static float g_listTop = 0, g_listH = 0;
static int g_rowCount = 0;

static bool Hit(const RectF& r, float x, float y) {
    return x >= r.X && x < r.X + r.Width && y >= r.Y && y < r.Y + r.Height;
}

static void DrawButton(Graphics& g, const RectF& r, const wchar_t* label,
    Font& f, SolidBrush& txt, SolidBrush& bg, Pen& border) {
    g.FillRectangle(&bg, r);
    g.DrawRectangle(&border, r);
    StringFormat cf;
    cf.SetAlignment(StringAlignmentCenter);
    cf.SetLineAlignment(StringAlignmentCenter);
    cf.SetTrimming(StringTrimmingEllipsisCharacter);
    RectF textRect(r.X, r.Y + 2.0f, r.Width, r.Height);
    g.DrawString(label, -1, &f, textRect, &cf, &txt);
}

// Auth Dialog (Name / Pass)
struct PromptState {
    std::wstring prompt;
    std::wstring result;
    bool password = false;
    HWND hEdit = nullptr;
    bool ok = false;
    bool done = false;
};

static LRESULT CALLBACK PromptWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PromptState* st = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        st = reinterpret_cast<PromptState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND hPrompt = CreateWindowExW(0, L"STATIC", st->prompt.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 10, 300, 20, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(hPrompt, WM_SETFONT, (WPARAM)font, TRUE);
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
        if (st->password) style |= ES_PASSWORD;
        st->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", style,
            10, 35, 300, 24, hWnd, (HMENU)100, nullptr, nullptr);
        SendMessageW(st->hEdit, WM_SETFONT, (WPARAM)font, TRUE);
        HWND hOk = CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 140, 70, 80, 26, hWnd, (HMENU)1, nullptr, nullptr);
        HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Отмена",
            WS_CHILD | WS_VISIBLE, 230, 70, 80, 26, hWnd, (HMENU)2, nullptr, nullptr);
        SendMessageW(hOk, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(hCancel, WM_SETFONT, (WPARAM)font, TRUE);
        SetFocus(st->hEdit);
        return 0;
    }
    case WM_COMMAND:
        if (!st) break;
        if (LOWORD(wParam) == 1) {
            wchar_t buf[256]{};
            GetWindowTextW(st->hEdit, buf, 256);
            st->result = buf; st->ok = true; st->done = true;
        }
        else if (LOWORD(wParam) == 2) { st->ok = false; st->done = true; }
        return 0;
    case WM_CLOSE:
        if (st) { st->ok = false; st->done = true; }
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool Prompt(HWND parent, const wchar_t* title, const wchar_t* prompt,
    std::wstring& text, bool password = false) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = PromptWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SYSIM_AccountPrompt";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        reg = true;
    }
    PromptState st;
    st.prompt = prompt;
    st.password = password;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"SYSIM_AccountPrompt",
        title, WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 340, 140, parent, nullptr, nullptr, &st);
    if (!h) return false;
    ShowWindow(h, SW_SHOW);
    EnableWindow(parent, FALSE);
    SetForegroundWindow(h);
    while (!st.done) {
        MSG m{};
        if (GetMessageW(&m, nullptr, 0, 0) <= 0) break;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(parent, TRUE);
    DestroyWindow(h);
    SetForegroundWindow(parent);
    if (st.ok) text = st.result;
    return st.ok;
}

// Drawing
void DrawAccountsContent(Graphics& g, const RectF& contentArea, Font& contentFont) {
    (void)contentFont;
    float x = contentArea.X + 10.0f;
    float y = contentArea.Y + 10.0f;

    FontFamily ff(g_fontFamilyName.c_str());
    Font font(&ff, 12.0f, FontStyleRegular, UnitPixel);
    SolidBrush txt(COLOR_TEXT);
    SolidBrush muted(COLOR_TEXT_MUTED);
    SolidBrush bg(COLOR_BUTTON_BG);
    Pen border(COLOR_BORDER, 1.0f);

    StringFormat lf;
    lf.SetAlignment(StringAlignmentNear);
    lf.SetLineAlignment(StringAlignmentCenter);
    lf.SetTrimming(StringTrimmingEllipsisCharacter);

    // Offline (Recovery Environment)
    if (AccountManager::IsWinRE()) {
        if (!g_loaded) {
            g_offAccounts = AccountManager::GetOfflineAccounts();
            g_loaded = true;
        }
        g.DrawString(L"Среда восстановления: учётные записи оффлайн-Windows",
            -1, &font, PointF(x, y), &lf, &txt);
        y += 26.0f;

        g_listTop = y;
        g_listH = 160.0f;
        SolidBrush listBg(Color(45, 45, 45));
        g.FillRectangle(&listBg, RectF(x, g_listTop, contentArea.Width - 20.0f, g_listH));
        g.DrawRectangle(&border, RectF(x, g_listTop, contentArea.Width - 20.0f, g_listH));

        g_rowCount = (int)g_offAccounts.size();
        g_rowRects.clear();
        const float rowH = 26.0f;
        int off = g_scrollOffset[6];
        if (off < 0) off = 0;
        int startRow = off / (int)rowH;
        int maxRows = (int)(g_listH / rowH) + 1;
        int endRow = (std::min)(startRow + maxRows, g_rowCount);

        for (int i = startRow; i < endRow; ++i) {
            float ry = g_listTop + 2.0f + i * rowH - (float)off;
            RectF rowR(x + 2, ry, contentArea.Width - 24.0f, rowH);
            g_rowRects.push_back(rowR);
            if (i == g_selected) {
                SolidBrush sel(COLOR_TAB_ACTIVE);
                g.FillRectangle(&sel, rowR);
            }
            std::wstring line = g_offAccounts[i].name + L"   (" + g_offAccounts[i].sid + L")";
            RectF textRect(rowR.X + 8.0f, rowR.Y, rowR.Width - 8.0f, rowR.Height);
            g.DrawString(line.c_str(), -1, &font, textRect, &lf, &txt);
        }

        y = g_listTop + g_listH + 12.0f;
        g_btnBackdoor = RectF(x, y, 240.0f, 40.0f);
        g_btnRestore = RectF(x + 250.0f, y, 220.0f, 40.0f);
        DrawButton(g, g_btnBackdoor, L"Проверить среду", font, txt, bg, border);
        DrawButton(g, g_btnRestore, L"Сбросить журнал", font, txt, bg, border);
        y += 52.0f;

        if (!g_offlineLog.empty()) {
            g.DrawString(g_offlineLog.c_str(), -1, &font,
                RectF(x, y, contentArea.Width - 20.0f, 140.0f), &lf, &muted);
        }
        else {
            g.DrawString(
                L"Оффлайн-учётные записи отображаются только для чтения.\r\n"
                L"Изменение системных файлов и входа в WinRE отключено в безопасной сборке.",
                -1, &font, RectF(x, y, contentArea.Width - 20.0f, 120.0f), &lf, &muted);
        }
        return;
    }

    // Online: компактные кнопки (высота 20px, шрифт 10pt)
    if (!g_loaded) {
        g_accounts = AccountManager::GetAccounts();
        g_loaded = true;
    }

    float btnH = 32.0f;
    Font smallFont(&ff, 12.0f, FontStyleRegular, UnitPixel);

    RectF bounds;
    g.MeasureString(L"Создать администратора", -1, &smallFont, PointF(0, 0), &bounds);
    float wCreate = bounds.Width + 26.0f;
    if (wCreate < 100.0f) wCreate = 100.0f;

    float btnX = contentArea.X + contentArea.Width - 10.0f - wCreate;
    float btnY = contentArea.Y + 8.0f;

    g_btnCreate = RectF(btnX, btnY, wCreate, btnH);

    DrawButton(g, g_btnCreate, L"Создать администратора", smallFont, txt, bg, border);

    y = btnY + btnH + 14.0f;

    g.DrawString(L"Пользователи", -1, &smallFont, RectF(x, btnY - 2.0f, 180.0f, 20.0f), &lf, &txt);

    // Список учётных записей
    g_listTop = y;
    g_listH = contentArea.Height - (g_listTop - contentArea.Y) - 8.0f;
    if (g_listH < 10.0f) return;

    SolidBrush listBg(Color(45, 45, 45));
    g.FillRectangle(&listBg, RectF(x, g_listTop, contentArea.Width - 20.0f, g_listH));
    g.DrawRectangle(&border, RectF(x, g_listTop, contentArea.Width - 20.0f, g_listH));

    g_rowCount = (int)g_accounts.size();
    g_rowRects.clear();
    const float rowH = 30.0f;
    float totalH = (float)g_accounts.size() * rowH;
    g_maxScroll[6] = (totalH > g_listH) ? (int)(totalH - g_listH) : 0;
    if (g_scrollOffset[6] < 0) g_scrollOffset[6] = 0;
    if (g_scrollOffset[6] > g_maxScroll[6]) g_scrollOffset[6] = g_maxScroll[6];
    int off = g_scrollOffset[6];

    int startRow = off / (int)rowH;
    int maxRows = (int)(g_listH / rowH) + 1;
    int endRow = (std::min)(startRow + maxRows, g_rowCount);

    for (int i = startRow; i < endRow; ++i) {
        float ry = g_listTop + 2.0f + i * rowH - (float)off;
        RectF rowR(x + 2, ry, contentArea.Width - 24.0f, rowH);
        g_rowRects.push_back(rowR);

        SolidBrush rowBrush(i == g_selected ? COLOR_TAB_ACTIVE : Color(255, 52, 52, 56));
        g.FillRectangle(&rowBrush, rowR);
        g.DrawRectangle(&border, rowR);

        const auto& a = g_accounts[i];
        RectF nameRect(rowR.X + 10.0f, rowR.Y + 2.0f, rowR.Width * 0.52f, rowR.Height - 4.0f);
        g.DrawString(a.name.c_str(), -1, &font, nameRect, &lf, &txt);

        RectF statusRect(rowR.X + rowR.Width - 132.0f, rowR.Y + 6.0f, 70.0f, rowR.Height - 12.0f);
        SolidBrush statusBg(a.enabled ? Color(255, 44, 125, 69) : Color(255, 90, 90, 90));
        g.FillRectangle(&statusBg, statusRect);
        g.DrawRectangle(&border, statusRect);
        StringFormat badgeFmt;
        badgeFmt.SetAlignment(StringAlignmentCenter);
        badgeFmt.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(a.enabled ? L"вкл" : L"выкл", -1, &font, statusRect, &badgeFmt, &txt);

        RectF roleRect(rowR.X + rowR.Width - 52.0f, rowR.Y + 6.0f, 42.0f, rowR.Height - 12.0f);
        SolidBrush roleBg(a.admin ? Color(255, 60, 90, 160) : Color(255, 90, 90, 90));
        g.FillRectangle(&roleBg, roleRect);
        g.DrawRectangle(&border, roleRect);
        g.DrawString(a.admin ? L"адм" : L"польз", -1, &font, roleRect, &badgeFmt, &txt);
    }
}

// Обработка кликов
bool OnAccountsClick(int x, int y, const RectF& contentArea) {
    (void)contentArea;
    float fx = (float)x, fy = (float)y;
    HWND hwnd = App::Instance()->GetHWND();

    // Клик по списку (выбор строки)
    for (size_t i = 0; i < g_rowRects.size(); ++i) {
        if (Hit(g_rowRects[i], fx, fy)) {
            int startRow = g_scrollOffset[6] / 30;
            g_selected = startRow + (int)i;
            InvalidateRect(hwnd, nullptr, TRUE);
            return true;
        }
    }

    // Offline (Recovery Mode)
    if (AccountManager::IsWinRE()) {
        if (Hit(g_btnBackdoor, fx, fy)) {
            MessageBoxW(hwnd,
                L"Проверка офлайн-среды выполнена успешно. Изменение системных файлов и входа в WinRE отключено безопасной сборкой.",
                L"Учётные записи", MB_OK | MB_ICONINFORMATION);
            InvalidateRect(hwnd, nullptr, TRUE);
            return true;
        }
        if (Hit(g_btnRestore, fx, fy)) {
            g_offlineLog.clear();
            InvalidateRect(hwnd, nullptr, TRUE);
            return true;
        }
        return false;
    }

    // Online: кнопка "Создать"
    if (Hit(g_btnCreate, fx, fy)) {
        std::wstring name, pwd;
        if (Prompt(hwnd, L"Новый администратор", L"Имя пользователя:", name)) {
            if (!name.empty() &&
                Prompt(hwnd, L"Новый администратор", L"Пароль:", pwd, true)) {
                DWORD errorCode = 0;
                if (AccountManager::CreateAccount(name, pwd, &errorCode)) {
                    g_loaded = false;
                    MessageBoxW(hwnd, L"Учётная запись добавлена в группу администраторов.",
                        L"Учётные записи", MB_OK | MB_ICONINFORMATION);
                } else {
                    std::wstring error = L"Не удалось создать администратора. Запустите SYSIM с правами администратора.\r\nКод ошибки: " +
                        std::to_wstring(errorCode);
                    MessageBoxW(hwnd, error.c_str(),
                        L"Учётные записи", MB_OK | MB_ICONERROR);
                }
            }
        }
        g_loaded = false;
        InvalidateRect(hwnd, nullptr, TRUE);
        return true;
    }

    return false;
}

// Контекстное меню (ПКМ)
bool OnAccountsRightClick(int x, int y, const RectF& contentArea) {
    (void)contentArea;
    float fx = (float)x, fy = (float)y;
    HWND hwnd = App::Instance()->GetHWND();

    if (AccountManager::IsWinRE()) {
        return false; // в оффлайн-режиме нет контекстного меню
    }

    if (fx < g_listTop || fy < g_listTop || fy > g_listTop + g_listH) return false;

    int row = -1;
    float rowH = 30.0f;
    int off = g_scrollOffset[6];
    float relY = fy - g_listTop - 2.0f + off;
    int index = (int)(relY / rowH);
    if (index >= 0 && index < (int)g_accounts.size()) {
        row = index;
    }
    if (row < 0) return false;

    g_selected = row;
    InvalidateRect(hwnd, nullptr, TRUE);

    const auto& a = g_accounts[row];

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Включить / Выключить");
    AppendMenuW(menu, MF_STRING, 2, L"Сменить пароль");
    AppendMenuW(menu, MF_STRING, 3, L"Удалить");
    AppendMenuW(menu, MF_STRING, 4, a.admin ? L"Убрать из администраторов" : L"Сделать администратором");

    POINT pt{ x, y };
    ClientToScreen(hwnd, &pt);
    SetForegroundWindow(hwnd);

    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd) {
        switch (cmd) {
        case 1: AccountManager::SetEnabled(a.name, !a.enabled); break;
        case 2: {
            std::wstring pwd;
            if (Prompt(hwnd, (L"Пароль для " + a.name).c_str(), L"Новый пароль:", pwd, true)) {
                if (AccountManager::SetPassword(a.name, pwd))
                    MessageBoxW(hwnd, L"Пароль изменён.", L"Учётные записи", MB_OK | MB_ICONINFORMATION);
                else
                    MessageBoxW(hwnd, L"Не удалось сменить пароль.", L"Учётные записи", MB_OK | MB_ICONERROR);
            }
            break;
        }
        case 3:
            if (MessageBoxW(hwnd, (L"Удалить учётную запись \"" + a.name + L"\"?").c_str(),
                L"Учётные записи", MB_YESNO | MB_ICONWARNING) == IDYES) {
                DWORD errorCode = 0;
                if (!AccountManager::DeleteAccount(a.name, &errorCode)) {
                    std::wstring error = L"Не удалось удалить учётную запись. Код ошибки: " +
                        std::to_wstring(errorCode);
                    MessageBoxW(hwnd, error.c_str(), L"Учётные записи", MB_OK | MB_ICONERROR);
                }
                else {
                    g_selected = -1;
                }
            }
            break;
        case 4:
            AccountManager::SetAdmin(a.name, !a.admin);
            break;
        }
        g_loaded = false;
        InvalidateRect(hwnd, nullptr, TRUE);
    }
    return true;
}