#include "service_edit_dialog.h"
#include <commdlg.h>
#include <string>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")

namespace ServiceEditDialog {

    static const wchar_t* CLASS_NAME = L"SysimServiceEditDialog";
    static bool g_classRegistered = false;

    struct State {
        Config cfg;
        bool isCreate = false;
        bool isDriver = false;
        HWND hName = nullptr;
        HWND hDisplayName = nullptr;
        HWND hPath = nullptr;
        HWND hStartType = nullptr;
        HWND hAccount = nullptr;
        HWND hDescription = nullptr;
        bool ok = false;
        bool done = false;
    };

    static bool ReadConfig(const std::wstring& name, Config& cfg) {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) return false;
        SC_HANDLE svc = OpenServiceW(scm, name.c_str(), SERVICE_QUERY_CONFIG);
        if (!svc) { CloseServiceHandle(scm); return false; }

        DWORD need = 0;
        QueryServiceConfigW(svc, nullptr, 0, &need);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            LPQUERY_SERVICE_CONFIGW q = (LPQUERY_SERVICE_CONFIGW)LocalAlloc(LMEM_FIXED, need);
            if (q) {
                if (QueryServiceConfigW(svc, q, need, &need)) {
                    cfg.name = name;
                    cfg.serviceType = q->dwServiceType;
                    if (q->lpDisplayName) cfg.displayName = q->lpDisplayName;
                    if (q->lpBinaryPathName) cfg.binaryPath = q->lpBinaryPathName;
                    if (q->lpServiceStartName) cfg.account = q->lpServiceStartName;
                    cfg.startType = q->dwStartType;
                }
                LocalFree(q);
            }
        }

        DWORD dNeed = 0;
        QueryServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, nullptr, 0, &dNeed);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            LPSERVICE_DESCRIPTIONW d = (LPSERVICE_DESCRIPTIONW)LocalAlloc(LMEM_FIXED, dNeed);
            if (d) {
                if (QueryServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, (LPBYTE)d, dNeed, &dNeed)) {
                    if (d->lpDescription) cfg.description = d->lpDescription;
                }
                LocalFree(d);
            }
        }

        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return true;
    }

    static DWORD ApplyConfig(const Config& cfg, bool isCreate) {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr,
            isCreate ? SC_MANAGER_CREATE_SERVICE : SC_MANAGER_CONNECT);
        if (!scm) return GetLastError();

        DWORD result = ERROR_SUCCESS;

        if (isCreate) {
            SC_HANDLE svc = CreateServiceW(
                scm, cfg.name.c_str(), cfg.displayName.c_str(),
                SERVICE_ALL_ACCESS,
                cfg.serviceType,        // SERVICE_WIN32_OWN_PROCESS
                cfg.startType, SERVICE_ERROR_NORMAL,
                cfg.binaryPath.c_str(),
                nullptr, nullptr, nullptr,
                cfg.account.empty() ? nullptr : cfg.account.c_str(),
                nullptr);
            if (!svc) {
                result = GetLastError();
            }
            else {
                if (!cfg.description.empty()) {
                    SERVICE_DESCRIPTIONW sd{};
                    sd.lpDescription = (LPWSTR)cfg.description.c_str();
                    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &sd);
                }
                CloseServiceHandle(svc);
            }
        }
        else {
            SC_HANDLE svc = OpenServiceW(scm, cfg.name.c_str(), SERVICE_CHANGE_CONFIG);
            if (!svc) {
                result = GetLastError();
            }
            else {
                BOOL ok = ChangeServiceConfigW(
                    svc, SERVICE_NO_CHANGE, cfg.startType, SERVICE_NO_CHANGE,
                    cfg.binaryPath.empty() ? nullptr : cfg.binaryPath.c_str(),
                    nullptr, nullptr, nullptr,
                    cfg.account.empty() ? nullptr : cfg.account.c_str(),
                    nullptr,
                    cfg.displayName.empty() ? nullptr : cfg.displayName.c_str());
                if (!ok) result = GetLastError();

                SERVICE_DESCRIPTIONW sd{};
                sd.lpDescription = (LPWSTR)cfg.description.c_str();
                ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &sd);

                CloseServiceHandle(svc);
            }
        }

        CloseServiceHandle(scm);
        return result;
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

            int labelX = 14, labelW = 120;
            int editX = 140, editW = 340, editH = 24, gap = 8, y = 14;

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

            label(L"Имя службы:", y);
            st->hName = edit(st->cfg.name, y, st->isCreate ? 0 : ES_READONLY);
            y += editH + gap;

            label(L"Отображаемое имя:", y);
            st->hDisplayName = edit(st->cfg.displayName, y);
            y += editH + gap;

            label(L"Путь:", y);
            st->hPath = edit(st->cfg.binaryPath, y);
            HWND hBrowse = CreateWindowExW(0, L"BUTTON", L"...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, editX + editW + 4, y, 34, editH,
                hWnd, (HMENU)101, hInst, nullptr);
            SendMessageW(hBrowse, WM_SETFONT, (WPARAM)font, TRUE);
            y += editH + gap;

            label(L"Тип запуска:", y);
            st->hStartType = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                editX, y, editW, 200, hWnd, (HMENU)102, hInst, nullptr);
            SendMessageW(st->hStartType, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageW(st->hStartType, CB_ADDSTRING, 0, (LPARAM)L"Автоматически");
            SendMessageW(st->hStartType, CB_ADDSTRING, 0, (LPARAM)L"Вручную");
            SendMessageW(st->hStartType, CB_ADDSTRING, 0, (LPARAM)L"Отключена");
            SendMessageW(st->hStartType, CB_ADDSTRING, 0, (LPARAM)L"Загрузочная");
            SendMessageW(st->hStartType, CB_ADDSTRING, 0, (LPARAM)L"Системная");
            int sel = 1;
            if (st->cfg.startType == SERVICE_AUTO_START) sel = 0;
            else if (st->cfg.startType == SERVICE_DEMAND_START) sel = 1;
            else if (st->cfg.startType == SERVICE_DISABLED) sel = 2;
            else if (st->cfg.startType == SERVICE_BOOT_START) sel = 3;
            else if (st->cfg.startType == SERVICE_SYSTEM_START) sel = 4;
            SendMessageW(st->hStartType, CB_SETCURSEL, sel, 0);
            y += editH + gap;

            if (!st->isDriver) {
                label(L"Учётная запись:", y);
                st->hAccount = edit(st->cfg.account, y);
                y += editH + gap;
            }
            else {
                label(L"Тип:", y);
                HWND h = CreateWindowExW(0, L"STATIC",
                    (st->cfg.serviceType == SERVICE_FILE_SYSTEM_DRIVER)
                    ? L"File System Driver"
                    : L"Kernel Driver",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    editX, y + 3, editW, editH, hWnd, nullptr, hInst, nullptr);
                SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
                y += editH + gap;
            }

            label(L"Описание:", y);
            y += 20;
            st->hDescription = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st->cfg.description.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                editX, y, editW, 80, hWnd, nullptr, hInst, nullptr);
            SendMessageW(st->hDescription, WM_SETFONT, (WPARAM)font, TRUE);
            y += 80 + gap + 6;

            HWND hOk = CreateWindowExW(0, L"BUTTON", st->isCreate ? L"Создать" : L"Сохранить",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                editX + editW - 190, y, 90, 30, hWnd, (HMENU)1, hInst, nullptr);
            SendMessageW(hOk, WM_SETFONT, (WPARAM)font, TRUE);
            HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Отмена",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                editX + editW - 95, y, 90, 30, hWnd, (HMENU)2, hInst, nullptr);
            SendMessageW(hCancel, WM_SETFONT, (WPARAM)font, TRUE);

            SetFocus(st->hDisplayName);
            return 0;
        }

        case WM_COMMAND: {
            if (!st) break;
            int id = LOWORD(wParam);

            if (id == 1) {
                st->cfg.name = GetText(st->hName);
                st->cfg.displayName = GetText(st->hDisplayName);
                st->cfg.binaryPath = GetText(st->hPath);
                if (st->hAccount) st->cfg.account = GetText(st->hAccount);
                st->cfg.description = GetText(st->hDescription);

                int idx = (int)SendMessageW(st->hStartType, CB_GETCURSEL, 0, 0);
                switch (idx) {
                case 0: st->cfg.startType = SERVICE_AUTO_START; break;
                case 1: st->cfg.startType = SERVICE_DEMAND_START; break;
                case 2: st->cfg.startType = SERVICE_DISABLED; break;
                case 3: st->cfg.startType = SERVICE_BOOT_START; break;
                case 4: st->cfg.startType = SERVICE_SYSTEM_START; break;
                }

                if (st->cfg.name.empty()) {
                    MessageBoxW(hWnd, L"Имя службы не может быть пустым.", L"Ошибка", MB_OK | MB_ICONERROR);
                    return 0;
                }
                if (st->cfg.binaryPath.empty()) {
                    MessageBoxW(hWnd, L"Путь не может быть пустым.", L"Ошибка", MB_OK | MB_ICONERROR);
                    return 0;
                }

                DWORD err = ApplyConfig(st->cfg, st->isCreate);
                if (err != ERROR_SUCCESS) {
                    wchar_t buf[256];
                    swprintf_s(buf, L"Не удалось сохранить.\r\nКод ошибки: %u", err);
                    MessageBoxW(hWnd, buf, L"Ошибка", MB_OK | MB_ICONERROR);
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
                if (!f.empty()) SetWindowTextW(st->hPath, f.c_str());
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
        int w = 520, h = 410;
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

    bool ShowEdit(HWND parent, const std::wstring& serviceName, bool isDriver) {
        State st{};
        st.isCreate = false;
        st.isDriver = isDriver;
        if (!ReadConfig(serviceName, st.cfg)) {
            MessageBoxW(parent, L"Не удалось прочитать конфигурацию.",
                L"Ошибка", MB_OK | MB_ICONERROR);
            return false;
        }
        const wchar_t* title = isDriver ? L"Изменение драйвера" : L"Изменение службы";
        return RunDialog(parent, st, title);
    }

    bool ShowCreate(HWND parent) {
        State st{};
        st.isCreate = true;
        st.isDriver = false;
        st.cfg.startType = SERVICE_DEMAND_START;
        st.cfg.account = L"LocalSystem";
        return RunDialog(parent, st, L"Создание службы");
    }
}