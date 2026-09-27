#include "home.h"
#include "core/globals.h"
#include "core/app.h"
#include "features/unlock.h"
#include "features/settings.h"
#include "features/taskmgr.h"
#include "utils/unlock/unlock_tools.h"
#include "utils/accounts/account_manager.h"
#include "utils/file_system/winre_manager.h"
#include "utils/registry/startup_registry.h"
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <powrprof.h>
#include <commdlg.h>
#include <shellapi.h>
#include <urlmon.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "urlmon.lib")

#ifndef SHTDN_REASON_MAJOR_OTHER
#define SHTDN_REASON_MAJOR_OTHER 0x00000000
#endif
#ifndef SHTDN_REASON_MINOR_OTHER
#define SHTDN_REASON_MINOR_OTHER 0x00000000
#endif

// Запуск
static bool Launch(const std::wstring& path, bool asAdmin = false) {
    if (IsProcessLaunchBlocked(App::Instance()->GetHWND())) return false;
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
    if (IsProcessLaunchBlocked(App::Instance()->GetHWND())) return false;
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

static bool OpenWinRARInstaller() {
    const std::wstring candidates[] = {
        L"C:\\Program Files\\WinRAR\\WinRAR.exe",
        L"C:\\Program Files (x86)\\WinRAR\\WinRAR.exe",
        L"C:\\WinRAR\\WinRAR.exe"
    };

    for (const auto& path : candidates) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return Launch(path, false);
        }
    }

    std::wstring tempDir;
    wchar_t tempPathBuf[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tempPathBuf) != 0) {
        tempDir = tempPathBuf;
        if (!tempDir.empty() && tempDir.back() != L'\\') tempDir += L"\\";
    }

    std::wstring installerPath = tempDir.empty() ? L"winrar_installer.exe" : tempDir + L"winrar_installer.exe";
    HRESULT hr = URLDownloadToFileW(nullptr,
        L"https://www.rarlab.com/rar/winrar-x64-621.exe",
        installerPath.c_str(),
        0,
        nullptr);

    if (hr == S_OK && GetFileAttributesW(installerPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return Launch(installerPath, false);
    }

    const std::wstring url = L"https://www.rarlab.com/download.htm";
    HINSTANCE result = ShellExecuteW(
        App::Instance()->GetHWND(),
        L"open",
        url.c_str(),
        nullptr,
        nullptr,
        SW_SHOWNORMAL);

    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Не удалось скачать или открыть WinRAR. Проверьте подключение к интернету.",
            L"SYSIM",
            MB_OK | MB_ICONERROR);
        return false;
    }

    return true;
}

// Кнопки
struct HomeButton {
    RectF rect;
    std::wstring text;
};

static std::vector<HomeButton> g_homeButtons;
static int g_winPeCategory = 0;

// Inline Run
static std::wstring g_runText;
static bool g_runActive = false;
static int  g_runCaretPos = 0;

// Утилиты
static void HomeRedraw() {
    InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
}

static bool HomeHitRect(const RectF& rect, float x, float y) {
    return x >= rect.X && x < rect.X + rect.Width &&
        y >= rect.Y && y < rect.Y + rect.Height;
}

// Питание
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

static bool HomeRunWinPePowerCommand(const wchar_t* operation) {
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    const std::wstring executable = std::wstring(systemDirectory) + L"\\wpeutil.exe";
    if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    std::wstring commandLine = L"\"" + executable + L"\" " + operation;
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo)) return false;
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

