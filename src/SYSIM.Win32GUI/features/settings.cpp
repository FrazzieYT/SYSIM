#include "settings.h"
#include "taskmgr.h"
#include "core/globals.h"
#include "core/app.h"
#include "ui/widgets.h"
#include "utils/unlock/unlock_tools.h"
#include <uxtheme.h>
#include <string>
#include <vector>

#pragma comment(lib, "uxtheme.lib")

using namespace Gdiplus;

// Состояние настроек
static bool g_defaultsApplied = false;

// Положение панели вкладок: 0=Верх, 1=Лево, 2=Право, 3=Низ
static int g_settingsTabPosition = 1;

// Среда: 0 = Windows 10, 1 = WinPE
static int g_settingsSelectedEnvironment = 0;
static std::wstring g_settingsSavedDrive;

// Диск: -1 = "Все", >=0 = индекс в g_settingsDrives
static int g_settingsSelectedDrive = -1;
static std::vector<std::wstring> g_settingsDrives;
static std::vector<std::wstring> g_settingsDriveDisplayNames;
static bool g_settingsDrivesInitialized = false;

// Прямоугольники для кликов
static RectF g_settingsAllDrivesRect;            // пункт "Все"
static std::vector<RectF> g_settingsDriveRects; // диски C:\, D:\, ...
static std::vector<RectF> g_settingsEnvironmentRects;
static RectF g_settingsTabPosRects[4];         // Верх/Лево/Право/Низ
static RectF g_settingsSystemFontRect;
static RectF g_settingsAlwaysOnTopRect;

static void ApplyFontSelection();
static bool IsRecoveryEnv();

static bool IsSystemLanguageHealthy() {
    LANGID uiLang = GetUserDefaultUILanguage();
    LANGID sysLang = GetSystemDefaultUILanguage();
    return (uiLang != 0 && sysLang != 0);
}

static void RepairSystemLanguageIfNeeded() {
    SetThreadUILanguage(MAKELANGID(LANG_RUSSIAN, SUBLANG_RUSSIAN_RUSSIA));
    SetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, L"ru-RU", nullptr);
    g_useSystemFonts = false;
    ApplyFontSelection();
}

static std::wstring ResolveUsableFontFamilyName(const std::wstring& candidate) {
    if (candidate.empty()) return L"Comic Sans MS";

    if (_wcsicmp(candidate.c_str(), L"Segoe UI") == 0) {
        return L"Comic Sans MS";
    }

    Gdiplus::FontFamily family(candidate.c_str());
    if (family.GetLastStatus() != Gdiplus::Ok) {
        return L"Comic Sans MS";
    }

    WCHAR resolved[256] = {};
    if (family.GetFamilyName(resolved, LANG_NEUTRAL) == Gdiplus::Ok && resolved[0] != 0) {
        return resolved;
    }

    return L"Comic Sans MS";
}

static void ApplyFontSelection() {
    g_useSystemFonts = IsRecoveryEnv();
    if (g_useSystemFonts) {
        g_fontFamilyName = L"Segoe UI";
        return;
    }

    std::wstring preferred = L"Comic Sans MS";
    g_fontFamilyName = ResolveUsableFontFamilyName(preferred);
}

static void SyncOfflineDriveHint() {
    std::wstring preferred;
    if (g_settingsSelectedDrive >= 0 && g_settingsSelectedDrive < (int)g_settingsDrives.size()) {
        preferred = g_settingsDrives[g_settingsSelectedDrive];
    }
    else if (IsRecoveryEnv() && g_settingsDrives.size() == 1) {
        preferred = g_settingsDrives.front();
    }
    if (preferred.empty()) {
        UnlockTools::SetOfflineDriveHint(L"");
        return;
    }
    UnlockTools::SetOfflineDriveHint(preferred);
}

// Утилиты
static bool SettingsHitRect(const RectF& rect, float x, float y) {
    return x >= rect.X &&
        x < rect.X + rect.Width &&
        y >= rect.Y &&
        y < rect.Y + rect.Height;
}

static bool IsRecoveryEnv() {
    wchar_t sysDir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(sysDir, MAX_PATH) &&
        _wcsicmp(sysDir, L"X:\\Windows") == 0) return true;
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\MiniNT",
        0, KEY_READ, &h) == ERROR_SUCCESS) {
        RegCloseKey(h);
        return true;
    }
    return false;
}

