#include "unlock.h"
#include "core/globals.h"
#include "core/app.h"
#include "ui/widgets.h"
#include "utils/unlock/unlock_tools.h"
#include <string>
#include <sstream>
#include <algorithm>
#include <vector>

using namespace Gdiplus;

// --- Глобальные состояния ---
static bool g_immediateUnlock = true;
static bool g_fixBcdSafeBoot = false;
static bool g_fixAcl = false;
static bool g_fixBoot = false;

// Прямоугольники для интерактивных элементов
static RectF g_checkboxRect;        // "Разблок."
static RectF g_bcdCheckboxRect;     // "BCD safeboot"
static RectF g_aclCheckboxRect;     // "ACL"
static RectF g_bootCheckboxRect;    // "Boot"
static RectF g_buttonRect;          // Кнопка "Выполнить"
static RectF g_refreshButtonRect;   // Кнопка "Обновить"

static std::wstring g_lastReport;   // Текст отчёта / лога

// Вспомогательные функции
static bool HitTestRect(const RectF& rect, float x, float y) {
    return x >= rect.X && x < rect.X + rect.Width &&
        y >= rect.Y && y < rect.Y + rect.Height;
}

static void DrawCheckbox(
    Graphics& g, const RectF& boxRect, bool checked, const wchar_t* label,
    Font& font, SolidBrush& textBrush, SolidBrush& controlBg,
    Pen& borderPen, SolidBrush& checkBrush
) {
    g.FillRectangle(&controlBg, boxRect);
    g.DrawRectangle(&borderPen, boxRect);
    if (checked) {
        g.FillRectangle(&checkBrush, RectF(
            boxRect.X + 4.0f, boxRect.Y + 4.0f,
            boxRect.Width - 8.0f, boxRect.Height - 8.0f));
    }
    StringFormat leftFormat;
    leftFormat.SetAlignment(StringAlignmentNear);
    leftFormat.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(label, -1, &font,
        RectF(boxRect.X + 24.0f, boxRect.Y - 4.0f, 120.0f, 24.0f),
        &leftFormat, &textBrush);
}

static void DrawButton(
    Graphics& g, const RectF& r, const wchar_t* label,
    Font& font, SolidBrush& textBrush, SolidBrush& controlBg, Pen& borderPen
) {
    g.FillRectangle(&controlBg, r);
    g.DrawRectangle(&borderPen, r);
    StringFormat cf;
    cf.SetAlignment(StringAlignmentCenter);
    cf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(label, -1, &font, r, &cf, &textBrush);
}

