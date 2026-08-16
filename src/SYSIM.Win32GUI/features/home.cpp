#include "home.h"
#include "core/globals.h"
#include "core/app.h"
#include <windows.h>
#include <shellapi.h>
#include <powrprof.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "comdlg32.lib")

#ifndef SHTDN_REASON_MAJOR_OTHER
#define SHTDN_REASON_MAJOR_OTHER 0x00000000
#endif
#ifndef SHTDN_REASON_MINOR_OTHER
#define SHTDN_REASON_MINOR_OTHER 0x00000000
#endif

// === Запуск ===
static bool Launch(const std::wstring& path, bool asAdmin = false) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(App::Instance()->GetHWND(),
            (L"Файл не найден:\n" + path).c_str(),
            L"Ошибка", MB_OK | MB_ICONERROR);
        return false;
    }
    HINSTANCE result = ShellExecuteW(
        App::Instance()->GetHWND(),
        asAdmin ? L"runas" : L"open",
        path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(App::Instance()->GetHWND(),
            L"Не удалось открыть файл.", L"Ошибка", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

static bool RunCommand(const std::wstring& cmd) {
    if (cmd.empty()) return false;
    DWORD attrs = GetFileAttributesW(cmd.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        HINSTANCE r = ShellExecuteW(nullptr, L"open", cmd.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(r) > 32;
    }
    HINSTANCE r = ShellExecuteW(nullptr, L"open", cmd.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) > 32) return true;

    std::wstring fullCmd = L"cmd.exe /c \"" + cmd + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = fullCmd;
    if (CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }
    return false;
}

// === Кнопки ===
struct HomeButton {
    RectF rect;
    std::wstring text;
};

static std::vector<HomeButton> g_homeButtons;

// === Inline Run ===
static std::wstring g_runText;
static bool g_runActive = false;
static int  g_runCaretPos = 0;
static RectF g_runRect;
static RectF g_runButtonRect;
static RectF g_runBrowseRect;

// === Утилиты ===
static void HomeRedraw() {
    InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
}

static bool HomeHitRect(const RectF& rect, float x, float y) {
    return x >= rect.X && x < rect.X + rect.Width &&
        y >= rect.Y && y < rect.Y + rect.Height;
}

// === Питание ===
static bool HomeEnableShutdownPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &privileges.Privileges[0].Luid)) {
        CloseHandle(token);
        return false;
    }
    BOOL ok = AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    DWORD error = GetLastError();
    CloseHandle(token);
    return ok && error == ERROR_SUCCESS;
}