static bool DriveHasWindows(const std::wstring& driveRoot) {
    std::wstring sys = driveRoot + L"Windows\\System32\\config\\SYSTEM";
    std::wstring soft = driveRoot + L"Windows\\System32\\config\\SOFTWARE";
    std::wstring kernel = driveRoot + L"Windows\\System32\\ntoskrnl.exe";
    return GetFileAttributesW(sys.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(soft.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(kernel.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static void LoadSettingsFromRegistry() {
    g_settingsTabPosition = 1;
    g_settingsSelectedEnvironment = IsRecoveryEnv() ? 1 : 0;
    g_settingsSelectedDrive = -1;
    g_settingsSavedDrive.clear();
    g_blockSystemHotkeys = false;
    g_keepWindowOnTop = false;
    g_useSystemFonts = false;

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\SYSIM\\Settings",
        0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        ApplyFontSelection();
        return;
    }

    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = REG_DWORD;
    size = sizeof(value);
    type = REG_DWORD;
    if (RegQueryValueExW(hKey, L"SystemHotkeys", nullptr, &type,
        reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS) {
        g_blockSystemHotkeys = (value != 0);
    }

    size = sizeof(value);
    type = REG_DWORD;
    if (RegQueryValueExW(hKey, L"AlwaysOnTop", nullptr, &type,
        reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS) {
        g_keepWindowOnTop = (value != 0);
    }

    size = sizeof(value);
    type = REG_DWORD;
    if (RegQueryValueExW(hKey, L"TabPosition", nullptr, &type,
        reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS) {
        if (value >= 0 && value <= 3) g_settingsTabPosition = static_cast<int>(value);
    }

    size = sizeof(value);
    type = REG_DWORD;
    if (RegQueryValueExW(hKey, L"Environment", nullptr, &type,
        reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS) {
        if (value == 1) g_settingsSelectedEnvironment = 1;
        else g_settingsSelectedEnvironment = 0;
    }

    size = sizeof(value);
    type = REG_DWORD;
    if (RegQueryValueExW(hKey, L"UseSystemFonts", nullptr, &type,
        reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS) {
        g_useSystemFonts = (value != 0);
    }
    else {
        g_useSystemFonts = false;
    }

    wchar_t driveBuffer[MAX_PATH] = {};
    size = sizeof(driveBuffer);
    type = REG_SZ;
    if (RegQueryValueExW(hKey, L"SelectedDrive", nullptr, &type,
        reinterpret_cast<LPBYTE>(driveBuffer), &size) == ERROR_SUCCESS) {
        g_settingsSavedDrive = driveBuffer;
    }

    RegCloseKey(hKey);
    ApplyFontSelection();
}

static void SaveSettingsToRegistry() {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\SYSIM\\Settings",
        0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return;
    }

    DWORD value = g_blockSystemHotkeys ? 1u : 0u;
    RegSetValueExW(hKey, L"SystemHotkeys", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));

    value = g_keepWindowOnTop ? 1u : 0u;
    RegSetValueExW(hKey, L"AlwaysOnTop", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));

    value = g_useSystemFonts ? 1u : 0u;
    RegSetValueExW(hKey, L"UseSystemFonts", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));

    value = static_cast<DWORD>(g_settingsTabPosition);
    RegSetValueExW(hKey, L"TabPosition", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));

    value = static_cast<DWORD>(g_settingsSelectedEnvironment);
    RegSetValueExW(hKey, L"Environment", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));

    std::wstring drive = L"";
    if (g_settingsSelectedDrive >= 0 && g_settingsSelectedDrive < (int)g_settingsDrives.size()) {
        drive = g_settingsDrives[g_settingsSelectedDrive];
    }
    if (!drive.empty()) {
        const DWORD bytes = static_cast<DWORD>((drive.size() + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"SelectedDrive", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(drive.c_str()), bytes);
    }
    else {
        const wchar_t empty = L'\0';
        RegSetValueExW(hKey, L"SelectedDrive", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(&empty), sizeof(wchar_t));
    }

    RegCloseKey(hKey);
}

static void ApplySavedDriveSelection() {
    if (g_settingsSavedDrive.empty()) {
        if (g_settingsSelectedDrive < 0 && !g_settingsDrives.empty()) {
            for (size_t i = 0; i < g_settingsDrives.size(); ++i) {
                if (_wcsicmp(g_settingsDrives[i].c_str(), L"C:\\") == 0) {
                    g_settingsSelectedDrive = (int)i;
                    break;
                }
            }
        }
        return;
    }

    g_settingsSelectedDrive = -1;
    for (size_t i = 0; i < g_settingsDrives.size(); ++i) {
        if (_wcsicmp(g_settingsDrives[i].c_str(), g_settingsSavedDrive.c_str()) == 0) {
            g_settingsSelectedDrive = (int)i;
            break;
        }
    }

    if (g_settingsSelectedDrive < 0 && !g_settingsDrives.empty()) {
        for (size_t i = 0; i < g_settingsDrives.size(); ++i) {
            if (_wcsicmp(g_settingsDrives[i].c_str(), L"C:\\") == 0) {
                g_settingsSelectedDrive = (int)i;
                break;
            }
        }
    }

    g_settingsSavedDrive.clear();
}

static std::wstring GetVolumeLabel(const std::wstring& driveRoot) {
    wchar_t label[MAX_PATH + 1] = {};
    wchar_t fs[MAX_PATH + 1] = {};
    DWORD serial = 0, maxLen = 0, flags = 0;
    if (GetVolumeInformationW(driveRoot.c_str(), label, MAX_PATH,
        &serial, &maxLen, &flags, fs, MAX_PATH) && label[0]) {
        return label;
    }
    return L"";
}

static void EnsureDrivesInitialized() {
    if (g_settingsDrivesInitialized) return;

    g_settingsDrives.clear();
    g_settingsDriveDisplayNames.clear();

    wchar_t buffer[512] = {};
    DWORD len = GetLogicalDriveStringsW(sizeof(buffer) / sizeof(wchar_t), buffer);
    if (len == 0) { g_settingsDrivesInitialized = true; return; }

    bool inRecovery = IsRecoveryEnv();

    wchar_t sysDir[MAX_PATH] = {};
    (void)GetWindowsDirectoryW(sysDir, MAX_PATH);
    std::wstring sysDrive;
    if (sysDir[0] != 0) { sysDrive += sysDir[0]; sysDrive += L":\\"; }

    struct Entry {
        std::wstring root;
        std::wstring display;
        bool hasWindows = false;
    };
    std::vector<Entry> entries;

    for (const wchar_t* p = buffer; *p; p += wcslen(p) + 1) {
        std::wstring drive = p;
        UINT type = GetDriveTypeW(drive.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;

        bool hasWin = DriveHasWindows(drive);
        bool isSysDrive = (!sysDrive.empty() && drive == sysDrive);

        // В recovery: пропускаем X, и всё, где нет Windows
        if (inRecovery) {
            if (isSysDrive) continue;
            if (!hasWin) continue;
        }

        Entry e;
        e.root = drive;
        e.hasWindows = hasWin;

        std::wstring disp = drive.substr(0, 2);  // C:
        if (!inRecovery && isSysDrive) {
            disp += L" (система)";
        }
        else if (hasWin) {
            std::wstring label = GetVolumeLabel(drive);
            disp += label.empty() ? L" (Windows)" : L" (Windows: " + label + L")";
        }
        else {
            std::wstring label = GetVolumeLabel(drive);
            if (!label.empty()) disp += L" (" + label + L")";
        }
        e.display = disp;

        entries.push_back(e);
    }

    for (const auto& e : entries) {
        g_settingsDrives.push_back(e.root);
        g_settingsDriveDisplayNames.push_back(e.display);
    }

    if (!inRecovery) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].root == sysDrive) {
                g_settingsSelectedDrive = static_cast<int>(i);
                break;
            }
        }
        if (g_settingsSelectedDrive < 0 && !g_settingsDrives.empty())
            g_settingsSelectedDrive = 0;
    } else {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (_wcsicmp(entries[i].root.c_str(), L"C:\\") == 0) {
                g_settingsSelectedDrive = static_cast<int>(i);
                break;
            }
        }
        if (g_settingsSelectedDrive < 0 && entries.size() == 1)
            g_settingsSelectedDrive = 0;
    }

    // Переопределяем выбор диска сохранённым значением, если оно есть.
    ApplySavedDriveSelection();
    SyncOfflineDriveHint();
    g_settingsDrivesInitialized = true;
}

