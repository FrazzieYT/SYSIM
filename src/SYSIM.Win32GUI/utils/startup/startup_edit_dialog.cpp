#include "startup_edit_dialog.h"
#include "core/globals.h"
#include <commdlg.h>
#include <string>
#include <vector>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")

namespace StartupEditDialog {

    static const wchar_t* CLASS_NAME = L"SysimStartupEditDialog";
    static bool g_classRegistered = false;

    struct State {
        Location loc;
        std::vector<Location> customLocations;
        std::wstring valueName;
        std::wstring command;
        bool isCreate = false;
        HWND hLocation = nullptr;
        HWND hName = nullptr;
        HWND hCommand = nullptr;
        HFONT font = nullptr;
        bool ok = false;
        bool done = false;
    };

    static const wchar_t* kRun = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    static const wchar_t* kRunOnce = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";

    static std::vector<Location> GetStandardLocations() {
        return {
            { HKEY_CURRENT_USER,  kRun,     L"HKCU \\ Run",           KEY_WOW64_64KEY },
            { HKEY_CURRENT_USER,  kRunOnce, L"HKCU \\ RunOnce",       KEY_WOW64_64KEY },
            { HKEY_LOCAL_MACHINE, kRun,     L"HKLM \\ Run",           KEY_WOW64_64KEY },
            { HKEY_LOCAL_MACHINE, kRunOnce, L"HKLM \\ RunOnce",       KEY_WOW64_64KEY },
            { HKEY_CURRENT_USER,  kRun,     L"HKCU \\ Run (32)",      KEY_WOW64_32KEY },
            { HKEY_LOCAL_MACHINE, kRun,     L"HKLM \\ Run (32)",      KEY_WOW64_32KEY },
        };
    }