// --- Основная функция рисования ---
void DrawUnlockContent(Graphics& g, const RectF& contentArea, Font& contentFont) {
    (void)contentFont;

    if (g_lastReport.empty()) {
        g_lastReport = UnlockTools::GetBestUnlockReport(false);
    }

    float x = contentArea.X + 10.0f;
    float y = contentArea.Y + 8.0f;

    FontFamily ff(g_fontFamilyName.c_str());
    Font font(&ff, 11.0f, FontStyleRegular, UnitPixel);        // для чекбоксов
    Font smallFont(&ff, 10.5f, FontStyleRegular, UnitPixel);   // для кнопок
    SolidBrush textBrush(COLOR_TEXT);
    SolidBrush controlBg(COLOR_BUTTON_BG);
    SolidBrush checkBrush(COLOR_TEXT);
    Pen borderPen(COLOR_BORDER, 1.0f);

    // ---- Чекбоксы в одну строку ----
    // Распределяем по ширине: 4 чекбокса + отступы
    float checkboxW = 120.0f;
    float checkboxGap = 8.0f;
    float totalCheckW = checkboxW * 4 + checkboxGap * 3;
    float startX = x;

    // Чекбокс 1: "Разблок."
    RectF boxRect1(startX, y, 16.0f, 16.0f);
    g_checkboxRect = RectF(startX - 4.0f, y - 4.0f, checkboxW, 24.0f);
    DrawCheckbox(g, boxRect1, g_immediateUnlock, L"Разблок.",
        font, textBrush, controlBg, borderPen, checkBrush);

    // Чекбокс 2: "BCD safeboot"
    startX += checkboxW + checkboxGap;
    RectF boxRect2(startX, y, 16.0f, 16.0f);
    g_bcdCheckboxRect = RectF(startX - 4.0f, y - 4.0f, checkboxW, 24.0f);
    DrawCheckbox(g, boxRect2, g_fixBcdSafeBoot, L"BCD safeboot",
        font, textBrush, controlBg, borderPen, checkBrush);

    // Чекбокс 3: "ACL"
    startX += checkboxW + checkboxGap;
    RectF boxRect3(startX, y, 16.0f, 16.0f);
    g_aclCheckboxRect = RectF(startX - 4.0f, y - 4.0f, checkboxW, 24.0f);
    DrawCheckbox(g, boxRect3, g_fixAcl, L"ACL",
        font, textBrush, controlBg, borderPen, checkBrush);

    // Чекбокс 4: "Boot"
    startX += checkboxW + checkboxGap;
    RectF boxRect4(startX, y, 16.0f, 16.0f);
    g_bootCheckboxRect = RectF(startX - 4.0f, y - 4.0f, checkboxW, 24.0f);
    DrawCheckbox(g, boxRect4, g_fixBoot, L"Boot",
        font, textBrush, controlBg, borderPen, checkBrush);

    // ---- Кнопки справа от чекбоксов ----
    float btnW = 80.0f;
    float btnH = 22.0f;
    float btnY = y + 1.0f; // небольшое смещение для выравнивания
    float btnX = contentArea.X + contentArea.Width - 10.0f - btnW - 4.0f - btnW; // две кнопки справа

    g_buttonRect = RectF(btnX, btnY, btnW, btnH);
    g_refreshButtonRect = RectF(btnX + btnW + 4.0f, btnY, btnW, btnH);

    DrawButton(g, g_buttonRect,
        g_immediateUnlock ? L"Выполнить" : L"Обновить",
        smallFont, textBrush, controlBg, borderPen);
    DrawButton(g, g_refreshButtonRect, L"Обновить",
        smallFont, textBrush, controlBg, borderPen);

    // ---- Область отчёта / лога ----
    float reportY = y + 28.0f; // отступ после строки чекбоксов
    float reportHeight = contentArea.Height - (reportY - contentArea.Y) - 8.0f;
    if (reportHeight > 10.0f) {
        std::vector<std::wstring> lines;
        std::wstringstream ss(g_lastReport);
        std::wstring line;
        while (std::getline(ss, line)) lines.push_back(line);

        float lineHeight = 16.0f;
        float totalTextHeight = (float)lines.size() * lineHeight;
        g_maxScroll[4] = (totalTextHeight > reportHeight)
            ? (int)(totalTextHeight - reportHeight) : 0;
        int offsetY = g_scrollOffset[4];

        SolidBrush reportBg(Color(45, 45, 45));
        g.FillRectangle(&reportBg,
            RectF(x, reportY, contentArea.Width - 20.0f, reportHeight));
        g.DrawRectangle(&borderPen,
            RectF(x, reportY, contentArea.Width - 20.0f, reportHeight));

        int startLine = 0;
        if (offsetY > 0) startLine = (int)(offsetY / lineHeight);
        int maxLines = (int)(reportHeight / lineHeight) + 1;
        int endLine = (std::min)(startLine + maxLines, (int)lines.size());
        for (int i = startLine; i < endLine; ++i) {
            float yPos = reportY + 4.0f + (i - startLine) * lineHeight - offsetY;
            g.DrawString(lines[i].c_str(), -1, &font,
                PointF(x + 6.0f, yPos), &textBrush);
        }
    }
}