// Публичные геттеры
int GetSettingsTabPosition() {
    return g_settingsTabPosition;
}

bool IsDriveMonitored(const std::wstring& drive) {
    EnsureDrivesInitialized();
    if (g_settingsSelectedDrive < 0) return true;   // Все
    if (g_settingsSelectedDrive >= (int)g_settingsDrives.size()) return false;
    return g_settingsDrives[g_settingsSelectedDrive] == drive;
}

std::vector<std::wstring> GetMonitoredDrives() {
    EnsureDrivesInitialized();
    std::vector<std::wstring> result;
    if (g_settingsSelectedDrive < 0) {
        // "Все" отдаём весь список
        result = g_settingsDrives;
    }
    else if (g_settingsSelectedDrive < (int)g_settingsDrives.size()) {
        result.push_back(g_settingsDrives[g_settingsSelectedDrive]);
    }
    return result;
}

// Применение при старте
void ApplyDefaultSettings() {
    LoadSettingsFromRegistry();
    EnsureDrivesInitialized();
    ApplySystemHotkeyProtection();
    g_defaultsApplied = true;
}

static void RebuildDrivesForCurrentEnvironment() {
    g_settingsDrives.clear();
    g_settingsDriveDisplayNames.clear();
    g_settingsDriveRects.clear();
    g_settingsSelectedDrive = -1;
    g_settingsDrivesInitialized = false;
    g_settingsSavedDrive.clear();
    EnsureDrivesInitialized();
}

