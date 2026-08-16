#include "startup_edit_dialog.h"
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
        int len = GetWindowTextLengthW(h);
        std::wstring s(len + 1, L'\0');
        GetWindowTextW(h, &s[0], len + 1);
        s.resize(len);
        return s;
    }

    static LRESULT CALLBACK DlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        State* st = (State*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

        switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            st = (State*)cs->lpCreateParams;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)st);

            HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HINSTANCE hInst = GetModuleHandleW(nullptr);

            int labelW = 110;
            int editX = 130, editW = 320, editH = 24, gap = 8, y = 14;

            auto label = [&](const wchar_t* t, int yy) {
                HWND h = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
                    12, yy + 3, labelW, editH, hWnd, nullptr, hInst, nullptr);
                SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
                };
            auto edit = [&](const std::wstring& t, int yy, DWORD style = 0) {
                HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", t.c_str(),
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | style,
                    editX, yy, editW, editH, hWnd, nullptr, hInst, nullptr);
                SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
                return h;
                };
             
            label(L"Расположение:", y);
            if (st->isCreate) {
                st->hLocation = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                    editX, y, editW, 200, hWnd, (HMENU)103, hInst, nullptr);
                SendMessageW(st->hLocation, WM_SETFONT, (WPARAM)font, TRUE);
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
                    editX, y + 3, editW, editH, hWnd, nullptr, hInst, nullptr);
                SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
            }
            y += editH + gap;

            label(L"Имя:", y);
            st->hName = edit(st->valueName, y, st->isCreate ? 0 : ES_READONLY);
            y += editH + gap;

            label(L"Команда:", y);
            st->hCommand = edit(st->command, y);
            HWND hBrowse = CreateWindowExW(0, L"BUTTON", L"...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, editX + editW + 4, y, 34, editH,
                hWnd, (HMENU)101, hInst, nullptr);
            SendMessageW(hBrowse, WM_SETFONT, (WPARAM)font, TRUE);
            y += editH + gap + 10;

            HWND hOk = CreateWindowExW(0, L"BUTTON", st->isCreate ? L"Создать" : L"Сохранить",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                editX + editW - 180, y, 86, 30, hWnd, (HMENU)1, hInst, nullptr);
            SendMessageW(hOk, WM_SETFONT, (WPARAM)font, TRUE);
            HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Отмена",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                editX + editW - 90, y, 86, 30, hWnd, (HMENU)2, hInst, nullptr);
            SendMessageW(hCancel, WM_SETFONT, (WPARAM)font, TRUE);

            SetFocus(st->hName);
            return 0;
        }

        case WM_COMMAND: {
            if (!st) break;
            int id = LOWORD(wParam);

            if (id == 1) {
                std::wstring name = GetText(st->hName);
                std::wstring cmd = GetText(st->hCommand);

                if (name.empty()) {
                    MessageBoxW(hWnd, L"Имя не может быть пустым.", L"Ошибка", MB_OK | MB_ICONERROR);
                    return 0;
                }
                if (cmd.empty()) {
                    MessageBoxW(hWnd, L"Команда не может быть пустой.", L"Ошибка", MB_OK | MB_ICONERROR);
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
            if (id == 2) {
                st->ok = false;
                st->done = true;
                PostMessageW(hWnd, WM_NULL, 0, 0);
                return 0;
            }
            if (id == 101) {
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
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wc);
        g_classRegistered = true;
    }

    static bool RunDialog(HWND parent, State& state, const wchar_t* title) {
        EnsureClass();
        RECT pr{};
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        int w = 480, h = 200;
        if (GetWindowRect(parent, &pr)) {
            x = pr.left + ((pr.right - pr.left) - w) / 2;
            y = pr.top + ((pr.bottom - pr.top) - h) / 2;
        }
        HWND hWnd = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
            CLASS_NAME, title,
            WS_POPUP | WS_CAPTION | WS_SYSMENU,
            x, y, w, h, parent, nullptr, GetModuleHandleW(nullptr), &state);
        if (!hWnd) return false;

        ShowWindow(hWnd, SW_SHOW);
        UpdateWindow(hWnd);
        EnableWindow(parent, FALSE);
        SetForegroundWindow(hWnd);

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
            MessageBoxW(parent, L"Не удалось прочитать значение.", L"Ошибка", MB_OK | MB_ICONERROR);
            return false;
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