// --- Функция, выполняющая разблокировку и формирующая лог ---
static std::wstring PerformUnlock(bool immediate, bool fixBcd, bool fixAcl, bool fixBoot) {
    std::wstring log;
    int unlockedCount = 0, failedCount = 0;

    auto restrictions = UnlockTools::GetKnownRestrictions();
    for (const auto& r : restrictions) {
        if (!UnlockTools::IsRestricted(r))
            continue;

        if (immediate) {
            if (UnlockTools::UnlockRestriction(r)) {
                ++unlockedCount;
                std::wstring hiveStr;
                for (HKEY h : r.hives) {
                    if (h == HKEY_CURRENT_USER) hiveStr = L"HKCU";
                    else if (h == HKEY_LOCAL_MACHINE) hiveStr = L"HKLM";
                    else hiveStr = L"UNKNOWN";
                }
                log += L"Разблокировано: " + r.description +
                    L" (" + hiveStr + L"\\" + r.subKey + L"\\" + r.valueName +
                    L" = " + std::to_wstring(r.disableValue) + L")\r\n";
            }
            else {
                ++failedCount;
                log += L"Ошибка: " + r.description + L"\r\n";
            }
        }
        else {
            log += L"Обнаружена блокировка: " + r.description + L"\r\n";
        }
    }

    if (immediate) {
        bool hasDisallowRun =
            UnlockTools::HasDisallowRunAt(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer") ||
            UnlockTools::HasDisallowRunAt(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer");
        if (hasDisallowRun) {
            if (UnlockTools::ClearDisallowRun()) {
                log += L"Разблокировано: DisallowRun (удалены все записи)\r\n";
                ++unlockedCount;
            }
            else {
                log += L"Ошибка: не удалось очистить DisallowRun\r\n";
                ++failedCount;
            }
        }

        bool hasIFEO = UnlockTools::HasIFEODebuggerAt(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options");
        if (hasIFEO) {
            if (UnlockTools::ClearImageFileExecutionOptions()) {
                log += L"Разблокировано: IFEO Debugger (удалены все Debugger)\r\n";
                ++unlockedCount;
            }
            else {
                log += L"Ошибка: не удалось очистить IFEO\r\n";
                ++failedCount;
            }
        }
    }

    if (fixBcd && immediate) {
        if (UnlockTools::IsBcdSafeBootEnabled()) {
            if (UnlockTools::ClearBcdSafeBoot()) {
                log += L"BCD: удалено значение safeboot\r\n";
                ++unlockedCount;
            }
            else {
                log += L"BCD: не удалось удалить safeboot\r\n";
                ++failedCount;
            }
        }
        else {
            log += L"BCD: safeboot не обнаружен\r\n";
        }
    }

    if (fixAcl && immediate) {
        wchar_t win[MAX_PATH] = {};
        if (GetWindowsDirectoryW(win, MAX_PATH)) {
            std::wstring aclLog;
            if (UnlockTools::ResetAclOnPath(win, true, aclLog)) {
                log += L"ACL: сброшены права на папку Windows\r\n";
                if (!aclLog.empty()) log += aclLog + L"\r\n";
                ++unlockedCount;
            }
            else {
                log += L"ACL: ошибка при сбросе прав\r\n";
                ++failedCount;
            }
        }
        else {
            log += L"ACL: не удалось определить папку Windows\r\n";
        }
    }

    if (fixBoot && immediate) {
        std::wstring bootLog;
        if (UnlockTools::RepairBootRecords(bootLog)) {
            log += L"Boot: выполнен ремонт записей загрузки\r\n";
            if (!bootLog.empty()) log += bootLog + L"\r\n";
            ++unlockedCount;
        }
        else {
            log += L"Boot: ошибка при ремонте загрузки\r\n";
            ++failedCount;
        }
    }

    if (immediate) {
        std::wstring header;
        header += L"Разблокировано: " + std::to_wstring(unlockedCount) + L"\r\n";
        header += L"Ошибок: " + std::to_wstring(failedCount) + L"\r\n\r\n";
        log = header + log;
    }
    else {
        log = L"--- Список обнаруженных блокировок ---\r\n" + log;
    }

    return log;
}

// --- Обработка кликов ---
bool OnUnlockClick(int x, int y, const RectF& contentArea) {
    (void)contentArea;
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);

    if (HitTestRect(g_checkboxRect, fx, fy)) {
        g_immediateUnlock = !g_immediateUnlock;
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }
    if (HitTestRect(g_bcdCheckboxRect, fx, fy)) {
        g_fixBcdSafeBoot = !g_fixBcdSafeBoot;
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }
    if (HitTestRect(g_aclCheckboxRect, fx, fy)) {
        g_fixAcl = !g_fixAcl;
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }
    if (HitTestRect(g_bootCheckboxRect, fx, fy)) {
        g_fixBoot = !g_fixBoot;
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }

    if (HitTestRect(g_refreshButtonRect, fx, fy)) {
        g_lastReport = UnlockTools::GetBestUnlockReport(false);
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }

    if (HitTestRect(g_buttonRect, fx, fy)) {
        if (g_immediateUnlock) {
            std::wstring log = PerformUnlock(
                true,
                g_fixBcdSafeBoot,
                g_fixAcl,
                g_fixBoot
            );
            g_lastReport = log;
            MessageBoxW(App::Instance()->GetHWND(),
                L"Разблокировка выполнена.\r\nПодробности в отчёте.",
                L"Разблокировка", MB_OK | MB_ICONINFORMATION);
        }
        else {
            g_lastReport = UnlockTools::GetBestUnlockReport(false);
        }
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }

    return false;
}