// Рисование одного пункта списка
static void DrawListItem(
    Graphics& g, const RectF& r, const wchar_t* label,
    bool active, Font& font,
    SolidBrush& textBrush, SolidBrush& mutedBrush,
    SolidBrush& controlBg, SolidBrush& activeBg,
    Pen& borderPen)
{
    SolidBrush& bg = active ? activeBg : controlBg;
    g.FillRectangle(&bg, r);
    g.DrawRectangle(&borderPen, r);

    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentNear);
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    RectF txt(r.X + 10.0f, r.Y, r.Width - 14.0f, r.Height);
    g.DrawString(label, -1, &font, txt, &fmt, active ? &textBrush : &mutedBrush);
}

// Отрисовка
void DrawSettingsContent(Graphics& g, const RectF& contentArea, Font& contentFont) {
    (void)contentFont;

    if (!g_defaultsApplied) {
        EnsureDrivesInitialized();
        g_defaultsApplied = true;
    }

    FontFamily settingsFontFamily(L"Segoe UI");
    Font titleFont(&settingsFontFamily, 15.0f, FontStyleBold, UnitPixel);
    Font itemFont(&settingsFontFamily, 12.0f, FontStyleRegular, UnitPixel);
    Font smallFont(&settingsFontFamily, 11.0f, FontStyleRegular, UnitPixel);

    SolidBrush textBrush(COLOR_TEXT);
    SolidBrush mutedBrush(COLOR_TEXT_MUTED);
    SolidBrush controlBg(COLOR_TAB_BG);
    SolidBrush activeBg(COLOR_TAB_ACTIVE);
    Pen borderPen(COLOR_BORDER, 1.0f);

    StringFormat leftFormat;
    leftFormat.SetAlignment(StringAlignmentNear);
    leftFormat.SetLineAlignment(StringAlignmentCenter);
    leftFormat.SetTrimming(StringTrimmingEllipsisCharacter);

    StringFormat rightFormat;
    rightFormat.SetAlignment(StringAlignmentFar);
    rightFormat.SetLineAlignment(StringAlignmentCenter);
    rightFormat.SetTrimming(StringTrimmingEllipsisCharacter);

    float x = contentArea.X + 16.0f;
    float top = contentArea.Y + 16.0f;
    float titleW = contentArea.Width - 32.0f;

    RectF titleRect(x, top, titleW, 30.0f);
    g.DrawString(L"Настройки", -1, &titleFont, titleRect, &leftFormat, &textBrush);

    // Две колонки: Диск | Панель вкладок
    float colsTop = top + 52.0f;
    const float colGap = 24.0f;
    const float colW = 220.0f;

    float leftColX = x;
    float rightColX = x + colW + colGap;

    const float itemH = 28.0f;
    const float itemGap = 4.0f;

    RectF drivesTitle(leftColX, colsTop, colW, 24.0f);
    g.DrawString(L"Диск:", -1, &itemFont, drivesTitle, &leftFormat, &textBrush);

    float itemY = colsTop + 28.0f;

    // "Все"
    g_settingsAllDrivesRect = RectF(leftColX, itemY, colW, itemH);
    bool allActive = (g_settingsSelectedDrive < 0);
    DrawListItem(g, g_settingsAllDrivesRect, L"Все", allActive, itemFont,
        textBrush, mutedBrush, controlBg, activeBg, borderPen);
    itemY += itemH + itemGap;

    // Пункты дисков
    g_settingsDriveRects.clear();
    g_settingsDriveRects.resize(g_settingsDrives.size());
    for (size_t i = 0; i < g_settingsDrives.size(); ++i) {
        RectF r(leftColX, itemY, colW, itemH);
        g_settingsDriveRects[i] = r;

        bool active = ((int)i == g_settingsSelectedDrive);
        const std::wstring& disp =
            (i < g_settingsDriveDisplayNames.size())
            ? g_settingsDriveDisplayNames[i]
            : g_settingsDrives[i];
        DrawListItem(g, r, disp.c_str(), active, itemFont,
            textBrush, mutedBrush, controlBg, activeBg, borderPen);

        itemY += itemH + itemGap;
    }

    // Правая колонка: Панель вкладок
    RectF tabPosTitle(rightColX, colsTop, colW, 24.0f);
    g.DrawString(L"Панель вкладок:", -1, &itemFont, tabPosTitle, &leftFormat, &textBrush);

    const wchar_t* tabNames[4] = { L"Верх", L"Лево", L"Право", L"Низ" };
    float tabY = colsTop + 28.0f;
    for (int i = 0; i < 4; ++i) {
        RectF r(rightColX, tabY, colW, itemH);
        g_settingsTabPosRects[i] = r;

        bool active = (g_settingsTabPosition == i);
        DrawListItem(g, r, tabNames[i], active, itemFont,
            textBrush, mutedBrush, controlBg, activeBg, borderPen);

        tabY += itemH + itemGap;
    }

    // Экстренный режим шрифтов
    RectF fontTitle(rightColX, tabY + 16.0f, colW, 24.0f);
    g.DrawString(L"Шрифт:", -1, &itemFont, fontTitle, &leftFormat, &textBrush);

    g_settingsSystemFontRect = RectF(rightColX, tabY + 40.0f, colW, itemH);
    const wchar_t* fontState = g_useSystemFonts
        ? L"Авто: системный шрифт (WinRE/WinPE)"
        : L"Авто: Comic Sans MS (Windows)";
    DrawListItem(g, g_settingsSystemFontRect, fontState, g_useSystemFonts, itemFont,
        textBrush, mutedBrush, controlBg, activeBg, borderPen);

    g_settingsAlwaysOnTopRect = RectF(rightColX, tabY + 72.0f, colW, itemH);
    DrawListItem(g, g_settingsAlwaysOnTopRect, L"Поверх всех окон", g_keepWindowOnTop,
        itemFont, textBrush, mutedBrush, controlBg, activeBg, borderPen);
}