static void HomeRebootComputer() {
    if (MessageBoxW(App::Instance()->GetHWND(), L"Перезагрузить компьютер?",
        L"Перезагрузка", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    HomeEnableShutdownPrivilege();
    InitiateSystemShutdownExW(nullptr, nullptr, 0, TRUE, TRUE,
        SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER);
}

static void HomeShutdownComputer() {
    if (MessageBoxW(App::Instance()->GetHWND(), L"Выключить компьютер?",
        L"Выключение", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    HomeEnableShutdownPrivilege();
    InitiateSystemShutdownExW(nullptr, nullptr, 0, TRUE, FALSE,
        SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER);
}

static void HomeSleepComputer() {
    if (MessageBoxW(App::Instance()->GetHWND(), L"Перевести компьютер в спящий режим?",
        L"Спящий режим", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (!HomeEnableShutdownPrivilege()) {
        MessageBoxW(App::Instance()->GetHWND(), L"Не удалось получить привилегию для сна.",
            L"Ошибка", MB_OK | MB_ICONERROR);
        return;
    }
    if (!SetSuspendState(FALSE, TRUE, FALSE)) {
        DWORD err = GetLastError();
        wchar_t msg[256];
        wsprintfW(msg, L"SetSuspendState ошибка: %lu", err);
        MessageBoxW(App::Instance()->GetHWND(), msg, L"Сон", MB_OK | MB_ICONERROR);
    }
}

static void HomeLogOff() {
    if (MessageBoxW(App::Instance()->GetHWND(), L"Выйти из системы?",
        L"Выход из системы", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    ExitWindowsEx(EWX_LOGOFF, 0);
}

static void HomeOpenProgramSettings() { g_activeMainTab = 8; HomeRedraw(); }
static void HomeOpenUnlock() { g_activeMainTab = 4; HomeRedraw(); }

static void HomeShowHelp() {
    MessageBoxW(App::Instance()->GetHWND(),
        L"Главная страница\n\n"
        L"• Строка снизу — вводите команды, пути, URL-ы и жмите Enter\n"
        L"• Свёрнутая клавиша Esc отменяет ввод\n"
        L"• Кнопки запускают системные инструменты",
        L"Справка", MB_OK | MB_ICONINFORMATION);
}

// === Run-строка ===
bool IsHomeRunEditing() { return g_runActive; }

void CancelHomeRunEdit() {
    g_runActive = false;
    g_runText.clear();
    g_runCaretPos = 0;
}

static void FinishRunEdit(bool apply) {
    if (apply && !g_runText.empty()) {
        if (!RunCommand(g_runText)) {
            MessageBoxW(App::Instance()->GetHWND(),
                (L"Не удалось выполнить:\n\n" + g_runText).c_str(),
                L"Ошибка", MB_OK | MB_ICONWARNING);
        }
    }
    CancelHomeRunEdit();
}

bool OnHomeRunKey(UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    if (!g_runActive) return false;

    if (msg == WM_CHAR) {
        wchar_t ch = (wchar_t)wParam;
        if (ch == L'\b' || ch == L'\r' || ch == L'\x1b' || ch == L'\t') return true;
        g_runText.insert(g_runText.begin() + g_runCaretPos, ch);
        g_runCaretPos++;
        HomeRedraw();
        return true;
    }
    if (msg == WM_KEYDOWN) {
        // Ctrl-комбинации
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl) {
            switch (wParam) {
            case 'A': case 'a':
                g_runCaretPos = (int)g_runText.size();
                HomeRedraw();
                return true;

            case 'C': case 'c':
                if (!g_runText.empty()) {
                    if (OpenClipboard(nullptr)) {
                        EmptyClipboard();
                        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (g_runText.size() + 1) * sizeof(wchar_t));
                        if (h) {
                            wchar_t* p = (wchar_t*)GlobalLock(h);
                            if (p) {
                                wcscpy_s(p, g_runText.size() + 1, g_runText.c_str());
                                GlobalUnlock(h);
                                SetClipboardData(CF_UNICODETEXT, h);
                            }
                        }
                        CloseClipboard();
                    }
                }
                return true;

            case 'X': case 'x':
                if (!g_runText.empty()) {
                    if (OpenClipboard(nullptr)) {
                        EmptyClipboard();
                        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (g_runText.size() + 1) * sizeof(wchar_t));
                        if (h) {
                            wchar_t* p = (wchar_t*)GlobalLock(h);
                            if (p) {
                                wcscpy_s(p, g_runText.size() + 1, g_runText.c_str());
                                GlobalUnlock(h);
                                SetClipboardData(CF_UNICODETEXT, h);
                            }
                        }
                        CloseClipboard();
                    }
                    g_runText.clear();
                    g_runCaretPos = 0;
                    HomeRedraw();
                }
                return true;

            case 'V': case 'v':
                if (OpenClipboard(nullptr)) {
                    HANDLE h = GetClipboardData(CF_UNICODETEXT);
                    if (h) {
                        wchar_t* p = (wchar_t*)GlobalLock(h);
                        if (p) {
                            std::wstring paste = p;
                            for (wchar_t& c : paste)
                                if (c == L'\r' || c == L'\n') c = L' ';
                            if (paste.size() > 2048) paste.resize(2048);

                            g_runText.insert(g_runCaretPos, paste);
                            g_runCaretPos += (int)paste.size();
                            GlobalUnlock(h);
                            HomeRedraw();
                        }
                    }
                    CloseClipboard();
                }
                return true;
            }
            return true;
        }
        switch (wParam) {
        case VK_RETURN: FinishRunEdit(true); HomeRedraw(); return true;
        case VK_ESCAPE: FinishRunEdit(false); HomeRedraw(); return true;
        case VK_BACK:
            if (g_runCaretPos > 0) {
                g_runText.erase(g_runCaretPos - 1, 1);
                g_runCaretPos--;
                HomeRedraw();
            }
            return true;
        case VK_DELETE:
            if (g_runCaretPos < (int)g_runText.size()) {
                g_runText.erase(g_runCaretPos, 1);
                HomeRedraw();
            }
            return true;
        case VK_LEFT:
            if (g_runCaretPos > 0) g_runCaretPos--;
            HomeRedraw();
            return true;
        case VK_RIGHT:
            if (g_runCaretPos < (int)g_runText.size()) g_runCaretPos++;
            HomeRedraw();
            return true;
        case VK_HOME: g_runCaretPos = 0; HomeRedraw(); return true;
        case VK_END: g_runCaretPos = (int)g_runText.size(); HomeRedraw(); return true;
        default: return false;
        }
    }
    return false;
}

// === Инициализация кнопок ===
void InitHomeButtons() {
    g_homeButtons.clear();

    // Питание
    g_homeButtons.push_back({ RectF(), L"Перезагрузка" });
    g_homeButtons.push_back({ RectF(), L"Выключить" });
    g_homeButtons.push_back({ RectF(), L"Спящий режим" });
    g_homeButtons.push_back({ RectF(), L"Выйти" });
    g_homeButtons.push_back({ RectF(), L"Заблокировать" });

    // Консоли
    g_homeButtons.push_back({ RectF(), L"CMD" });
    g_homeButtons.push_back({ RectF(), L"PowerShell" });
    g_homeButtons.push_back({ RectF(), L"Терминал" });

    // Оснастки
    g_homeButtons.push_back({ RectF(), L"Диспетчер задач" });
    g_homeButtons.push_back({ RectF(), L"Редактор реестра" });
    g_homeButtons.push_back({ RectF(), L"Службы" });
    g_homeButtons.push_back({ RectF(), L"Управление дисками" });
    g_homeButtons.push_back({ RectF(), L"Просмотр событий" });
    g_homeButtons.push_back({ RectF(), L"Планировщик" });
    g_homeButtons.push_back({ RectF(), L"Монитор ресурсов" });
    g_homeButtons.push_back({ RectF(), L"Диспетчер устройств" });

    // Восстановление
    g_homeButtons.push_back({ RectF(), L"Восстановление" });
    g_homeButtons.push_back({ RectF(), L"Разблокировка" });
    g_homeButtons.push_back({ RectF(), L"msconfig" });

    // Настройки
    g_homeButtons.push_back({ RectF(), L"Параметры" });
    g_homeButtons.push_back({ RectF(), L"Панель управления" });
    g_homeButtons.push_back({ RectF(), L"Свойства системы" });
    g_homeButtons.push_back({ RectF(), L"Настройки программы" });

    // Утилиты
    g_homeButtons.push_back({ RectF(), L"Проводник" });
    g_homeButtons.push_back({ RectF(), L"Блокнот" });
    g_homeButtons.push_back({ RectF(), L"Калькулятор" });
    g_homeButtons.push_back({ RectF(), L"Отключить сеть" });
    g_homeButtons.push_back({ RectF(), L"Справка" });
}

// === Отрисовка ===
void DrawHomeContent(Graphics& g, const RectF& contentArea, Font& contentFont) {
    (void)contentFont;

    if (g_homeButtons.empty()) InitHomeButtons();

    const float btnW = 160.0f;
    const float btnH = 34.0f;
    const float gap = 6.0f;
    const float leftMargin = 10.0f;
    const float topMargin = 8.0f;
    const float bottomMargin = 8.0f;
    const float runBarH = 30.0f;
    const float runGap = 8.0f;

    FontFamily ff(g_fontFamilyName.c_str());
    Font runFont(&ff, 11.5f, FontStyleRegular, UnitPixel);
    Font buttonFont(&ff, 11.0f, FontStyleRegular, UnitPixel);

    SolidBrush textBrush(COLOR_TEXT);
    SolidBrush mutedBrush(COLOR_TEXT_MUTED);
    SolidBrush buttonBg(COLOR_BUTTON_BG);
    Pen borderPen(COLOR_BORDER, 1.0f);

    // ===== Область для кнопок (сверху) =====
    float listTop = contentArea.Y + topMargin;
    float listBottom = contentArea.Y + contentArea.Height - bottomMargin - runBarH - runGap;
    float listH = listBottom - listTop;

    int cols = 1;
    float availW = contentArea.Width - 2.0f * leftMargin + gap;
    if (availW > 0) cols = (int)(availW / (btnW + gap));
    if (cols < 1) cols = 1;
    if (cols > 8) cols = 8;

    int n = (int)g_homeButtons.size();
    int rows = (n + cols - 1) / cols;
    float totalH = rows * btnH + (rows - 1) * gap;
    g_maxScroll[0] = (totalH > listH) ? (int)(totalH - listH) : 0;
    if (g_scrollOffset[0] < 0) g_scrollOffset[0] = 0;
    if (g_scrollOffset[0] > g_maxScroll[0]) g_scrollOffset[0] = g_maxScroll[0];

    float gridY = listTop - g_scrollOffset[0];

    StringFormat bf;
    bf.SetAlignment(StringAlignmentCenter);
    bf.SetLineAlignment(StringAlignmentCenter);
    bf.SetTrimming(StringTrimmingEllipsisCharacter);

    for (int i = 0; i < n; ++i) {
        int r = i / cols;
        int c = i % cols;
        float bx = contentArea.X + leftMargin + c * (btnW + gap);
        float by = gridY + r * (btnH + gap);

        g_homeButtons[i].rect = RectF(bx, by, btnW, btnH);

        if (by + btnH < listTop || by > listBottom) continue;

        g.FillRectangle(&buttonBg, g_homeButtons[i].rect);
        g.DrawRectangle(&borderPen, g_homeButtons[i].rect);
        g.DrawString(g_homeButtons[i].text.c_str(), -1, &buttonFont,
            g_homeButtons[i].rect, &bf, &textBrush);
    }

    // ===== Run-строка (снизу) =====
    float runY = contentArea.Y + contentArea.Height - bottomMargin - runBarH;

    float maxRunW = contentArea.Width - 2.0f * leftMargin - 84.0f - 84.0f - 8.0f; // под кнопки
    float runW = (std::min)(maxRunW, 500.0f);
    if (runW < 200.0f) runW = (std::max)(200.0f, maxRunW);

    float runX = contentArea.X + leftMargin;

    g_runRect = RectF(runX, runY, runW, runBarH);
    g_runButtonRect = RectF(g_runRect.X + g_runRect.Width + 6.0f, runY, 80.0f, runBarH);
    g_runBrowseRect = RectF(g_runButtonRect.X + g_runButtonRect.Width + 4.0f, runY, 80.0f, runBarH);

    SolidBrush runBg(g_runActive ? Color(255, 45, 45, 55) : COLOR_TAB_BG);
    g.FillRectangle(&runBg, g_runRect);
    Pen runBorder(g_runActive ? COLOR_TAB_ACTIVE : COLOR_BORDER, g_runActive ? 1.5f : 1.0f);
    g.DrawRectangle(&runBorder, g_runRect);

    StringFormat rf;
    rf.SetAlignment(StringAlignmentNear);
    rf.SetLineAlignment(StringAlignmentCenter);
    rf.SetTrimming(StringTrimmingEllipsisCharacter);

    RectF runTextRect(g_runRect.X + 8.0f, g_runRect.Y, g_runRect.Width - 16.0f, g_runRect.Height);
    const wchar_t* placeholder = L"Команда, путь, URL...";
    if (g_runActive && !g_runText.empty()) {
        g.DrawString(g_runText.c_str(), -1, &runFont, runTextRect, &rf, &textBrush);
        if (g_runCaretPos >= 0 && g_runCaretPos <= (int)g_runText.size()) {
            RectF bb;
            std::wstring prefix = g_runText.substr(0, g_runCaretPos);
            PointF origin(runTextRect.X, runTextRect.Y);
            g.MeasureString(prefix.c_str(), -1, &runFont, origin, &bb);
            Pen caretPen(COLOR_TEXT, 1.0f);
            g.DrawLine(&caretPen,
                runTextRect.X + bb.Width, runTextRect.Y + 5.0f,
                runTextRect.X + bb.Width, runTextRect.Y + runTextRect.Height - 5.0f);
        }
    }
    else {
        g.DrawString(placeholder, -1, &runFont, runTextRect, &rf, &mutedBrush);
        if (g_runActive) {
            Pen caretPen(COLOR_TEXT, 1.0f);
            g.DrawLine(&caretPen,
                runTextRect.X, runTextRect.Y + 5.0f,
                runTextRect.X, runTextRect.Y + runTextRect.Height - 5.0f);
        }
    }

    {
        SolidBrush bg(COLOR_TAB_ACTIVE);
        g.FillRectangle(&bg, g_runButtonRect);
        g.DrawString(L"Выполнить", -1, &buttonFont, g_runButtonRect, &bf, &textBrush);
    }
    {
        g.FillRectangle(&buttonBg, g_runBrowseRect);
        g.DrawRectangle(&borderPen, g_runBrowseRect);
        g.DrawString(L"Обзор...", -1, &buttonFont, g_runBrowseRect, &bf, &textBrush);
    }
}

// === Клики ===
bool OnHomeClick(int x, int y, const RectF& contentArea) {
    (void)contentArea;
    if (g_homeButtons.empty()) InitHomeButtons();

    float fx = (float)x, fy = (float)y;

    // Run-строка
    if (HomeHitRect(g_runRect, fx, fy)) {
        g_runActive = true;
        g_runCaretPos = (int)g_runText.size();
        HomeRedraw();
        return true;
    }
    if (HomeHitRect(g_runButtonRect, fx, fy)) {
        FinishRunEdit(true);
        HomeRedraw();
        return true;
    }
    if (HomeHitRect(g_runBrowseRect, fx, fy)) {
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = App::Instance()->GetHWND();
        ofn.lpstrFilter = L"Все файлы (*.*)\0*.*\0Программы (*.exe)\0*.exe\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            g_runText = file;
            g_runActive = true;
            g_runCaretPos = (int)g_runText.size();
            HomeRedraw();
        }
        return true;
    }

    // Клик вне run-строки деактивирует
    if (g_runActive) {
        g_runActive = false;
        HomeRedraw();
    }

    // Кнопки
    for (size_t i = 0; i < g_homeButtons.size(); ++i) {
        if (!HomeHitRect(g_homeButtons[i].rect, fx, fy)) continue;
        const std::wstring& t = g_homeButtons[i].text;

        if (t == L"Перезагрузка") { HomeRebootComputer(); return true; }
        if (t == L"Выключить") { HomeShutdownComputer(); return true; }
        if (t == L"Спящий режим") { HomeSleepComputer(); return true; }
        if (t == L"Выйти") { HomeLogOff(); return true; }
        if (t == L"Заблокировать") { LockWorkStation(); return true; }

        if (t == L"CMD") { Launch(L"C:\\Windows\\System32\\cmd.exe", true); return true; }
        if (t == L"PowerShell") { Launch(L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", true); return true; }
        if (t == L"Терминал") { Launch(L"C:\\Windows\\System32\\cmd.exe", false); return true; }

        if (t == L"Диспетчер задач") { Launch(L"C:\\Windows\\System32\\taskmgr.exe", false); return true; }
        if (t == L"Редактор реестра") { Launch(L"C:\\Windows\\regedit.exe", true); return true; }
        if (t == L"Службы") { Launch(L"C:\\Windows\\System32\\services.msc", false); return true; }
        if (t == L"Управление дисками") { Launch(L"C:\\Windows\\System32\\diskmgmt.msc", true); return true; }
        if (t == L"Просмотр событий") { Launch(L"C:\\Windows\\System32\\eventvwr.msc", false); return true; }
        if (t == L"Планировщик") { Launch(L"C:\\Windows\\System32\\taskschd.msc", false); return true; }
        if (t == L"Монитор ресурсов") { Launch(L"C:\\Windows\\System32\\resmon.exe", false); return true; }
        if (t == L"Диспетчер устройств") { Launch(L"C:\\Windows\\System32\\devmgmt.msc", true); return true; }

        if (t == L"Восстановление") { Launch(L"C:\\Windows\\System32\\rstrui.exe", true); return true; }
        if (t == L"Разблокировка") { HomeOpenUnlock(); return true; }
        if (t == L"msconfig") { Launch(L"C:\\Windows\\System32\\msconfig.exe", true); return true; }

        if (t == L"Параметры") { Launch(L"ms-settings:", false); return true; }
        if (t == L"Панель управления") { Launch(L"C:\\Windows\\System32\\control.exe", false); return true; }
        if (t == L"Свойства системы") { Launch(L"C:\\Windows\\System32\\sysdm.cpl", false); return true; }
        if (t == L"Настройки программы") { HomeOpenProgramSettings(); return true; }

        if (t == L"Проводник") { Launch(L"C:\\Windows\\explorer.exe", false); return true; }
        if (t == L"Блокнот") { Launch(L"C:\\Windows\\notepad.exe", false); return true; }
        if (t == L"Калькулятор") { Launch(L"C:\\Windows\\System32\\calc.exe", false); return true; }
        if (t == L"Отключить сеть") {
            if (MessageBoxW(App::Instance()->GetHWND(), L"Открыть управление сетевыми подключениями?",
                L"Сеть", MB_YESNO | MB_ICONQUESTION) == IDYES)
                Launch(L"C:\\Windows\\System32\\ncpa.cpl", false);
            return true;
        }
        if (t == L"Справка") { HomeShowHelp(); return true; }

        return true;
    }
    return false;
}