static void HomeRebootComputer() {
    if (MessageBoxW(App::Instance()->GetHWND(), L"Перезагрузить компьютер?",
        L"Перезагрузка", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (UnlockTools::IsRecoveryEnvironment()) {
        if (!HomeRunWinPePowerCommand(L"reboot"))
            MessageBoxW(App::Instance()->GetHWND(), L"Не удалось запустить wpeutil reboot.",
                L"WINPE-RE", MB_OK | MB_ICONERROR);
        return;
    }
    HomeEnableShutdownPrivilege();
    InitiateSystemShutdownExW(nullptr, nullptr, 0, TRUE, TRUE,
        SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER);
}

static void HomeShutdownComputer() {
    if (MessageBoxW(App::Instance()->GetHWND(), L"Выключить компьютер?",
        L"Выключение", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (UnlockTools::IsRecoveryEnvironment()) {
        if (!HomeRunWinPePowerCommand(L"shutdown"))
            MessageBoxW(App::Instance()->GetHWND(), L"Не удалось запустить wpeutil shutdown.",
                L"WINPE-RE", MB_OK | MB_ICONERROR);
        return;
    }
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

static std::wstring HomeGetOfflineWinREStatus(const std::wstring& selectedDrive) {
    const std::wstring windowsRoot = selectedDrive + L"Windows";
    const std::wstring reagentcPath = windowsRoot + L"\\System32\\reagentc.exe";
    const std::wstring recoveryDirectory = windowsRoot + L"\\System32\\Recovery";
    const std::wstring configurationPath = recoveryDirectory + L"\\ReAgent.xml";
    const std::wstring imagePath = recoveryDirectory + L"\\Winre.wim";
    const bool reagentcExists = GetFileAttributesW(reagentcPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool configurationExists = GetFileAttributesW(configurationPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool imageExists = GetFileAttributesW(imagePath.c_str()) != INVALID_FILE_ATTRIBUTES;

    return L"Целевая Windows: " + windowsRoot + L"\r\n" +
        L"reagentc.exe: " + (reagentcExists ? L"найден" : L"нет в офлайн-образе") + L"\r\n" +
        L"ReAgent.xml: " + (configurationExists ? L"найден" : L"не найден") + L"\r\n" +
        L"Winre.wim в System32\\Recovery: " + (imageExists ? L"найден" : L"не найден") +
        L"\r\n\r\nУтилита reagentc отсутствует в текущей WinRE; проверено целевое хранилище. "
        L"Раздел X: не использовался.";
}

static void HomeShowWinREManager() {
    HWND owner = App::Instance()->GetHWND();
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT directoryLength = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (directoryLength == 0 || directoryLength >= MAX_PATH) {
        MessageBoxW(owner, L"Не удалось определить каталог Windows System32.",
            L"Менеджер WinRE", MB_OK | MB_ICONERROR);
        return;
    }

    const bool inRecovery = UnlockTools::IsRecoveryEnvironment();
    std::wstring selectedDrive;
    std::wstring targetWindows;
    std::wstring reagentcPath;
    std::wstring parameters = L"/info";
    if (inRecovery) {
        selectedDrive = UnlockTools::GetOfflineDriveHint();
        const auto installations = UnlockTools::ScanDrivesWithWindows();
        const bool selectedTargetFound = !selectedDrive.empty() &&
            std::any_of(installations.begin(), installations.end(),
                [&](const std::wstring& drive) {
                    return _wcsicmp(drive.c_str(), selectedDrive.c_str()) == 0;
                });
        if (!selectedTargetFound) {
            MessageBoxW(owner,
                L"Сначала выберите в настройках доступный раздел установленной Windows. "
                L"Системный раздел WinRE X: не считается целевой установкой.",
                L"Менеджер WinRE", MB_OK | MB_ICONWARNING);
            return;
        }
        targetWindows = selectedDrive + L"Windows";
        reagentcPath = targetWindows + L"\\System32\\reagentc.exe";
        parameters += L" /target \"" + targetWindows + L"\"";
        if (GetFileAttributesW(reagentcPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const std::wstring status = HomeGetOfflineWinREStatus(selectedDrive);
            MessageBoxW(owner, status.c_str(), L"Менеджер WinRE · офлайн-статус",
                MB_OK | MB_ICONINFORMATION);
            return;
        }
    }
    else {
        reagentcPath = std::wstring(systemDirectory) + L"\\reagentc.exe";
    }

    SECURITY_ATTRIBUTES securityAttributes{ sizeof(securityAttributes), nullptr, TRUE };
    HANDLE outputRead = nullptr;
    HANDLE outputWrite = nullptr;
    if (!CreatePipe(&outputRead, &outputWrite, &securityAttributes, 0) ||
        !SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0)) {
        if (outputRead) CloseHandle(outputRead);
        if (outputWrite) CloseHandle(outputWrite);
        MessageBoxW(owner, L"Не удалось создать канал для чтения статуса WinRE.",
            L"Менеджер WinRE", MB_OK | MB_ICONERROR);
        return;
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = outputWrite;
    startupInfo.hStdError = outputWrite;
    PROCESS_INFORMATION processInfo{};
    std::wstring commandLine = L"\"" + reagentcPath + L"\" " + parameters;
    const BOOL started = CreateProcessW(reagentcPath.c_str(), commandLine.data(),
        nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
        &startupInfo, &processInfo);
    CloseHandle(outputWrite);
    if (!started) {
        const DWORD errorCode = GetLastError();
        CloseHandle(outputRead);
        if (inRecovery) {
            const std::wstring status = HomeGetOfflineWinREStatus(selectedDrive) +
                L"\r\n\r\nНе удалось запустить офлайн reagentc.exe. Код Windows: " +
                std::to_wstring(errorCode);
            MessageBoxW(owner, status.c_str(), L"Менеджер WinRE · офлайн-статус",
                MB_OK | MB_ICONWARNING);
            return;
        }
        wchar_t message[192]{};
        swprintf_s(message, ARRAYSIZE(message),
            L"Не удалось запустить reagentc.exe. Код Windows: %lu", errorCode);
        MessageBoxW(owner, message, L"Менеджер WinRE", MB_OK | MB_ICONERROR);
        return;
    }

    const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, 15000);
    if (waitResult != WAIT_OBJECT_0) {
        TerminateProcess(processInfo.hProcess, ERROR_TIMEOUT);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        CloseHandle(outputRead);
        if (inRecovery) {
            const std::wstring status = HomeGetOfflineWinREStatus(selectedDrive) +
                L"\r\n\r\nОфлайн reagentc.exe не завершился за 15 секунд.";
            MessageBoxW(owner, status.c_str(), L"Менеджер WinRE · офлайн-статус",
                MB_OK | MB_ICONWARNING);
            return;
        }
        MessageBoxW(owner, L"reagentc /info не завершился за 15 секунд.",
            L"Менеджер WinRE", MB_OK | MB_ICONWARNING);
        return;
    }

    DWORD exitCode = ERROR_GEN_FAILURE;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    std::string output;
    char buffer[2048];
    DWORD bytesRead = 0;
    while (ReadFile(outputRead, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        const size_t remaining = output.size() < 65536 ? 65536 - output.size() : 0;
        output.append(buffer, (std::min)(remaining, static_cast<size_t>(bytesRead)));
    }
    CloseHandle(outputRead);

    std::wstring status;
    if (!output.empty()) {
        const int charCount = MultiByteToWideChar(CP_OEMCP, 0, output.data(),
            static_cast<int>(output.size()), nullptr, 0);
        if (charCount > 0) {
            status.resize(charCount);
            MultiByteToWideChar(CP_OEMCP, 0, output.data(),
                static_cast<int>(output.size()), status.data(), charCount);
        }
    }
    if (status.empty()) status = L"reagentc.exe не вернул текстовый статус.";

    std::wstring report = L"Целевой Windows-диск: " +
        (inRecovery ? selectedDrive : L"текущая Windows") +
        L"\r\nКод выполнения reagentc: " + std::to_wstring(exitCode) +
        L"\r\n\r\n" + status;
    MessageBoxW(owner, report.c_str(), L"Менеджер WinRE · состояние",
        MB_OK | (exitCode == ERROR_SUCCESS ? MB_ICONINFORMATION : MB_ICONWARNING));
}

static void HomeRunUnlockAll() {
    std::wstring report = UnlockTools::GetBestUnlockReport(true);
    MessageBoxW(App::Instance()->GetHWND(), report.c_str(),
        L"Восстановление ограничений", MB_OK | MB_ICONINFORMATION);
}

static void HomeShowIfeoDebuggers(bool removeEntries) {
    HWND owner = App::Instance()->GetHWND();
    if (removeEntries && MessageBoxW(owner,
        L"Удалить найденные значения IFEO Debugger из выбранной Windows?\r\n\r\n"
        L"Будут изменены только значения Debugger; пользовательские NTUSER.DAT не требуются.",
        L"Удаление IFEO-дебаггеров", MB_YESNO | MB_ICONWARNING) != IDYES) return;

    const std::wstring report = UnlockTools::GetIfeoDebuggerReport(removeEntries);
    MessageBoxW(owner, report.c_str(),
        removeEntries ? L"IFEO-дебаггеры удалены" : L"Сканирование IFEO-дебаггеров",
        MB_OK | (report.find(L"ошибок: 0") != std::wstring::npos
            ? MB_ICONINFORMATION : MB_ICONWARNING));
}

static std::wstring HomeCurrentExePath() {
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return std::wstring();
    }
    return std::wstring(path);
}

static std::wstring HomeReadWinlogonValue(const wchar_t* valueName) {
    HKEY hKey = nullptr;
    std::wstring result;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
        0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return result;
    }

    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(hKey, valueName, nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size > 0) {
        std::vector<wchar_t> buffer((size / sizeof(wchar_t)) + 1, L'\0');
        if (RegQueryValueExW(hKey, valueName, nullptr, &type,
            reinterpret_cast<LPBYTE>(buffer.data()), &size) == ERROR_SUCCESS) {
            result = buffer.data();
        }
    }

    RegCloseKey(hKey);
    return result;
}

static void HomeCheckShellAndUserinit() {
    std::wstring shell = HomeReadWinlogonValue(L"Shell");
    std::wstring userinit = HomeReadWinlogonValue(L"Userinit");

    std::wstring message = L"Текущее состояние Winlogon:\n\n";
    message += L"Shell: " + (shell.empty() ? L"<не задано>" : shell) + L"\n";
    message += L"Userinit: " + (userinit.empty() ? L"<не задано>" : userinit) + L"\n";

    MessageBoxW(App::Instance()->GetHWND(), message.c_str(),
        L"Мониторинг Shell и Userinit", MB_OK | MB_ICONINFORMATION);
    g_activeMainTab = 5;
    HomeRedraw();
}

static bool HomeSetRegistryStringValue(HKEY root, const std::wstring& regPath,
    const std::wstring& valueName, const std::wstring& value) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(root, regPath.c_str(), 0, nullptr, 0,
        KEY_WRITE | KEY_WOW64_64KEY, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    LONG status = RegSetValueExW(hKey, valueName.c_str(), 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(hKey);
    return status == ERROR_SUCCESS;
}

static bool HomeSetRegistryDwordValue(HKEY root, const std::wstring& regPath,
    const std::wstring& valueName, DWORD value) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(root, regPath.c_str(), 0, nullptr, 0,
        KEY_WRITE | KEY_WOW64_64KEY, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LONG status = RegSetValueExW(hKey, valueName.c_str(), 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(hKey);
    return status == ERROR_SUCCESS;
}

static void HomeAddAppToAllStartupLocations() {
    if (UnlockTools::IsRecoveryEnvironment()) {
        MessageBoxW(App::Instance()->GetHWND(),
            L"Автозапуск из WinRE не добавлен: текущая программа находится в X: и не будет доступна "
            L"после перезагрузки. Запустите SYSIM из установленной Windows.",
            L"Автозагрузка", MB_OK | MB_ICONWARNING);
        return;
    }

    std::wstring exePath = HomeCurrentExePath();
    if (exePath.empty()) {
        MessageBoxW(App::Instance()->GetHWND(), L"Не удалось определить путь к текущему exe.",
            L"Автозагрузка", MB_OK | MB_ICONERROR);
        return;
    }

    std::wstring command = L'"' + exePath + L'"';
    struct StartupTarget { HKEY root; const wchar_t* regPath; const wchar_t* valueName; };
    const StartupTarget targets[] = {
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"SYSIM" },
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"SYSIM" }
    };

    int added = 0;
    for (const auto& target : targets) {
        if (StartupRegistry::AddEntry(target.root, target.regPath, target.valueName, command, 0)) {
            ++added;
        }
    }

    if (added > 0) {
        MessageBoxW(App::Instance()->GetHWND(),
            L"SYSIM добавлен в стандартную автозагрузку текущей Windows. Параметры Shell и Setup не изменялись.",
            L"Автозагрузка", MB_OK | MB_ICONINFORMATION);
        g_activeMainTab = 9;
        g_activeSubTab = SUBTAB_STARTUP;
        HomeRedraw();
    }
    else {
        MessageBoxW(App::Instance()->GetHWND(), L"Не удалось добавить приложение в автозагрузку безопасно.",
            L"Автозагрузка", MB_OK | MB_ICONERROR);
    }
}

static void HomeRestoreShellSetupAndSetAppStart() {
    if (UnlockTools::IsRecoveryEnvironment()) {
        MessageBoxW(App::Instance()->GetHWND(),
            L"Изменение Shell/Setup из WinRE отключено: оно меняло бы только среду X:, "
            L"а не выбранную установленную Windows.",
            L"Восстановление запуска", MB_OK | MB_ICONWARNING);
        return;
    }

    std::wstring exePath = HomeCurrentExePath();
    if (exePath.empty()) {
        MessageBoxW(App::Instance()->GetHWND(), L"Не удалось определить путь к текущему exe.",
            L"Восстановление запуска", MB_OK | MB_ICONERROR);
        return;
    }

    std::wstring command = L'"' + exePath + L'"';
    int okCount = 0;

    if (HomeSetRegistryStringValue(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
        L"Shell", L"explorer.exe")) {
        ++okCount;
    }
    if (HomeSetRegistryStringValue(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
        L"Userinit", L"C:\\Windows\\system32\\userinit.exe,")) {
        ++okCount;
    }
    if (HomeSetRegistryStringValue(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", L"CmdLine", command)) {
        ++okCount;
    }
    if (HomeSetRegistryDwordValue(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", L"SetupType", 2u)) {
        ++okCount;
    }
    if (HomeSetRegistryDwordValue(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        L"EnableCursorSuppression", 0u)) {
        ++okCount;
    }

    const struct StartupTarget { HKEY root; const wchar_t* regPath; const wchar_t* valueName; } targets[] = {
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"SYSIM" },
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"SYSIM" },
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", L"SYSIM" },
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", L"SYSIM" }
    };

    for (const auto& target : targets) {
        if (StartupRegistry::AddEntry(target.root, target.regPath, target.valueName, command, 0)) {
            ++okCount;
        }
    }

    if (okCount > 0) {
        MessageBoxW(App::Instance()->GetHWND(),
            (L"Shell/Userinit/Setup восстановлены и запускаются безопасно через текущую программу (операций: " + std::to_wstring(okCount) + L").").c_str(),
            L"Восстановление запуска", MB_OK | MB_ICONINFORMATION);
        g_activeMainTab = 9;
        g_activeSubTab = SUBTAB_STARTUP;
        HomeRedraw();
    }
    else {
        MessageBoxW(App::Instance()->GetHWND(),
            L"Не удалось восстановить Shell/Userinit/Setup и выставить запуск через текущую программу.",
            L"Восстановление запуска", MB_OK | MB_ICONERROR);
    }
}

static void HomeInstallUtilmanBackdoor() {
    std::wstring log;
    bool ok = AccountManager::InstallLoginShellBackdoor(log);
    MessageBoxW(App::Instance()->GetHWND(),
        log.c_str(),
        ok ? L"Подмена utilman/sethc" : L"Ошибка замены",
        MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
}

static void HomeRunUtilmanRepair() {
    std::wstring log;
    bool ok = AccountManager::RemoveLoginShellBackdoor(log);
    MessageBoxW(App::Instance()->GetHWND(),
        log.c_str(),
        ok ? L"Восстановление utilman/sethc" : L"Ошибка восстановления",
        MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
}

static bool HomeRunElevatedCommand(const std::wstring& command) {
    std::wstring workingDirectory;
    if (UnlockTools::IsRecoveryEnvironment()) {
        workingDirectory = UnlockTools::GetOfflineDriveHint();
        const auto installations = UnlockTools::ScanDrivesWithWindows();
        const bool targetIsInstalledWindows = std::any_of(
            installations.begin(), installations.end(), [&](const std::wstring& drive) {
                return _wcsicmp(drive.c_str(), workingDirectory.c_str()) == 0;
            });
        if (workingDirectory.empty() || !targetIsInstalledWindows) {
            MessageBoxW(App::Instance()->GetHWND(),
                L"Выбранный диск больше недоступен или не содержит установленную Windows.\r\n"
                L"Обновите выбор диска в настройках.",
                L"WinPE", MB_OK | MB_ICONWARNING);
            return false;
        }
    }
    std::wstring commandLine;
    if (workingDirectory.empty()) {
        commandLine = L"/d /c \"" + command + L" & pause\"";
    } else {
        commandLine = L"/d /c \"set \\\"SystemDrive=" + workingDirectory.substr(0, 2) +
            L"\\\"&&set \\\"SystemRoot=" + workingDirectory + L"Windows\\\"&&cd /d \\\"" +
            workingDirectory + L"\\\"&&" + command + L" & pause\"";
    }
    HINSTANCE result = ShellExecuteW(
        App::Instance()->GetHWND(), L"runas", L"cmd.exe",
        commandLine.c_str(),
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(), SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

struct WinPeHomeAction {
    const wchar_t* label;
    const wchar_t* command;
};

static const std::vector<WinPeHomeAction>& HomeGetWinPeActions() {
    static const std::vector<WinPeHomeAction> actions = {
        { L"Корень выбранного диска", L"dir /a \"{D}\\\"" },
        { L"Папка Windows", L"dir /a \"{D}\\Windows\"" },
        { L"System32", L"dir /a \"{D}\\Windows\\System32\"" },
        { L"Драйверы Windows", L"dir /a \"{D}\\Windows\\System32\\drivers\"" },
        { L"DriverStore", L"dir /a \"{D}\\Windows\\System32\\DriverStore\\FileRepository\"" },
        { L"WinSxS", L"dir /a \"{D}\\Windows\\WinSxS\"" },
        { L"Профили пользователей", L"dir /ad \"{D}\\Users\"" },
        { L"Системные задачи", L"dir /a \"{D}\\Windows\\System32\\Tasks\"" },
        { L"Журналы CBS", L"dir /a \"{D}\\Windows\\Logs\\CBS\"" },
        { L"Журналы DISM", L"dir /a \"{D}\\Windows\\Logs\\DISM\"" },
        { L"Журналы SetupAPI", L"dir /a \"{D}\\Windows\\INF\\setupapi*.log\"" },
        { L"Журналы установки", L"dir /a \"{D}\\Windows\\Panther\"" },
        { L"Журналы загрузки", L"dir /a \"{D}\\Windows\\System32\\LogFiles\\Srt\"" },
        { L"Minidump", L"dir /a \"{D}\\Windows\\Minidump\"" },
        { L"Memory dump", L"dir /a \"{D}\\Windows\\MEMORY.DMP\"" },
        { L"WinRE image", L"dir /a \"{D}\\Windows\\System32\\Recovery\"" },
        { L"BCD BIOS", L"dir /a \"{D}\\Boot\\BCD\"" },
        { L"BCD EFI", L"dir /a \"{D}\\EFI\\Microsoft\\Boot\\BCD\"" },
        { L"Boot manager", L"dir /a \"{D}\\bootmgr\"" },
        { L"Boot resources", L"dir /a \"{D}\\Windows\\Boot\"" },
        { L"Проверка dirty bit", L"fsutil dirty query {D}" },
        { L"Информация тома", L"fsutil fsinfo volumeinfo {D}" },
        { L"Информация NTFS", L"fsutil fsinfo ntfsinfo {D}" },
        { L"Статистика NTFS", L"fsutil fsinfo statistics {D}" },
        { L"Проверка файловой системы", L"chkdsk {D} /scan" },
        { L"Проверка тома только чтение", L"chkdsk {D} /v" },
        { L"Свободное место", L"fsutil volume diskfree {D}" },
        { L"Точки подключения", L"mountvol {D}\\ /l" },
        { L"Атрибуты Windows", L"attrib \"{D}\\Windows\"" },
        { L"Атрибуты System32", L"attrib \"{D}\\Windows\\System32\"" },
        { L"ACL Windows", L"icacls \"{D}\\Windows\"" },
        { L"ACL System32", L"icacls \"{D}\\Windows\\System32\"" },
        { L"ACL config", L"icacls \"{D}\\Windows\\System32\\config\"" },
        { L"Проверка kernel32", L"dir /a \"{D}\\Windows\\System32\\kernel32.dll\"" },
        { L"Проверка ntoskrnl", L"dir /a \"{D}\\Windows\\System32\\ntoskrnl.exe\"" },
        { L"Проверка winload EFI", L"dir /a \"{D}\\Windows\\System32\\winload.efi\"" },
        { L"Проверка winload BIOS", L"dir /a \"{D}\\Windows\\System32\\winload.exe\"" },
        { L"Проверка bootres", L"dir /a \"{D}\\Windows\\System32\\bootres.dll\"" },
        { L"Проверка hal", L"dir /a \"{D}\\Windows\\System32\\hal.dll\"" },
        { L"Проверка ntfs.sys", L"dir /a \"{D}\\Windows\\System32\\drivers\\ntfs.sys\"" },
        { L"Проверка disk.sys", L"dir /a \"{D}\\Windows\\System32\\drivers\\disk.sys\"" },
        { L"Проверка partmgr.sys", L"dir /a \"{D}\\Windows\\System32\\drivers\\partmgr.sys\"" },
        { L"Проверка volmgr.sys", L"dir /a \"{D}\\Windows\\System32\\drivers\\volmgr.sys\"" },
        { L"Проверка volsnap.sys", L"dir /a \"{D}\\Windows\\System32\\drivers\\volsnap.sys\"" },
        { L"Проверка stornvme.sys", L"dir /a \"{D}\\Windows\\System32\\drivers\\stornvme.sys\"" },
        { L"Проверка storahci.sys", L"dir /a \"{D}\\Windows\\System32\\drivers\\storahci.sys\"" },
        { L"Проверка classpnp.sys", L"dir /a \"{D}\\Windows\\System32\\drivers\\classpnp.sys\"" },
        { L"Проверка mountmgr.sys", L"dir /a \"{D}\\Windows\\System32\\drivers\\mountmgr.sys\"" },
        { L"SFC проверка файлов", L"sfc /verifyonly /offbootdir={D}\\ /offwindir={D}\\Windows" },
        { L"SFC проверка kernel32", L"sfc /scanfile={D}\\Windows\\System32\\kernel32.dll /offbootdir={D}\\ /offwindir={D}\\Windows" },
        { L"SFC проверка ntoskrnl", L"sfc /scanfile={D}\\Windows\\System32\\ntoskrnl.exe /offbootdir={D}\\ /offwindir={D}\\Windows" },
        { L"SFC проверка winload EFI", L"sfc /scanfile={D}\\Windows\\System32\\winload.efi /offbootdir={D}\\ /offwindir={D}\\Windows" },
        { L"SFC проверка winload BIOS", L"sfc /scanfile={D}\\Windows\\System32\\winload.exe /offbootdir={D}\\ /offwindir={D}\\Windows" },
        { L"SFC проверка bootres", L"sfc /scanfile={D}\\Windows\\System32\\bootres.dll /offbootdir={D}\\ /offwindir={D}\\Windows" },
        { L"SFC проверка hal", L"sfc /scanfile={D}\\Windows\\System32\\hal.dll /offbootdir={D}\\ /offwindir={D}\\Windows" },
        { L"DISM CheckHealth", L"dism /image:{D}\\ /cleanup-image /checkhealth" },
        { L"DISM ScanHealth", L"dism /image:{D}\\ /cleanup-image /scanhealth" },
        { L"DISM GetInfo", L"dism /image:{D}\\ /get-currentedition" },
        { L"DISM редакции Windows", L"dism /image:{D}\\ /get-targeteditions" },
        { L"DISM драйверы", L"dism /image:{D}\\ /get-drivers /all" },
        { L"DISM пакеты", L"dism /image:{D}\\ /get-packages" },
        { L"DISM функции", L"dism /image:{D}\\ /get-features /format:table" },
        { L"DISM языки", L"dism /image:{D}\\ /get-intl" },
        { L"DISM возможности", L"dism /image:{D}\\ /get-capabilities" },
        { L"DISM резервирование", L"dism /image:{D}\\ /get-reservationinfo" },
        { L"DISM pending actions", L"dism /image:{D}\\ /get-packages | findstr /i pending" },
        { L"Пакеты обслуживания", L"dir /b \"{D}\\Windows\\servicing\\Packages\\*.mum\"" },
        { L"Pending.xml", L"dir /a \"{D}\\Windows\\WinSxS\\pending.xml\"" },
        { L"CBS.log ошибки", L"findstr /i /c:\"error\" /c:\"failed\" \"{D}\\Windows\\Logs\\CBS\\CBS.log\"" },
        { L"DISM.log ошибки", L"findstr /i /c:\"error\" /c:\"failed\" \"{D}\\Windows\\Logs\\DISM\\dism.log\"" },
        { L"Setupact ошибки", L"findstr /i /c:\"error\" /c:\"failed\" \"{D}\\Windows\\Panther\\setupact.log\"" },
        { L"Setuperr.log", L"type \"{D}\\Windows\\Panther\\setuperr.log\"" },
        { L"SrtTrail.log", L"type \"{D}\\Windows\\System32\\LogFiles\\Srt\\SrtTrail.txt\"" },
        { L"Ntbtlog", L"type \"{D}\\Windows\\ntbtlog.txt\"" },
        { L"Драйверы boot-start", L"dir /b \"{D}\\Windows\\System32\\drivers\\*.sys\"" },
        { L"Драйверы подписанные", L"pnputil /enum-drivers /offline {D}\\Windows" },
        { L"Драйверы класса System", L"pnputil /enum-drivers /class System /offline {D}\\Windows" },
        { L"Драйверы класса Storage", L"pnputil /enum-drivers /class SCSIAdapter /offline {D}\\Windows" },
        { L"Драйверы класса Net", L"pnputil /enum-drivers /class Net /offline {D}\\Windows" },
        { L"Драйверы класса Display", L"pnputil /enum-drivers /class Display /offline {D}\\Windows" },
        { L"Проблемные устройства", L"pnputil /enum-devices /problem" },
        { L"Подключённые устройства", L"pnputil /enum-devices /connected" },
        { L"BitLocker статус", L"manage-bde -status {D}" },
        { L"BitLocker protectors", L"manage-bde -protectors -get {D}" },
        { L"BitLocker метаданные", L"manage-bde -info {D}" },
        { L"BCD всё", L"bcdedit /store {D}\\Boot\\BCD /enum all /v" },
        { L"BCD boot manager", L"bcdedit /store {D}\\Boot\\BCD /enum {bootmgr}" },
        { L"BCD loaders", L"bcdedit /store {D}\\Boot\\BCD /enum osloader" },
        { L"BCD firmware", L"bcdedit /enum firmware" },
        { L"EFI каталог", L"dir /a /s \"{D}\\EFI\\Microsoft\\Boot\"" },
        { L"BIOS boot каталог", L"dir /a /s \"{D}\\Boot\"" },
        { L"Recovery каталог", L"dir /a /s \"{D}\\Windows\\System32\\Recovery\"" },
        { L"ReAgent.xml", L"type \"{D}\\Windows\\System32\\Recovery\\ReAgent.xml\"" },
        { L"WinRE WIM размер", L"dir /-c \"{D}\\Windows\\System32\\Recovery\\Winre.wim\"" },
        { L"Проверка загрузочной конфигурации", L"bcdedit /store {D}\\Boot\\BCD /verify" },
        { L"Архив реестра", L"dir /a \"{D}\\Windows\\System32\\config\"" },
        { L"SYSTEM hive", L"dir /a \"{D}\\Windows\\System32\\config\\SYSTEM*\"" },
        { L"SOFTWARE hive", L"dir /a \"{D}\\Windows\\System32\\config\\SOFTWARE*\"" },
        { L"SAM hive", L"dir /a \"{D}\\Windows\\System32\\config\\SAM*\"" },
        { L"SECURITY hive", L"dir /a \"{D}\\Windows\\System32\\config\\SECURITY*\"" },
        { L"Registry logs", L"dir /a \"{D}\\Windows\\System32\\config\\*.LOG*\"" },
        { L"RegBack", L"dir /a \"{D}\\Windows\\System32\\config\\RegBack\"" },
        { L"Hosts файл", L"type \"{D}\\Windows\\System32\\drivers\\etc\\hosts\"" },
        { L"Hosts атрибуты", L"attrib \"{D}\\Windows\\System32\\drivers\\etc\\hosts\"" },
        { L"Winlogon hive path", L"reg query HKLM\\OfflineSoftware\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon" },
        { L"Windows версия файла", L"wmic datafile where name=\"{D}\\\\Windows\\\\System32\\\\ntoskrnl.exe\" get Version /value" },
        { L"Системные DLL", L"dir /b \"{D}\\Windows\\System32\\*.dll\"" },
        { L"Системные EXE", L"dir /b \"{D}\\Windows\\System32\\*.exe\"" },
        { L"Автозапуск профилей", L"dir /a /s \"{D}\\Users\\*\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\"" },
        { L"Default профиль", L"dir /a \"{D}\\Users\\Default\"" },
        { L"ProfileList файлы", L"dir /a \"{D}\\Users\"" },
        { L"Scheduled Tasks файлы", L"dir /a /s \"{D}\\Windows\\System32\\Tasks\"" },
        { L"Services registry files", L"dir /a \"{D}\\Windows\\System32\\config\\SYSTEM\"" },
        { L"AppCompat cache", L"dir /a \"{D}\\Windows\\AppCompat\"" },
        { L"Fonts каталог", L"dir /a \"{D}\\Windows\\Fonts\"" },
        { L"Windows Update cache", L"dir /a \"{D}\\Windows\\SoftwareDistribution\"" },
        { L"Catroot cache", L"dir /a \"{D}\\Windows\\System32\\catroot2\"" },
        { L"WMI repository", L"dir /a \"{D}\\Windows\\System32\\wbem\\Repository\"" },
        { L"EventLog файлы", L"dir /a \"{D}\\Windows\\System32\\winevt\\Logs\"" },
        { L"Security database", L"dir /a \"{D}\\Windows\\Security\\Database\"" },
        { L"Code Integrity logs", L"dir /a \"{D}\\Windows\\System32\\CodeIntegrity\"" },
        { L"Defender каталог", L"dir /a \"{D}\\ProgramData\\Microsoft\\Windows Defender\"" },
        { L"CrashControl файлы", L"dir /a \"{D}\\Windows\\LiveKernelReports\"" },
        { L"Temporary files", L"dir /a \"{D}\\Windows\\Temp\"" },
        { L"Installer cache", L"dir /a \"{D}\\Windows\\Installer\"" },
        { L"Component store cleanup check", L"dism /image:{D}\\ /cleanup-image /analyzecomponentstore" },
        { L"Component store health", L"dism /image:{D}\\ /cleanup-image /checkhealth" },
        { L"Component store scan", L"dism /image:{D}\\ /cleanup-image /scanhealth" },
        { L"SFC повторная проверка", L"sfc /verifyonly /offbootdir={D}\\ /offwindir={D}\\Windows" },
        { L"Проверка WindowsApps", L"dir /a \"{D}\\Program Files\\WindowsApps\"" },
        { L"Проверка ProgramData", L"dir /a \"{D}\\ProgramData\"" },
        { L"Проверка Program Files", L"dir /a \"{D}\\Program Files\"" },
        { L"Проверка SysWOW64", L"dir /a \"{D}\\Windows\\SysWOW64\"" },
        { L"Проверка WinSxS манифестов", L"dir /b \"{D}\\Windows\\WinSxS\\Manifests\"" },
        { L"Проверка лицензий", L"dir /a \"{D}\\Windows\\System32\\spp\\store\"" },
        { L"Проверка лицензирования", L"dir /a \"{D}\\Windows\\ServiceProfiles\"" },
        { L"Проверка службы профилей", L"dir /a \"{D}\\Windows\\ServiceProfiles\\NetworkService\"" },
        { L"Проверка системного профиля", L"dir /a \"{D}\\Windows\\System32\\config\\systemprofile\"" },
        { L"Проверка публичного профиля", L"dir /a \"{D}\\Users\\Public\"" },
        { L"Проверка загрузочных ресурсов", L"dir /a /s \"{D}\\Windows\\Boot\\Resources\"" },
        { L"Проверка языковых ресурсов", L"dir /a \"{D}\\Windows\\System32\\ru-RU\"" },
        { L"Проверка английских ресурсов", L"dir /a \"{D}\\Windows\\System32\\en-US\"" },
        { L"Проверка установленных шрифтов", L"dir /a \"{D}\\Windows\\Fonts\\*.ttf\"" },
        { L"Проверка системных задач XML", L"dir /b /s \"{D}\\Windows\\System32\\Tasks\\*.xml\"" },
        { L"Проверка .NET каталогов", L"dir /a \"{D}\\Windows\\Microsoft.NET\"" },
        { L"Проверка журналов обновлений", L"dir /a \"{D}\\Windows\\Logs\\WindowsUpdate\"" },
        { L"Проверка журналов Defender", L"dir /a \"{D}\\ProgramData\\Microsoft\\Windows Defender\\Support\"" },
        { L"Проверка логов драйверов", L"dir /a \"{D}\\Windows\\INF\\*.log\"" },
        { L"Проверка логов миграции", L"dir /a \"{D}\\Windows\\Panther\\Migration\"" },
        { L"Проверка резервных профилей", L"dir /a \"{D}\\Windows\\System32\\config\\TxR\"" },
        { L"Проверка Pending операции", L"dir /a \"{D}\\Windows\\WinSxS\\*.xml\"" },
        { L"Проверка корневых ACL", L"icacls \"{D}\\\"" },
        { L"Проверка Users ACL", L"icacls \"{D}\\Users\"" },
        { L"Проверка Boot ACL", L"icacls \"{D}\\Boot\"" },
        { L"Проверка EFI ACL", L"icacls \"{D}\\EFI\"" },
        { L"Проверка Recovery ACL", L"icacls \"{D}\\Windows\\System32\\Recovery\"" },
        { L"Финальный статус тома", L"fsutil volume diskfree {D}" }
    };
    return actions;
}

struct WinPeCategory {
    const wchar_t* label;
    size_t first;
    size_t last;
};

static const std::vector<WinPeCategory>& HomeGetWinPeCategories() {
    static const std::vector<WinPeCategory> categories = {
        { L"Диск и NTFS", 0, 30 },
        { L"Системные файлы", 30, 59 },
        { L"DISM и обслуживание", 59, 78 },
        { L"Драйверы и устройства", 78, 93 },
        { L"Загрузка и BCD", 93, 108 },
        { L"Реестр и профили", 108, 124 },
        { L"Журналы и диагностика", 124, 139 },
        { L"Безопасность и ACL", 139, 151 },
        { L"Финальная проверка", 151, 157 }
    };
    return categories;
}

static void HomeSelectWinPeCategory(int category) {
    const auto& categories = HomeGetWinPeCategories();
    if (category < 0 || category >= static_cast<int>(categories.size())) return;
    g_winPeCategory = category;
    g_homeButtons.clear();
    InitHomeButtons();
    HomeRedraw();
}

static void HomeRunWinPeAction(size_t actionIndex) {
    const auto& actions = HomeGetWinPeActions();
    if (actionIndex >= actions.size()) return;
    const std::wstring drive = UnlockTools::GetOfflineDriveHint();
    if (drive.empty()) {
        MessageBoxW(App::Instance()->GetHWND(),
            L"Сначала выберите установленную Windows на диске в настройках.",
            L"WinPE", MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring command = actions[actionIndex].command;
    size_t position = 0;
    while ((position = command.find(L"{D}", position)) != std::wstring::npos) {
        command.replace(position, 3, drive);
        position += drive.size();
    }
    if (!HomeRunElevatedCommand(command)) {
        MessageBoxW(App::Instance()->GetHWND(),
            L"Не удалось запустить выбранную операцию WinPE.",
            L"WinPE", MB_OK | MB_ICONERROR);
    }
}

static void HomeAutoUnlockC() {
    const auto installations = UnlockTools::ScanDrivesWithWindows();
    const auto target = std::find_if(installations.begin(), installations.end(),
        [](const std::wstring& drive) { return _wcsicmp(drive.c_str(), L"C:\\") == 0; });
    if (target == installations.end()) {
        MessageBoxW(App::Instance()->GetHWND(),
            L"Установленная Windows на диске C: не найдена.",
            L"Авторазблокировка C:", MB_OK | MB_ICONWARNING);
        return;
    }
    UnlockTools::SetOfflineDriveHint(L"C:");
    if (MessageBoxW(App::Instance()->GetHWND(),
        L"Запустить полную автоматическую разблокировку установленной Windows на диске C:?\r\n\r\n"
        L"Будут сохранены системные hive и выполнены поддерживаемые исправления.",
        L"Авторазблокировка C:", MB_YESNO | MB_ICONWARNING) == IDYES) {
        RunFullRecovery(false);
    }
}

static void HomeShowPolicyRecoveryMenu() {
    enum : UINT {
        IDM_POLICY_RESTORE_ALL = 1,
        IDM_POLICY_OPEN_GPE = 2,
        IDM_POLICY_OFFLINE_RECOVERY = 3,
        IDM_POLICY_GROUP_FIRST = 100
    };

    struct PolicyGroup {
        const wchar_t* label;
        std::vector<std::wstring> descriptions;
    };

    const std::vector<PolicyGroup> groups = {
        { L"Параметры, Панель управления и оснастки MMC", {
            L"Панель управления", L"MMC (только разрешённые оснастки)",
            L"Диспетчер задач", L"Редактор реестра", L"Командная строка (CMD)",
            L"Командная строка (CMD, старый путь)", L"Окно 'Выполнить' (Win+R)" } },
        { L"Проводник, контекстное меню и рабочий стол", {
            L"Контекстное меню", L"Доступ к дискам", L"Скрытие дисков",
            L"Поиск в Пуске", L"Параметры папок", L"Вкладка 'Безопасность'",
            L"Меню 'Файл'", L"Настройки панели задач", L"Иконки рабочего стола (NoDesktop)" } },
        { L"Персонализация и переключение пользователей", {
            L"Смена обоев", L"Персонализация (NoDispCPL)",
            L"Настройки фона (NoDispBackgroundPage)", L"Выход из системы",
            L"Блокировка рабочей станции", L"Смена пароля" } },
        { L"Windows Installer, Update и восстановление системы", {
            L"Установщик MSI (DisableMSI)", L"Доступ к Windows Update",
            L"Восстановление системы", L"Настройка восстановления системы" } },
        { L"Microsoft Defender", {
            L"Defender (DisableAntiSpyware)", L"Defender (DisableAntiVirus)",
            L"Defender (мониторинг в реальном времени)" } }
    };

    HWND owner = App::Instance()->GetHWND();
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const bool inRecovery = UnlockTools::IsRecoveryEnvironment();
    std::vector<UnlockTools::Restriction> restrictions = UnlockTools::GetKnownRestrictions();
    std::vector<PolicyGroup> enabledGroups;

    if (inRecovery) {
        AppendMenuW(menu, MF_STRING, IDM_POLICY_OFFLINE_RECOVERY,
            L"Восстановить выбранную Windows офлайн...");
    }
    else {
        for (const auto& group : groups) {
            bool active = false;
            for (const auto& restriction : restrictions) {
                if (std::find(group.descriptions.begin(), group.descriptions.end(),
                    restriction.description) != group.descriptions.end() &&
                    UnlockTools::IsRestricted(restriction)) {
                    active = true;
                    break;
                }
            }
            const UINT command = IDM_POLICY_GROUP_FIRST + static_cast<UINT>(enabledGroups.size());
            AppendMenuW(menu, MF_STRING | (active ? 0 : MF_GRAYED), command, group.label);
            enabledGroups.push_back(group);
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_POLICY_RESTORE_ALL,
            L"Восстановить все обнаруженные ограничения...");
        AppendMenuW(menu, MF_STRING, IDM_POLICY_OPEN_GPE,
            L"Открыть редактор групповой политики (gpedit.msc)");
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(owner);
    const UINT command = TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, owner, nullptr);
    DestroyMenu(menu);

    if (command == IDM_POLICY_OFFLINE_RECOVERY) {
        RunFullRecovery();
        return;
    }
    if (command == IDM_POLICY_OPEN_GPE) {
        HINSTANCE result = ShellExecuteW(owner, L"runas", L"mmc.exe",
            L"gpedit.msc", nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            MessageBoxW(owner,
                L"Редактор локальной групповой политики недоступен в этой редакции Windows.",
                L"Групповая политика", MB_OK | MB_ICONINFORMATION);
        }
        return;
    }
    if (command == IDM_POLICY_RESTORE_ALL) {
        if (MessageBoxW(owner,
            L"Снять обнаруженные поддерживаемые ограничения и исправить выявленные системные изменения? "
            L"Политики организации могут быть применены снова.",
            L"Восстановление политик", MB_YESNO | MB_ICONWARNING) != IDYES) return;
        const std::wstring report = UnlockTools::GetBestUnlockReport(true);
        MessageBoxW(owner, report.c_str(), L"Восстановление политик",
            MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (command < IDM_POLICY_GROUP_FIRST ||
        command - IDM_POLICY_GROUP_FIRST >= enabledGroups.size()) return;
    const auto& group = enabledGroups[command - IDM_POLICY_GROUP_FIRST];
    std::wstring report;
    int restored = 0;
    int failed = 0;
    for (const auto& restriction : restrictions) {
        if (std::find(group.descriptions.begin(), group.descriptions.end(),
            restriction.description) == group.descriptions.end() ||
            !UnlockTools::IsRestricted(restriction)) continue;
        if (UnlockTools::UnlockRestriction(restriction)) {
            report += L"Восстановлено: " + restriction.description + L"\r\n";
            ++restored;
        }
        else {
            report += L"Не удалось восстановить: " + restriction.description + L"\r\n";
            ++failed;
        }
    }
    if (restored == 0 && failed == 0)
        report = L"Активные ограничения из этой категории не найдены.";
    else
        report += L"\r\nУспешно: " + std::to_wstring(restored) +
            L"; ошибок: " + std::to_wstring(failed) + L".";
    MessageBoxW(owner, report.c_str(), L"Восстановление политик",
        MB_OK | (failed == 0 ? MB_ICONINFORMATION : MB_ICONWARNING));
}

static void HomeShowBsodTools() {
    enum : UINT {
        IDM_BSOD_REPAIR = 1,
        IDM_BSOD_SETTINGS,
        IDM_BSOD_DUMPS
    };

    HWND owner = App::Instance()->GetHWND();
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Универсального исправления всех BSOD нет");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_BSOD_REPAIR, L"Проверить и восстановить Windows (DISM + SFC)");
    AppendMenuW(menu, MF_STRING, IDM_BSOD_SETTINGS, L"Настройки сбоя и автоперезапуска");
    AppendMenuW(menu, MF_STRING, IDM_BSOD_DUMPS, L"Открыть дампы выбранной Windows");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(owner);
    const UINT command = TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, owner, nullptr);
    DestroyMenu(menu);

    if (command == IDM_BSOD_REPAIR) {
        if (UnlockTools::IsRecoveryEnvironment()) {
            RunFullRecovery();
        }
        else if (MessageBoxW(owner,
            L"Запустить DISM RestoreHealth и SFC /scannow? Это может занять продолжительное время. "
            L"Проверка системных файлов не устраняет аппаратные и все драйверные причины BSOD.",
            L"Проверка Windows", MB_YESNO | MB_ICONWARNING) == IDYES) {
            if (!HomeRunElevatedCommand(
                L"dism.exe /Online /Cleanup-Image /RestoreHealth & sfc.exe /scannow")) {
                MessageBoxW(owner, L"Не удалось запустить DISM и SFC.",
                    L"Проверка Windows", MB_OK | MB_ICONERROR);
            }
        }
    }
    else if (command == IDM_BSOD_SETTINGS) {
        if (UnlockTools::IsRecoveryEnvironment()) {
            MessageBoxW(owner,
                L"Настройки автоперезапуска доступны в обычной Windows, не в WinRE.",
                L"Настройки BSOD", MB_OK | MB_ICONINFORMATION);
            return;
        }
        wchar_t systemDirectory[MAX_PATH] = {};
        const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            MessageBoxW(owner, L"Не удалось определить каталог Windows System32.",
                L"Настройки BSOD", MB_OK | MB_ICONERROR);
            return;
        }
        MessageBoxW(owner,
            L"Откроются дополнительные параметры системы. В разделе «Загрузка и восстановление» "
            L"можно снять флажок «Выполнить автоматическую перезагрузку». Это оставит экран BSOD "
            L"видимым, но не предотвратит сам сбой.",
            L"Настройки BSOD", MB_OK | MB_ICONINFORMATION);
        Launch(std::wstring(systemDirectory) + L"\\SystemPropertiesAdvanced.exe", true);
    }
    else if (command == IDM_BSOD_DUMPS) {
        std::wstring windowsDirectory;
        if (UnlockTools::IsRecoveryEnvironment()) {
            const std::wstring drive = UnlockTools::GetOfflineDriveHint();
            if (drive.empty()) {
                MessageBoxW(owner, L"Сначала выберите целевой диск Windows в настройках.",
                    L"Дампы BSOD", MB_OK | MB_ICONWARNING);
                return;
            }
            windowsDirectory = drive + L"Windows";
        }
        else {
            wchar_t path[MAX_PATH] = {};
            const UINT length = GetWindowsDirectoryW(path, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) {
                MessageBoxW(owner, L"Не удалось определить каталог Windows.",
                    L"Дампы BSOD", MB_OK | MB_ICONERROR);
                return;
            }
            windowsDirectory.assign(path, length);
        }

        const std::wstring dumpDirectory = windowsDirectory + L"\\Minidump";
        const std::wstring fullDump = windowsDirectory + L"\\MEMORY.DMP";
        const DWORD dumpAttributes = GetFileAttributesW(dumpDirectory.c_str());
        const bool hasMiniDumps = dumpAttributes != INVALID_FILE_ATTRIBUTES &&
            (dumpAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!hasMiniDumps && GetFileAttributesW(fullDump.c_str()) == INVALID_FILE_ATTRIBUTES) {
            MessageBoxW(owner, L"Файлы дампов BSOD не найдены в выбранной установке Windows.",
                L"Дампы BSOD", MB_OK | MB_ICONINFORMATION);
            return;
        }
        ShellExecuteW(owner, L"open",
            hasMiniDumps ? dumpDirectory.c_str() : windowsDirectory.c_str(),
            nullptr, nullptr, SW_SHOWNORMAL);
    }
}

static void HomeEnableUacMaximum() {
    HomeRunElevatedCommand(
        L"reg.exe add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System "
        L"/v EnableLUA /t REG_DWORD /d 1 /f & "
        L"reg.exe add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System "
        L"/v ConsentPromptBehaviorAdmin /t REG_DWORD /d 2 /f & "
        L"reg.exe add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System "
        L"/v PromptOnSecureDesktop /t REG_DWORD /d 1 /f");
}

static void HomeFormatDrive(HWND owner, const std::wstring& drive) {
    if (drive.empty()) return;
    std::wstring message = L"Форматировать диск " + drive + L"? Все данные будут удалены.";
    if (MessageBoxW(owner, message.c_str(), L"Форматирование", MB_YESNO | MB_ICONWARNING) == IDYES)
        SHFormatDrive(owner, drive[0] - L'A', SHFMT_ID_DEFAULT, 0);
}

static void HomeDeleteVolume(HWND owner, const std::wstring& drive) {
    if (drive.empty()) return;
    std::wstring message = L"Удалить раздел " + drive + L"? Все данные будут потеряны.";
    if (MessageBoxW(owner, message.c_str(), L"Удаление раздела", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    HomeRunElevatedCommand(
        L"(echo select volume " + std::wstring(1, drive[0]) +
        L"& echo delete volume override)|diskpart.exe");
}

enum class HomeToolWindowKind { Drives, Associations, Cleanup };

struct HomeToolWindowState {
    HomeToolWindowKind kind;
    HWND list = nullptr;
    HWND window = nullptr;
    std::vector<std::wstring> values;
    bool done = false;
    bool result = false;
};

static bool HomeToolAssociationBroken(const std::wstring& extension) {
    HKEY extensionKey = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, extension.c_str(), 0, KEY_READ, &extensionKey) != ERROR_SUCCESS)
        return true;

    wchar_t progId[256]{};
    DWORD size = sizeof(progId);
    DWORD type = 0;
    bool valid = RegQueryValueExW(extensionKey, nullptr, nullptr, &type,
        reinterpret_cast<LPBYTE>(progId), &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && progId[0] != L'\0';
    RegCloseKey(extensionKey);
    if (!valid) return true;

    std::wstring commandPath = L"";
    commandPath += progId;
    commandPath += L"\\shell\\open\\command";
    HKEY commandKey = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, commandPath.c_str(), 0, KEY_READ, &commandKey) != ERROR_SUCCESS)
        return true;
    wchar_t command[1024]{};
    size = sizeof(command);
    valid = RegQueryValueExW(commandKey, nullptr, nullptr, &type,
        reinterpret_cast<LPBYTE>(command), &size) == ERROR_SUCCESS && command[0] != L'\0';
    RegCloseKey(commandKey);
    return !valid;
}

static std::wstring HomeToolBrowseProgram(HWND owner) {
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"Программы (*.exe)\0*.exe\0Все файлы (*.*)\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameW(&dialog) ? std::wstring(file) : std::wstring();
}

static void HomeToolSetAssociation(const std::wstring& extension, const std::wstring& programPath) {
    HKEY extKey = nullptr;
    const wchar_t* progId = L"SYSIM.NotepadFile";
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        (L"Software\\Classes\\" + extension).c_str(), 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &extKey, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(extKey, nullptr, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(progId),
        static_cast<DWORD>((wcslen(progId) + 1) * sizeof(wchar_t)));
    RegCloseKey(extKey);

    HKEY commandKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Classes\\SYSIM.NotepadFile\\shell\\open\\command", 0,
        nullptr, 0, KEY_SET_VALUE, nullptr, &commandKey, nullptr) == ERROR_SUCCESS) {
        std::wstring command = L"\\\"" + programPath + L"\\\" \\\"%1\\\"";
        RegSetValueExW(commandKey, nullptr, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(commandKey);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

static unsigned long long HomeCleanDirectory(const std::wstring& directory) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::exists(directory, error)) return 0;
    unsigned long long removed = 0;
    for (const auto& entry : fs::directory_iterator(directory,
        fs::directory_options::skip_permission_denied, error)) {
        if (error) {
            error.clear();
            continue;
        }
        std::error_code sizeError;
        if (fs::is_regular_file(entry.path(), sizeError)) {
            auto size = fs::file_size(entry.path(), sizeError);
            if (!sizeError && fs::remove(entry.path(), error) && !error) removed += size;
        }
        else if (fs::is_directory(entry.path(), sizeError)) {
            fs::remove_all(entry.path(), error);
            error.clear();
        }
    }
    return removed;
}

static void HomeRunSystemCleanup(HWND owner) {
    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    unsigned long long bytes = HomeCleanDirectory(tempPath);
    wchar_t windowsPath[MAX_PATH]{};
    GetWindowsDirectoryW(windowsPath, MAX_PATH);
    bytes += HomeCleanDirectory(std::wstring(windowsPath) + L"\\Temp");
    SHEmptyRecycleBinW(owner, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    wchar_t message[256]{};
    swprintf_s(message, L"Очистка завершена. Освобождено примерно %llu МБ.",
        bytes / (1024ULL * 1024ULL));
    MessageBoxW(owner, message, L"Очистка системы", MB_OK | MB_ICONINFORMATION);
}

static void HomeToolPopulate(HomeToolWindowState* state) {
    if (!state || !state->list) return;
    if (state->kind == HomeToolWindowKind::Drives) {
        wchar_t buffer[512]{};
        DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(buffer)), buffer);
        for (wchar_t* drive = buffer; drive < buffer + length; drive += wcslen(drive) + 1) {
            UINT type = GetDriveTypeW(drive);
            if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
                state->values.emplace_back(drive);
                SendMessageW(state->list, LB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(drive));
            }
        }
    }
    else if (state->kind == HomeToolWindowKind::Cleanup) {
        const wchar_t* cleanupItems[] = {
            L"Временные файлы пользователя",
            L"Windows\\Temp",
            L"Корзина"
        };
        for (const wchar_t* item : cleanupItems) {
            state->values.emplace_back(item);
            SendMessageW(state->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        }
    }
    else {
        const wchar_t* extensions[] = {
            L".txt", L".log", L".ini", L".cfg", L".csv", L".xml", L".json",
            L".md", L".bat", L".cmd", L".ps1", L".reg", L".html", L".css",
            L".cpp", L".h", L".c", L".py", L".rtf"
        };
        for (const wchar_t* extension : extensions) {
            state->values.emplace_back(extension);
            std::wstring display = extension;
            if (HomeToolAssociationBroken(extension)) display += L"  [сломано]";
            SendMessageW(state->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(extension));
            if (HomeToolAssociationBroken(extension)) {
                int index = static_cast<int>(SendMessageW(state->list, LB_GETCOUNT, 0, 0)) - 1;
                SendMessageW(state->list, LB_DELETESTRING, index, 0);
                SendMessageW(state->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
            }
        }
    }
}

static void HomeToolRestoreAllAssociations() {
    const wchar_t* extensions[] = {
        L".txt", L".log", L".ini", L".cfg", L".csv", L".xml", L".json",
        L".md", L".bat", L".cmd", L".ps1", L".reg", L".html", L".css",
        L".cpp", L".h", L".c", L".py", L".rtf"
    };
    for (const wchar_t* extension : extensions)
        HomeToolSetAssociation(extension, L"C:\\Windows\\notepad.exe");
}

static LRESULT CALLBACK HomeToolWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<HomeToolWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<HomeToolWindowState*>(create->lpCreateParams);
        state->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(hwnd, message, wParam, lParam);

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC hdc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH background = CreateSolidBrush(RGB(18, 18, 18));
        FillRect(hdc, &client, background);
        DeleteObject(background);
        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(210, 210, 210));
        HGDIOBJ oldPen = SelectObject(hdc, borderPen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, 2, 2, client.right - 3, client.bottom - 3);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(borderPen);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(245, 245, 245));
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));
        const wchar_t* title = state->kind == HomeToolWindowKind::Drives
            ? L"Удаление дисков"
            : (state->kind == HomeToolWindowKind::Cleanup ? L"Очистка системы" : L"Ассоциации файлов");
        RECT titleRect{ 16, 8, client.right - 48, 34 };
        DrawTextW(hdc, title, -1, &titleRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        RECT closeRect{ client.right - 40, 2, client.right - 8, 34 };
        DrawTextW(hdc, L"X", -1, &closeRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        SetTextColor(hdc, RGB(190, 190, 190));
        RECT hintRect{ 16, 38, client.right - 16, 62 };
        const wchar_t* hint = state->kind == HomeToolWindowKind::Drives
            ? L"Выберите носитель для операции"
            : (state->kind == HomeToolWindowKind::Cleanup
                ? L"Временные файлы и корзина"
                : L"Выберите расширение и назначьте программу открытия");
        DrawTextW(hdc, hint, -1, &hintRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        SelectObject(hdc, oldFont);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT windowRect{};
        GetWindowRect(hwnd, &windowRect);
        if (point.y < windowRect.top + 38 && point.x < windowRect.right - 44) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_LBUTTONUP: {
        RECT client{};
        GetClientRect(hwnd, &client);
        if (GET_Y_LPARAM(lParam) < 38 && GET_X_LPARAM(lParam) > client.right - 44) {
            state->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, RGB(18, 18, 18));
        SetTextColor(hdc, RGB(245, 245, 245));
        static HBRUSH brush = CreateSolidBrush(RGB(18, 18, 18));
        return reinterpret_cast<LRESULT>(brush);
    }
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        measure->itemHeight = 28;
        return TRUE;
    }
    case WM_CONTEXTMENU:
        if (state->kind == HomeToolWindowKind::Drives &&
            reinterpret_cast<HWND>(wParam) == state->list) {
            int selected = static_cast<int>(SendMessageW(state->list, LB_GETCURSEL, 0, 0));
            if (selected < 0 || selected >= static_cast<int>(state->values.size())) return 0;
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 4, L"Форматировать");
            AppendMenuW(menu, MF_STRING, 5, L"Удалить раздел");
            POINT point{};
            GetCursorPos(&point);
            int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                point.x, point.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (command == 4) HomeFormatDrive(hwnd, state->values[selected]);
            else if (command == 5) HomeDeleteVolume(hwnd, state->values[selected]);
            return 0;
        }
        break;
    case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (draw->CtlType == ODT_LISTBOX) {
            wchar_t text[128]{};
            SendMessageW(draw->hwndItem, LB_GETTEXT, draw->itemID, reinterpret_cast<LPARAM>(text));
            bool selected = (draw->itemState & ODS_SELECTED) != 0;
            HBRUSH brush = CreateSolidBrush(selected ? RGB(0, 100, 180) : RGB(18, 18, 18));
            FillRect(draw->hDC, &draw->rcItem, brush);
            DeleteObject(brush);
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, RGB(235, 235, 235));
            RECT textRect = draw->rcItem;
            textRect.left += 10;
            DrawTextW(draw->hDC, text, -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            return TRUE;
        }
        if (draw->CtlType == ODT_BUTTON) {
            wchar_t text[128]{};
            GetWindowTextW(draw->hwndItem, text, static_cast<int>(std::size(text)));
            HBRUSH brush = CreateSolidBrush(RGB(225, 225, 225));
            FillRect(draw->hDC, &draw->rcItem, brush);
            DeleteObject(brush);
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, RGB(20, 20, 20));
            DrawTextW(draw->hDC, text, -1, &draw->rcItem, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            return TRUE;
        }
        break;
    }
    case WM_CREATE: {
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        const wchar_t* action = state->kind == HomeToolWindowKind::Drives
            ? L"Форматировать"
            : (state->kind == HomeToolWindowKind::Cleanup ? L"Очистить" : L"Выбрать программу");
        state->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
            16, 70, 468, 280, hwnd, reinterpret_cast<HMENU>(100), nullptr, nullptr);
        HWND actionButton = CreateWindowExW(0, L"BUTTON", action,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            16, 366, 220, 34, hwnd, reinterpret_cast<HMENU>(1), nullptr, nullptr);
        HWND restoreButton = nullptr;
        if (state->kind == HomeToolWindowKind::Associations) {
            restoreButton = CreateWindowExW(0, L"BUTTON", L"Восстановить все",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                250, 366, 234, 34, hwnd, reinterpret_cast<HMENU>(3), nullptr, nullptr);
        }
        SendMessageW(actionButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (restoreButton) SendMessageW(restoreButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(state->list, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        HomeToolPopulate(state);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 1 && HIWORD(wParam) == BN_CLICKED) {
            int selected = static_cast<int>(SendMessageW(state->list, LB_GETCURSEL, 0, 0));
            if (selected == LB_ERR) return 0;
            std::wstring value = selected < static_cast<int>(state->values.size())
                ? state->values[selected] : std::wstring();
            if (state->kind == HomeToolWindowKind::Drives) {
                HomeFormatDrive(hwnd, value);
            }
            else if (state->kind == HomeToolWindowKind::Cleanup) {
                HomeRunSystemCleanup(hwnd);
            }
            else {
                std::wstring programPath = HomeToolBrowseProgram(hwnd);
                if (!programPath.empty()) {
                    HomeToolSetAssociation(value, programPath);
                    MessageBoxW(hwnd, L"Ассоциация сохранена для текущего пользователя.",
                        L"Ассоциации файлов", MB_OK | MB_ICONINFORMATION);
                }
            }
            return 0;
        }
        if (LOWORD(wParam) == 4 && HIWORD(wParam) == BN_CLICKED) {
            int selected = static_cast<int>(SendMessageW(state->list, LB_GETCURSEL, 0, 0));
            if (selected >= 0 && selected < static_cast<int>(state->values.size()))
                HomeFormatDrive(hwnd, state->values[selected]);
            return 0;
        }
        if (LOWORD(wParam) == 5 && HIWORD(wParam) == BN_CLICKED) {
            int selected = static_cast<int>(SendMessageW(state->list, LB_GETCURSEL, 0, 0));
            if (selected >= 0 && selected < static_cast<int>(state->values.size()))
                HomeDeleteVolume(hwnd, state->values[selected]);
            return 0;
        }
        if (LOWORD(wParam) == 3 && HIWORD(wParam) == BN_CLICKED) {
            if (MessageBoxW(hwnd,
                L"Восстановить ассоциации поддерживаемых типов через Блокнот?",
                L"Ассоциации файлов", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                HomeToolRestoreAllAssociations();
                SendMessageW(state->list, LB_RESETCONTENT, 0, 0);
                state->values.clear();
                HomeToolPopulate(state);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static void HomeShowToolWindow(HomeToolWindowKind kind) {
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = HomeToolWindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = L"SYSIM_HomeToolWindow";
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        if (RegisterClassW(&windowClass) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
            registered = true;
    }
    if (!registered) return;

    HomeToolWindowState state{};
    state.kind = kind;
    HWND parent = App::Instance()->GetHWND();
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"SYSIM_HomeToolWindow", L"SYSIM", WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 440, parent, nullptr, instance, &state);
    if (!window) return;
    EnableWindow(parent, FALSE);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    while (!state.done && IsWindow(window)) {
        MSG message{};
        if (GetMessageW(&message, nullptr, 0, 0) <= 0) break;
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}

static void HomeShowHelp() {
    MessageBoxW(App::Instance()->GetHWND(),
        L"Главная страница\n\n"
        L"• Кнопки запускают системные инструменты",
        L"Справка", MB_OK | MB_ICONINFORMATION);
}

// Run-строка
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

// Инициализация кнопок
void InitHomeButtons() {
    g_homeButtons.clear();

    if (UnlockTools::IsRecoveryEnvironment()) {
        g_homeButtons.push_back({ RectF(), L"Авторазблокировка диска C:" });
        g_homeButtons.push_back({ RectF(), L"Сканировать блокировки" });
        g_homeButtons.push_back({ RectF(), L"Полная диагностика WinRE (140 проверок)" });
        g_homeButtons.push_back({ RectF(), L"Сканировать IFEO-дебаггеры" });
        g_homeButtons.push_back({ RectF(), L"Удалить IFEO-дебаггеры" });
        g_homeButtons.push_back({ RectF(), L"Разблокировать выбранную Windows" });
        g_homeButtons.push_back({ RectF(), L"Восстановить Windows (DISM/SFC)" });
        g_homeButtons.push_back({ RectF(), L"Восстановить файлы входа" });
        g_homeButtons.push_back({ RectF(), L"Перезагрузка" });
        g_homeButtons.push_back({ RectF(), L"Выключить" });
        const auto& categories = HomeGetWinPeCategories();
        for (size_t categoryIndex = 0; categoryIndex < categories.size(); ++categoryIndex) {
            g_homeButtons.push_back({ RectF(), std::wstring(L"Раздел WinPE: ") +
                categories[categoryIndex].label });
        }
        const auto& category = categories[g_winPeCategory];
        const auto& actions = HomeGetWinPeActions();
        const size_t last = (std::min)(category.last, actions.size());
        for (size_t actionIndex = category.first; actionIndex < last; ++actionIndex) {
            const auto& action = actions[actionIndex];
            g_homeButtons.push_back({ RectF(), std::wstring(L"WinPE · ") + action.label });
        }
        return;
    }

    g_homeButtons.push_back({ RectF(), L"Выйти из пользователя" });
    g_homeButtons.push_back({ RectF(), L"Войти в WinRE" });
    g_homeButtons.push_back({ RectF(), L"Вернуть русский язык" });
    g_homeButtons.push_back({ RectF(), L"Починить шрифты" });
    g_homeButtons.push_back({ RectF(), L"Вернуть стандартную тему" });
    g_homeButtons.push_back({ RectF(), L"Включить UAC" });
    g_homeButtons.push_back({ RectF(), L"sfc /scannow" });
    g_homeButtons.push_back({ RectF(), L"BAT_SAVER" });
    g_homeButtons.push_back({ RectF(), L"Перезапуск в Safe Mode" });
    g_homeButtons.push_back({ RectF(), L"Анти дебаггер файлов" });
    g_homeButtons.push_back({ RectF(), L"Восстановить LogonUI" });
    g_homeButtons.push_back({ RectF(), L"Подмена utilman/sethc" });
    g_homeButtons.push_back({ RectF(), L"Восстановить utilman/sethc" });
    g_homeButtons.push_back({ RectF(), L"Выключить тестовый режим" });
    g_homeButtons.push_back({ RectF(), L"Вернуть русскую раскладку" });
    g_homeButtons.push_back({ RectF(), L"Вернуть англ раскладку" });
    g_homeButtons.push_back({ RectF(), L"Разблокировка дисков" });
    g_homeButtons.push_back({ RectF(), L"Мониторинг Shell и Userinit" });
    g_homeButtons.push_back({ RectF(), L"Полный доступ к файлу" });
    g_homeButtons.push_back({ RectF(), L"Экстренная разблокировка" });
    g_homeButtons.push_back({ RectF(), L"Очистка драйверов" });
    g_homeButtons.push_back({ RectF(), L"Очистка временных файлов" });
    g_homeButtons.push_back({ RectF(), L"Добавить SYSIM в автозагрузку" });
    g_homeButtons.push_back({ RectF(), L"Отложенный запуск" });
    g_homeButtons.push_back({ RectF(), L"Разблокировка клавиатуры" });
    g_homeButtons.push_back({ RectF(), L"Разблокировать мышь" });
    g_homeButtons.push_back({ RectF(), L"WinRAR installer" });
    g_homeButtons.push_back({ RectF(), L"Создать файл болванку" });
    g_homeButtons.push_back({ RectF(), L"Блокировка USB-портов" });
    g_homeButtons.push_back({ RectF(), L"Разблокировка USB-портов" });
    g_homeButtons.push_back({ RectF(), L"Восстановление загрузчика Win" });
    g_homeButtons.push_back({ RectF(), L"Очистить реестр" });
    g_homeButtons.push_back({ RectF(), L"Удобный запуск" });

    g_homeButtons.push_back({ RectF(), L"mbrRE" });
    g_homeButtons.push_back({ RectF(), L"Управление пользователями" });
    g_homeButtons.push_back({ RectF(), L"Управление ассоциациями" });
    g_homeButtons.push_back({ RectF(), L"Сброс пароля" });
    g_homeButtons.push_back({ RectF(), L"Удаление дисков" });
    g_homeButtons.push_back({ RectF(), L"Менеджер WinRE" });
    g_homeButtons.push_back({ RectF(), L"Восстановление WinRE" });
    g_homeButtons.push_back({ RectF(), L"Восстановить политики Windows" });
    g_homeButtons.push_back({ RectF(), L"Инструменты BSOD" });
    g_homeButtons.push_back({ RectF(), L"Авто-замена cmdline + Shell" });
}

// Отрисовка
void DrawHomeContent(Graphics& g, const RectF& contentArea, Font& contentFont) {
    (void)contentFont;

    if (g_homeButtons.empty()) InitHomeButtons();

    const float btnW = 160.0f;
    const float btnH = 34.0f;
    const float gap = 6.0f;
    const float leftMargin = 10.0f;
    const float topMargin = 8.0f;
    const float bottomMargin = 8.0f;

    FontFamily ff(g_fontFamilyName.c_str());
    Font buttonFont(&ff, 11.0f, FontStyleRegular, UnitPixel);

    SolidBrush textBrush(COLOR_TEXT);
    SolidBrush mutedBrush(COLOR_TEXT_MUTED);
    SolidBrush buttonBg(COLOR_BUTTON_BG);
    Pen borderPen(COLOR_BORDER, 1.0f);

    // Область для кнопок (сверху)
    float listTop = contentArea.Y + topMargin;
    float listBottom = contentArea.Y + contentArea.Height - bottomMargin;
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

}

// Клики
bool OnHomeClick(int x, int y, const RectF& contentArea) {
    (void)contentArea;
    if (g_homeButtons.empty()) InitHomeButtons();

    float fx = (float)x, fy = (float)y;

    // Кнопки
    for (size_t i = 0; i < g_homeButtons.size(); ++i) {
        if (!HomeHitRect(g_homeButtons[i].rect, fx, fy)) continue;
        const std::wstring& t = g_homeButtons[i].text;

        if (UnlockTools::IsRecoveryEnvironment()) {
            if (t == L"Параметры выбора Windows") { HomeOpenProgramSettings(); return true; }
            if (t == L"Авторазблокировка диска C:") { HomeAutoUnlockC(); return true; }
            const std::wstring categoryPrefix = L"Раздел WinPE: ";
            if (t.compare(0, categoryPrefix.size(), categoryPrefix) == 0) {
                const auto& categories = HomeGetWinPeCategories();
                const std::wstring categoryLabel = t.substr(categoryPrefix.size());
                for (size_t categoryIndex = 0; categoryIndex < categories.size(); ++categoryIndex) {
                    if (categoryLabel == categories[categoryIndex].label) {
                        HomeSelectWinPeCategory(static_cast<int>(categoryIndex));
                        return true;
                    }
                }
            }
            const auto& winPeActions = HomeGetWinPeActions();
            const std::wstring winPePrefix = L"WinPE · ";
            if (t.compare(0, winPePrefix.size(), winPePrefix) == 0) {
                const std::wstring actionLabel = t.substr(winPePrefix.size());
                for (size_t actionIndex = 0; actionIndex < winPeActions.size(); ++actionIndex) {
                    if (actionLabel == winPeActions[actionIndex].label) {
                        HomeRunWinPeAction(actionIndex);
                        return true;
                    }
                }
            }
            if (t == L"Сканировать блокировки") { RunWinPeScan(); return true; }
            if (t == L"Полная диагностика WinRE (140 проверок)") { RunWinPeFullDiagnosis(); return true; }
            if (t == L"Сканировать IFEO-дебаггеры") { HomeShowIfeoDebuggers(false); return true; }
            if (t == L"Удалить IFEO-дебаггеры") { HomeShowIfeoDebuggers(true); return true; }
            if (t == L"Разблокировать выбранную Windows") { RunWinPeUnlockPolicies(); return true; }
            if (t == L"Восстановить Windows (DISM/SFC)") { RunWinPeFullRepair(); return true; }
            if (t == L"Восстановить файлы входа") { RunWinPeLogonFilesRepair(); return true; }
            if (t == L"Экстренная разблокировка") { RunFullRecovery(); return true; }
            if (t == L"Восстановить LogonUI" || t == L"Разблокировка дисков" ||
                t == L"Очистить реестр" || t == L"Разблокировка клавиатуры" ||
                t == L"Разблокировать мышь") {
                HomeRunUnlockAll();
                return true;
            }
            if (t == L"Менеджер WinRE") { HomeShowWinREManager(); return true; }
            if (t == L"Восстановление WinRE") { ShowReplaceWinREDialog(App::Instance()->GetHWND()); return true; }
            if (t == L"Восстановить политики Windows") { HomeShowPolicyRecoveryMenu(); return true; }
            if (t == L"Инструменты BSOD") { HomeShowBsodTools(); return true; }
            if (t == L"Разблокировка") { HomeOpenUnlock(); return true; }
            if (t == L"Настройки программы") { HomeOpenProgramSettings(); return true; }
            if (t == L"Перезагрузка") { HomeRebootComputer(); return true; }
            if (t == L"Выключить") { HomeShutdownComputer(); return true; }

            const std::wstring selectedDrive = UnlockTools::GetOfflineDriveHint();
            const std::wstring target = selectedDrive.empty()
                ? L"установленная Windows не выбрана"
                : L"цель: " + selectedDrive + L"Windows";
            const std::wstring message = L"Команда «" + t + L"» не запускается из WinRE: " +
                L"иначе она работала бы в текущей среде X:, а не на целевой Windows (" + target +
                L"). Используйте функции WINPE-RE или задайте диск в настройках.";
            MessageBoxW(App::Instance()->GetHWND(), message.c_str(),
                L"WINPE-RE · команда заблокирована", MB_OK | MB_ICONWARNING);
            return true;
        }

        if (t == L"Выйти из пользователя") { HomeLogOff(); return true; }
        if (t == L"Войти в WinRE") { RunCommand(L"shutdown.exe /r /o /t 0"); return true; }
        if (t == L"Вернуть русский язык") { RunCommand(L"control /name Microsoft.Language"); return true; }
        if (t == L"Вернуть русскую раскладку") { RunCommand(L"control intl.cpl,, /f:\"%windir%\\System32\\ru-RU\\input.xml\""); return true; }
        if (t == L"Вернуть англ раскладку") { RunCommand(L"control intl.cpl"); return true; }
        if (t == L"Починить шрифты") { RunCommand(L"sfc.exe /scannow"); return true; }
        if (t == L"Вернуть стандартную тему") { ApplyDefaultSettings(); return true; }
        if (t == L"Включить UAC") { HomeEnableUacMaximum(); return true; }
        if (t == L"sfc /scannow") { HomeRunElevatedCommand(L"sfc.exe /scannow"); return true; }
        if (t == L"BAT_SAVER") { RunCommand(L"powercfg.exe /setactive SCHEME_BALANCED"); return true; }
        if (t == L"Перезапуск в Safe Mode") { HomeRunElevatedCommand(L"bcdedit.exe /set {current} safeboot minimal"); return true; }
        if (t == L"Анти дебаггер файлов") {
            UnlockTools::ClearImageFileExecutionOptions();
            MessageBoxW(App::Instance()->GetHWND(), L"IFEO Debugger очищен.", L"SYSIM", MB_OK | MB_ICONINFORMATION);
            return true;
        }
        if (t == L"Восстановить LogonUI") { HomeRunUnlockAll(); return true; }
        if (t == L"Подмена utilman/sethc") { HomeInstallUtilmanBackdoor(); return true; }
        if (t == L"Восстановить utilman/sethc") { HomeRunUtilmanRepair(); return true; }
        if (t == L"Экстренная разблокировка") { RunFullRecovery(); return true; }
        if (t == L"Выключить тестовый режим") { RunCommand(L"bcdedit.exe /set testsigning off"); return true; }
        if (t == L"Полный доступ к файлу") {
            wchar_t file[MAX_PATH]{};
            OPENFILENAMEW dialog{};
            dialog.lStructSize = sizeof(dialog);
            dialog.hwndOwner = App::Instance()->GetHWND();
            dialog.lpstrFilter = L"Все файлы (*.*)\0*.*\0";
            dialog.lpstrFile = file;
            dialog.nMaxFile = MAX_PATH;
            dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&dialog))
                RunCommand(L"icacls \"" + std::wstring(file) + L"\" /grant *S-1-1-0:F");
            return true;
        }
        if (t == L"Очистка драйверов") { RunCommand(L"cleanmgr.exe /sageset:65535"); return true; }
        if (t == L"Удаление дисков") { HomeShowToolWindow(HomeToolWindowKind::Drives); return true; }
        if (t == L"Управление ассоциациями") { HomeShowToolWindow(HomeToolWindowKind::Associations); return true; }
        if (t == L"Разблокировка дисков") { HomeRunUnlockAll(); return true; }
        if (t == L"Мониторинг Shell и Userinit") { HomeCheckShellAndUserinit(); return true; }
        if (t == L"Очистка временных файлов") { HomeShowToolWindow(HomeToolWindowKind::Cleanup); return true; }
        if (t == L"Добавить SYSIM в автозагрузку") { HomeAddAppToAllStartupLocations(); return true; }
        if (t == L"Авто-замена cmdline + Shell") { HomeRestoreShellSetupAndSetAppStart(); return true; }
        if (t == L"Отложенный запуск") { RunCommand(L"msconfig.exe"); return true; }
        if (t == L"Разблокировка клавиатуры" || t == L"Разблокировать мышь") { HomeRunUnlockAll(); return true; }
        if (t == L"WinRAR installer") { OpenWinRARInstaller(); return true; }
        if (t == L"Создать файл болванку") { RunCommand(L"fsutil file createnew %TEMP%\\SYSIM_blank.bin 0"); return true; }
        if (t == L"Блокировка USB-портов") { HomeRunElevatedCommand(L"reg.exe add HKLM\\SYSTEM\\CurrentControlSet\\Services\\USBSTOR /v Start /t REG_DWORD /d 4 /f"); return true; }
        if (t == L"Разблокировка USB-портов") { HomeRunElevatedCommand(L"reg.exe add HKLM\\SYSTEM\\CurrentControlSet\\Services\\USBSTOR /v Start /t REG_DWORD /d 3 /f"); return true; }
        if (t == L"Восстановление загрузчика Win") { HomeRunElevatedCommand(L"bootrec.exe /fixmbr & bootrec.exe /fixboot & bootrec.exe /rebuildbcd"); return true; }
        if (t == L"Очистить реестр") { HomeRunUnlockAll(); return true; }
        if (t == L"Управление пользователями" || t == L"Сброс пароля") {
            g_activeMainTab = 6;
            HomeRedraw();
            return true;
        }
        if (t == L"Удобный запуск") { RunCommand(L"msconfig.exe"); return true; }
        if (t == L"mbrRE") { RunCommand(L"bcdedit.exe /enum all"); return true; }
        if (t == L"Менеджер WinRE") { HomeShowWinREManager(); return true; }
        if (t == L"Восстановление WinRE") { ShowReplaceWinREDialog(App::Instance()->GetHWND()); return true; }
        if (t == L"Восстановить политики Windows") { HomeShowPolicyRecoveryMenu(); return true; }
        if (t == L"Инструменты BSOD") { HomeShowBsodTools(); return true; }

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

        if (t == L"Экстренная разблокировка") { RunFullRecovery(); return true; }
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