// Клики
bool OnSettingsClick(int x, int y, const RectF& contentArea) {
    (void)contentArea;
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);

    // Диск: "Все"
    if (SettingsHitRect(g_settingsAllDrivesRect, fx, fy)) {
        g_settingsSelectedDrive = -1;
        SyncOfflineDriveHint();
        SaveSettingsToRegistry();
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }

    // Диск: конкретный
    for (size_t i = 0; i < g_settingsDriveRects.size(); ++i) {
        if (SettingsHitRect(g_settingsDriveRects[i], fx, fy)) {
            g_settingsSelectedDrive = (int)i;
            SyncOfflineDriveHint();
            SaveSettingsToRegistry();
            InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
            return true;
        }
    }

    // Панель вкладок
    for (int i = 0; i < 4; ++i) {
        if (SettingsHitRect(g_settingsTabPosRects[i], fx, fy)) {
            g_settingsTabPosition = i;
            SaveSettingsToRegistry();
            InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
            return true;
        }
    }

    // Шрифт выбирается автоматически по среде запуска.
    if (SettingsHitRect(g_settingsSystemFontRect, fx, fy)) {
        ApplyFontSelection();
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }

    if (SettingsHitRect(g_settingsAlwaysOnTopRect, fx, fy)) {
        g_keepWindowOnTop = !g_keepWindowOnTop;
        ApplySystemHotkeyProtection();
        SaveSettingsToRegistry();
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }

    return false;
}