    static bool ReadCommand(const Location& loc, const std::wstring& valueName, std::wstring& out) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(loc.root, loc.subKey.c_str(), 0, KEY_READ | loc.view, &hKey) != ERROR_SUCCESS)
            return false;
        wchar_t buf[4096] = {};
        DWORD sz = sizeof(buf);
        DWORD type = 0;
        LONG r = RegQueryValueExW(hKey, valueName.c_str(), nullptr, &type, (LPBYTE)buf, &sz);
        RegCloseKey(hKey);
        if (r != ERROR_SUCCESS) return false;
        out = buf;
        return true;
    }

    static bool WriteCommand(const Location& loc, const std::wstring& valueName, const std::wstring& command) {
        HKEY hKey = nullptr;
        if (RegCreateKeyExW(loc.root, loc.subKey.c_str(), 0, nullptr, 0,
            KEY_SET_VALUE | loc.view, nullptr, &hKey, nullptr) != ERROR_SUCCESS) return false;

        // Сохраняем тип существующего значения
        DWORD existingType = REG_SZ;
        DWORD typeSize = sizeof(existingType);
        if (RegQueryValueExW(hKey, valueName.c_str(), nullptr,
            &existingType, nullptr, nullptr) != ERROR_SUCCESS ||
            (existingType != REG_SZ && existingType != REG_EXPAND_SZ)) {
            existingType = REG_SZ;
        }

        LONG r = RegSetValueExW(hKey, valueName.c_str(), 0, existingType,
            (const BYTE*)command.c_str(),
            (DWORD)((command.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
        return r == ERROR_SUCCESS;
    }

    static std::wstring BrowseExe(HWND parent) {
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = parent;
        ofn.lpstrFilter = L"Программы (*.exe)\0*.exe\0Все файлы (*.*)\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) return file;
        return L"";
    }

    static std::wstring GetText(HWND h) {
        if (!h || !IsWindow(h)) return L"";
        int len = GetWindowTextLengthW(h);
        if (len <= 0) return L"";
        std::vector<wchar_t> buf(len + 1, L'\0');
        if (GetWindowTextW(h, buf.data(), len + 1) <= 0) return L"";
        return std::wstring(buf.data());
    }
/// @brief 
/// @param hWnd 
/// @param msg 
/// @param wParam 
/// @param lParam 
/// @return 

    static LRESULT CALLBACK DlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        State* st = (State*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

        switch (msg) {
        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wParam;
            RECT rect{};
            GetClientRect(hWnd, &rect);
            HBRUSH brush = CreateSolidBrush(RGB(18, 18, 18));
            FillRect(hdc, &rect, brush);
            DeleteObject(brush);
            return 1;
        }
        case WM_NCHITTEST: {
            POINT p{};
            p.x = LOWORD(lParam);
            p.y = HIWORD(lParam);
            RECT r{};
            GetWindowRect(hWnd, &r);
            if (p.y >= r.top && p.y <= r.top + 28) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, RGB(18, 18, 18));
            SetTextColor(hdc, RGB(245, 245, 245));
            static HBRUSH darkBrush = CreateSolidBrush(RGB(18, 18, 18));
            return (LRESULT)darkBrush;
        }
        /*case WM_CTLCOLORBTN: {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(240, 240, 240));
            return (LRESULT)GetStockObject(NULL_BRUSH);
        } */
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            st = (State*)cs->lpCreateParams;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)st);

            std::wstring fontName = g_useSystemFonts ? L"Segoe UI" : (g_fontFamilyName.empty() ? L"Segoe UI" : g_fontFamilyName);
            st->font = CreateFontW(
                18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                fontName.c_str());
            HFONT font = st->font ? st->font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HINSTANCE hInst = GetModuleHandleW(nullptr);

            int labelW = 120;
            int editX = 150, editW = 430, editH = 24, gap = 10, y = 16;
            int browseW = 34;
            int buttonW = 96;

            HWND hClose = CreateWindowExW(0, L"BUTTON", L"✕",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                600, 8, 22, 22, hWnd, (HMENU)3, hInst, nullptr);
            if (hClose) {
                SendMessageW(hClose, WM_SETFONT, (WPARAM)font, TRUE);
                SendMessageW(hClose, BM_SETSTYLE, (WPARAM)BS_OWNERDRAW, TRUE);
            }

            auto label = [&](const wchar_t* t, int yy) {
                HWND h = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
                    12, yy + 3, labelW, editH, hWnd, nullptr, hInst, nullptr);
                if (h) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
                };
            auto edit = [&](const std::wstring& t, int yy, DWORD style = 0) {
                HWND h = CreateWindowExW(0, L"EDIT", t.c_str(),
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | style,
                    editX, yy, editW, editH, hWnd, nullptr, hInst, nullptr);
                if (h) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
                return h;
                };
             
            label(L"Расположение:", y);
            if (st->isCreate) {
                st->hLocation = CreateWindowExW(0, L"COMBOBOX", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                    editX, y, editW, 200, hWnd, (HMENU)103, hInst, nullptr);
                if (st->hLocation) SendMessageW(st->hLocation, WM_SETFONT, (WPARAM)font, TRUE);
                const auto& locs = st->customLocations.empty()
                    ? GetStandardLocations()
                    : st->customLocations;
                for (const auto& L : locs)
                    SendMessageW(st->hLocation, CB_ADDSTRING, 0, (LPARAM)L.label.c_str());
                SendMessageW(st->hLocation, CB_SETCURSEL, 0, 0);
            }
            else {
                HWND h = CreateWindowExW(0, L"STATIC", st->loc.label.c_str(),
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    editX, y, editW, editH, hWnd, nullptr, hInst, nullptr);
                if (h) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
            }
            y += editH + gap;

            label(L"Имя:", y);
            if (st->isCreate) {
                st->hName = edit(st->valueName, y, 0);
            }
            else {
                st->hName = edit(st->valueName, y, ES_READONLY);
            }
            y += editH + gap;

            label(L"Команда:", y);
            st->hCommand = edit(st->command, y);
            HWND hBrowse = CreateWindowExW(0, L"BUTTON", L"...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                editX + editW + 8, y, browseW, editH,
                hWnd, (HMENU)101, hInst, nullptr);
            if (hBrowse) {
                SendMessageW(hBrowse, WM_SETFONT, (WPARAM)font, TRUE);
            }
            y += editH + 14;

            int contentW = 640;
            RECT clientRect{};
            GetClientRect(hWnd, &clientRect);
            int buttonsX = clientRect.right - 10 - (buttonW * 2) - 10;
            HWND hOk = CreateWindowExW(0, L"BUTTON", st->isCreate ? L"Создать" : L"Сохранить",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
                buttonsX, y, buttonW, 30, hWnd, (HMENU)1, hInst, nullptr);
            if (hOk) {
                SendMessageW(hOk, WM_SETFONT, (WPARAM)font, TRUE);
            }
            HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Отмена",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                buttonsX + buttonW + 10, y, buttonW, 30, hWnd, (HMENU)2, hInst, nullptr);
            if (hCancel) {
                SendMessageW(hCancel, WM_SETFONT, (WPARAM)font, TRUE);
            }

            // SetFocus(st->hName);
            return 0;
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
            if (dis && (dis->CtlID == 1 || dis->CtlID == 2 || dis->CtlID == 3 || dis->CtlID == 101)) {
                RECT r = dis->rcItem;
                // Чёрный фон
                HBRUSH bg = CreateSolidBrush(RGB(18, 18, 18));
                FillRect(dis->hDC, &r, bg);
                DeleteObject(bg);
                // Серая рамка
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
                HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
                SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                Rectangle(dis->hDC, r.left, r.top, r.right, r.bottom);
                SelectObject(dis->hDC, oldPen);
                DeleteObject(pen);
                // Белый текст
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, RGB(245, 245, 245));
                wchar_t text[256] = {};
                GetWindowTextW(dis->hwndItem, text, 256);
                HFONT font = st->font ? st->font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                HGDIOBJ oldFont = SelectObject(dis->hDC, font);
                DrawTextW(dis->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dis->hDC, oldFont);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND: {
            if (!st) break;
            int id = LOWORD(wParam);

            if (id == 1) {
                if (!st->hName || !st->hCommand) return 0;
                std::wstring name = GetText(st->hName);
                std::wstring cmd = GetText(st->hCommand);

                if (name.empty()) {
                    MessageBoxW(hWnd, L"Имя не может быть пустым.", L"Ошибка", MB_OK | MB_ICONERROR);
                    return 0;
                }

                Location loc = st->loc;
                if (st->isCreate && st->hLocation) {
                    int idx = (int)SendMessageW(st->hLocation, CB_GETCURSEL, 0, 0);
                    const auto& locs = st->customLocations.empty()
                        ? GetStandardLocations()
                        : st->customLocations;
                    if (idx >= 0 && idx < (int)locs.size()) loc = locs[idx];
                }

                if (!WriteCommand(loc, name, cmd)) {
                    MessageBoxW(hWnd, L"Не удалось записать в реестр.\r\nВозможно, нужны права администратора.",
                        L"Ошибка", MB_OK | MB_ICONERROR);
                    return 0;
                }

                st->ok = true;
                st->done = true;
                PostMessageW(hWnd, WM_NULL, 0, 0);
                return 0;
            }
            if (id == 2 || id == 3) {
                st->ok = false;
                st->done = true;
                PostMessageW(hWnd, WM_NULL, 0, 0);
                return 0;
            }
            if (id == 101) {
                if (!st->hCommand) return 0;
                std::wstring f = BrowseExe(hWnd);
                if (!f.empty()) {
                    std::wstring cur = GetText(st->hCommand);
                    if (!cur.empty()) f = L"\"" + f + L"\"";
                    else f = L"\"" + f + L"\"";
                    SetWindowTextW(st->hCommand, f.c_str());
                }
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            if (st) { st->ok = false; st->done = true; PostMessageW(hWnd, WM_NULL, 0, 0); }
            return 0;
        case WM_DESTROY:
            if (st && st->font) {
                DeleteObject(st->font);
                st->font = nullptr;
            }
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    static void EnsureClass() {
        if (g_classRegistered) return;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wc);
        g_classRegistered = true;
    }

    static bool RunDialog(HWND parent, State& state, const wchar_t* title) {
        if (!parent || !IsWindow(parent)) return false;

        EnsureClass();
        RECT pr{};
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        int w = 640, h = 220;
        if (GetWindowRect(parent, &pr)) {
            x = pr.left + ((pr.right - pr.left) - w) / 2;
            y = pr.top + ((pr.bottom - pr.top) - h) / 2;
        }
        HWND hWnd = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
            CLASS_NAME, title,
            WS_POPUP,
            x, y, w, h, parent, nullptr, GetModuleHandleW(nullptr), &state);
        if (!hWnd) return false;

        ShowWindow(hWnd, SW_SHOW);
        UpdateWindow(hWnd);
        EnableWindow(parent, FALSE);
        SetForegroundWindow(hWnd);

        HWND focusTarget = state.isCreate ? state.hName : state.hCommand;
        if (focusTarget) SetFocus(focusTarget);

        MSG msg{};
        while (!state.done) {
            BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
            if (ret <= 0) break;
            if (!IsDialogMessageW(hWnd, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        EnableWindow(parent, TRUE);
        if (IsWindow(hWnd)) DestroyWindow(hWnd);
        SetForegroundWindow(parent);
        return state.ok;
    }

    bool ShowEdit(HWND parent, const Location& loc, const std::wstring& valueName) {
        State st{};
        st.isCreate = false;
        st.loc = loc;
        st.valueName = valueName;
        if (!ReadCommand(loc, valueName, st.command)) {
            st.command.clear();
        }
        return RunDialog(parent, st, L"Изменение автозагрузки");
    }

    bool ShowCreate(HWND parent) {
        State st{};
        st.isCreate = true;
        return RunDialog(parent, st, L"Создание записи автозагрузки");
    }

    bool ShowCreate(HWND parent, const std::vector<Location>& locations) {
        State st{};
        st.isCreate = true;
        st.customLocations = locations;
        if (!st.customLocations.empty())
            st.loc = st.customLocations[0];
        return RunDialog(parent, st, L"Создание записи автозагрузки");
    }
}