#include "winre_manager.h"
#include "core/globals.h"
#include <string>
#include <vector>
#include <gdiplus.h>
#include <winhttp.h>
#include <winternl.h>
#include <windowsx.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace {
    constexpr DWORD kMinimumWinRESize = 32u * 1024u * 1024u;
    constexpr DWORD kMaximumWinRESize = 2u * 1024u * 1024u * 1024u;

bool IsRunAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&authority, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }

    return isAdmin != FALSE;
}

bool IsRunningInWinRE() {
    wchar_t windowsDirectory[MAX_PATH] = {};
    if (GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory)) &&
        _wcsicmp(windowsDirectory, L"X:\\Windows") == 0) {
        return true;
    }

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\MiniNT", 0, KEY_READ,
            &key) == ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
    }

    return false;
}

bool IsRegularFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

bool IsSafeDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool GetSystemExecutable(const wchar_t* fileName, std::wstring& path) {
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDirectory, _countof(systemDirectory));
    if (length == 0 || length >= _countof(systemDirectory)) {
        return false;
    }

    path.assign(systemDirectory, length);
    path += L"\\";
    path += fileName;
    return IsRegularFile(path);
}

std::wstring QuoteArgument(const std::wstring& value) {
    if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return value;
    }

    std::wstring quoted = L"\"";
    size_t backslashCount = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashCount;
            continue;
        }

        if (character == L'\"') {
            quoted.append(backslashCount * 2 + 1, L'\\');
            quoted += L'\"';
        }
        else {
            quoted.append(backslashCount, L'\\');
            quoted += character;
        }
        backslashCount = 0;
    }

    quoted.append(backslashCount * 2, L'\\');
    quoted += L'\"';
    return quoted;
}

bool RunProgramAndWait(const std::wstring& executable,
    const std::vector<std::wstring>& arguments, DWORD& exitCode) {
    std::wstring commandLine = QuoteArgument(executable);
    for (const std::wstring& argument : arguments) {
        commandLine += L" ";
        commandLine += QuoteArgument(argument);
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, // &commandLine[0]
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo)) {
        return false;
    }

    const DWORD waitStatus = WaitForSingleObject(processInfo.hProcess, INFINITE);
    const BOOL gotExitCode = waitStatus == WAIT_OBJECT_0 &&
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return gotExitCode != FALSE;
}

bool RunReagentc(const std::vector<std::wstring>& arguments, DWORD& exitCode) {
    std::wstring reagentcPath;
    return GetSystemExecutable(L"reagentc.exe", reagentcPath) &&
        RunProgramAndWait(reagentcPath, arguments, exitCode);
}

bool RunDism(const std::vector<std::wstring>& arguments, DWORD& exitCode) {
    std::wstring dismPath;
    return GetSystemExecutable(L"dism.exe", dismPath) &&
        RunProgramAndWait(dismPath, arguments, exitCode);
}

bool CreateWorkingDirectory(std::wstring& directory) {
    wchar_t temporaryDirectory[MAX_PATH] = {};
    const DWORD length = GetTempPathW(_countof(temporaryDirectory), temporaryDirectory);
    if (length == 0 || length >= _countof(temporaryDirectory)) {
        return false;
    }

    wchar_t temporaryName[MAX_PATH] = {};
    if (!GetTempFileNameW(temporaryDirectory, L"SIM", 0, temporaryName)) {
        return false;
    }

    if (!DeleteFileW(temporaryName) || !CreateDirectoryW(temporaryName, nullptr)) {
        return false;
    }

    directory = temporaryName;
    return true;
}

void RemoveFileIfPresent(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return;
    }

    if (attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    DeleteFileW(path.c_str());
}

bool RemoveDirectoryRecursively(const std::wstring& directory) {
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }

    std::wstring searchPath = directory;
    if (!searchPath.empty() && searchPath.back() != L'\\') {
        searchPath += L'\\';
    }
    searchPath += L'*';

    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW(searchPath.c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
        bool success = true;
        do {
            if (wcscmp(entry.cFileName, L".") == 0 ||
                wcscmp(entry.cFileName, L"..") == 0) {
                continue;
            }

            const std::wstring child = directory + L"\\" + entry.cFileName;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if ((entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    if (!RemoveDirectoryW(child.c_str())) success = false;
                }
                else if (!RemoveDirectoryRecursively(child)) {
                    success = false;
                }
            }
            else {
                RemoveFileIfPresent(child);
                if (GetFileAttributesW(child.c_str()) != INVALID_FILE_ATTRIBUTES)
                    success = false;
            }
        } while (FindNextFileW(search, &entry));

        const DWORD findError = GetLastError();
        FindClose(search);
        if (!success || findError != ERROR_NO_MORE_FILES) {
            return false;
        }
    }
    else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        return false;
    }

    return RemoveDirectoryW(directory.c_str()) != FALSE;
}

bool HasWimHeader(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    char header[8] = {};
    DWORD read = 0;
    const bool valid = ReadFile(file, header, sizeof(header), &read, nullptr) &&
        read == sizeof(header) &&
        header[0] == 'M' && header[1] == 'S' && header[2] == 'W' &&
        header[3] == 'I' && header[4] == 'M';
    CloseHandle(file);
    return valid;
}

bool IsPlausibleWinRESize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }

    ULARGE_INTEGER size{};
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;
    return size.QuadPart >= kMinimumWinRESize && size.QuadPart <= kMaximumWinRESize;
}

bool IsValidWinRECandidate(const std::wstring& path) {
    return IsRegularFile(path) && HasWimHeader(path) && IsPlausibleWinRESize(path);
}

bool DownloadWinRE(const std::wstring& url, const std::wstring& outPath) {
    if (url.empty() || url.size() > MAXDWORD) {
        return false;
    }

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS ||
        components.dwHostNameLength == 0 || components.dwUrlPathLength == 0) {
        return false;
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring objectName(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.lpszExtraInfo && components.dwExtraInfoLength != 0) {
        objectName.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }

    HINTERNET session = WinHttpOpen(L"SYSIM/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return false;
    }
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);

    HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connection, L"GET", objectName.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }
    /*
    DWORD securityFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
        SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
        SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
    WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags)); */

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    const bool configured = WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY,
        &redirectPolicy, sizeof(redirectPolicy)) != FALSE;
    const bool requested = configured && WinHttpSendRequest(request,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr);

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    const bool successfulStatus = requested && WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize,
        WINHTTP_NO_HEADER_INDEX) && statusCode >= 200 && statusCode < 300;
    if (!successfulStatus) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD contentLength = 0;
    DWORD contentLengthSize = sizeof(contentLength);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &contentLengthSize,
            WINHTTP_NO_HEADER_INDEX) && contentLength > kMaximumWinRESize) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    HANDLE file = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    std::vector<BYTE> buffer(64 * 1024);
    ULONGLONG totalBytes = 0;
    bool downloaded = true;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            downloaded = false;
            break;
        }
        if (available == 0) {
            break;
        }

        while (available != 0) {
            const DWORD toRead = available < buffer.size()
                ? available : static_cast<DWORD>(buffer.size());
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), toRead, &read) || read == 0) {
                downloaded = false;
                break;
            }

            totalBytes += read;
            if (totalBytes > kMaximumWinRESize) {
                downloaded = false;
                break;
            }

            DWORD written = 0;
            if (!WriteFile(file, buffer.data(), read, &written, nullptr) || written != read) {
                downloaded = false;
                break;
            }
            available -= read;
        }

        if (!downloaded) {
            break;
        }
    }

    CloseHandle(file);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (!downloaded) {
        RemoveFileIfPresent(outPath);
    }
    return downloaded;
}

void HealMountedWinRE(const std::wstring& mountDir) {
    const std::wstring iniPath = mountDir + L"\\Windows\\System32\\winpeshl.ini";
    if (IsRegularFile(iniPath)) {
        const std::wstring backupPath = iniPath + L".infected";
        CopyFileW(iniPath.c_str(), backupPath.c_str(), FALSE);
        RemoveFileIfPresent(iniPath);
    }

    RemoveDirectoryRecursively(mountDir + L"\\Windows\\Tools");
    RemoveDirectoryRecursively(mountDir + L"\\Tools");
    RemoveDirectoryRecursively(mountDir + L"\\Scripts");

    RemoveFileIfPresent(mountDir + L"\\autorun.inf");
    RemoveFileIfPresent(mountDir + L"\\Windows\\System32\\winpeshl.exe.bak");
}

bool MountImage(const std::wstring& wimPath, const std::wstring& mountDir) {
    if (!CreateDirectoryW(mountDir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }

    DWORD exitCode = 0;
    RunDism({ L"/Cleanup-Wim" }, exitCode);

    return RunDism({
        L"/Mount-Wim",
        L"/WimFile:" + wimPath,
        L"/Index:1",
        L"/MountDir:" + mountDir
        }, exitCode) && exitCode == 0;
}

bool UnmountImage(const std::wstring& mountDir, bool commit) {
    DWORD exitCode = 0;
    return RunDism({
        L"/Unmount-Wim",
        L"/MountDir:" + mountDir,
        commit ? L"/Commit" : L"/Discard"
        }, exitCode) && exitCode == 0;
}

bool GetRecoveryDirectory(std::wstring& directory) {
    wchar_t windowsDirectory[MAX_PATH] = {};
    const UINT length = GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory));
    if (length == 0 || length >= _countof(windowsDirectory)) {
        return false;
    }

    directory.assign(windowsDirectory, length);
    directory += L"\\System32\\Recovery";
    return IsSafeDirectory(directory);
}

bool PrepareHealedAndInjectedWinRE(const std::wstring& workDir,
    const std::wstring& outPath,
    const std::wstring& exeSourcePath,
    const std::wstring& exeNameInImage) {
    std::wstring recoveryDirectory;
    if (!GetRecoveryDirectory(recoveryDirectory)) {
        return false;
    }

    const std::wstring sourcePath = recoveryDirectory + L"\\Winre.wim";
    if (!IsRegularFile(sourcePath)) {
        return false;
    }

    if (!CopyFileW(sourcePath.c_str(), outPath.c_str(), TRUE)) {
        return false;
    }

    const std::wstring mountDir = workDir + L"\\mount";
    if (!MountImage(outPath, mountDir)) {
        RemoveDirectoryRecursively(mountDir);
        RemoveFileIfPresent(outPath);
        return false;
    }
    
    HealMountedWinRE(mountDir);

    bool success = true;
    const bool needInject = !exeSourcePath.empty() && !exeNameInImage.empty();

    if (needInject) {
        const std::wstring targetExePath = mountDir + L"\\Windows\\System32\\" + exeNameInImage;
        if (CopyFileW(exeSourcePath.c_str(), targetExePath.c_str(), FALSE)) {
            const std::wstring iniPath = mountDir + L"\\Windows\\System32\\winpeshl.ini";
            std::wstring iniContent = L"[LaunchApps]\r\n";
            iniContent += L"X:\\Windows\\System32\\" + exeNameInImage + L"\r\n";
            
            HANDLE hIni = CreateFileW(iniPath.c_str(), GENERIC_WRITE, 0,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hIni != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                const wchar_t bom = 0xFEFF;
                WriteFile(hIni, &bom, sizeof(bom), &written, nullptr);

                WriteFile(hIni, iniContent.c_str(),
                    static_cast<DWORD>(iniContent.size() * sizeof(wchar_t)),
                    &written, nullptr);
                CloseHandle(hIni);
                success = (written > 0);
            }
            else {
                success = false;
            }
        }
        else {
            success = false;
        }
    }

    const bool committed = UnmountImage(mountDir, success);
    RemoveDirectoryRecursively(mountDir);

    if (!committed || !success) {
        RemoveFileIfPresent(outPath);
        return false;
    }

    return true;
}

bool RestorePreviousImage(const std::wstring& targetPath, const std::wstring& backupPath) {
    const size_t separator = targetPath.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return false;
    }
    const std::wstring recoveryDirectory = targetPath.substr(0, separator);

    DWORD exitCode = 0;
    RunReagentc({ L"/disable" }, exitCode);
    RemoveFileIfPresent(targetPath);

    const bool restored = CopyFileW(backupPath.c_str(), targetPath.c_str(), FALSE) != FALSE;
    const bool configured = RunReagentc({ L"/setreimage", L"/path", recoveryDirectory }, exitCode) && exitCode == 0;
    const bool enabled = RunReagentc({ L"/enable" }, exitCode) && exitCode == 0;

    return restored && configured && enabled;
}

} // namespace

bool ObtainCleanWinRE(bool useSystemSource, const std::wstring& outPath,
    const std::wstring& exeSourcePath,
    const std::wstring& exeNameInImage) {
    if (outPath.empty() || GetFileAttributesW(outPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    if (useSystemSource) {
        const size_t separator = outPath.find_last_of(L"\\/");
        const std::wstring workDir = separator != std::wstring::npos
            ? outPath.substr(0, separator)
            : outPath;
        return PrepareHealedAndInjectedWinRE(workDir, outPath, exeSourcePath, exeNameInImage);
    }

    return DownloadWinRE(WINRE_URL, outPath);
}

bool ReplaceWinREWithInjectedApp() {
    if (!IsRunAsAdmin()) {
        MessageBoxW(nullptr, L"Требуются права администратора.", L"WinRE",
            MB_OK | MB_ICONERROR);
        return false;
    }

    if (IsRunningInWinRE()) {
        MessageBoxW(nullptr,
            L"Нельзя заменить WinRE из запущенной среды восстановления.\r\n"
            L"Загрузите Windows и повторите попытку.",
            L"WinRE", MB_OK | MB_ICONERROR);
        return false;
    }
    
    wchar_t exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        MessageBoxW(nullptr, L"Не удалось получить путь к текущему приложению.", L"WinRE",
            MB_OK | MB_ICONERROR);
        return false;
    }

    std::wstring exeSourcePath = exePath;
    std::wstring exeNameInImage = L"sysim.exe";

    std::wstring workDirectory;
    if (!CreateWorkingDirectory(workDirectory)) {
        MessageBoxW(nullptr, L"Не удалось создать временный рабочий каталог.", L"WinRE",
            MB_OK | MB_ICONERROR);
        return false;
    }

    const std::wstring candidatePath = workDirectory + L"\\winre.wim";
    bool success = ObtainCleanWinRE(true, candidatePath, exeSourcePath, exeNameInImage);

    if (!success) {
        MessageBoxW(nullptr, L"Системный Winre.wim отсутствует или недоступен.", L"WinRE",
            MB_OK | MB_ICONERROR);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    if (!IsValidWinRECandidate(candidatePath)) {
        MessageBoxW(nullptr, L"Полученный Winre.wim имеет неверный заголовок или размер.", L"WinRE",
            MB_OK | MB_ICONERROR);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    std::wstring recoveryDirectory;
    const bool hasRecoveryDirectory = GetRecoveryDirectory(recoveryDirectory);
    const std::wstring targetPath = recoveryDirectory + L"\\Winre.wim";

    if (!hasRecoveryDirectory || !IsRegularFile(targetPath)) {
        MessageBoxW(nullptr,
            L"Системный файл C:\\Windows\\System32\\Recovery\\Winre.wim отсутствует или недоступен.",
            L"WinRE", MB_OK | MB_ICONERROR);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    const std::wstring backupPath = workDirectory + L"\\previous-winre.wim";
    const std::wstring pendingPath = recoveryDirectory + L"\\Winre.wim.sysim-new";

    if (!CopyFileW(targetPath.c_str(), backupPath.c_str(), TRUE)) {
        MessageBoxW(nullptr, L"Не удалось создать резервную копию Winre.wim.", L"WinRE",
            MB_OK | MB_ICONERROR);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    DWORD exitCode = 0;
    bool recoveryDisabled = false;

    if (!RunReagentc({ L"/disable" }, exitCode) || exitCode != 0) {
        MessageBoxW(nullptr, L"reagentc /disable завершился с ошибкой.", L"WinRE",
            MB_OK | MB_ICONERROR);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }
    recoveryDisabled = true;

    bool replacementStarted = false;
    RemoveFileIfPresent(pendingPath);

    if (!CopyFileW(candidatePath.c_str(), pendingPath.c_str(), TRUE)) {
        RemoveFileIfPresent(pendingPath);
        MessageBoxW(nullptr, L"Не удалось подготовить новый Winre.wim.", L"WinRE",
            MB_OK | MB_ICONERROR);
        if (recoveryDisabled) RunReagentc({ L"/enable" }, exitCode);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    if (!MoveFileExW(pendingPath.c_str(), targetPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        RemoveFileIfPresent(pendingPath);
        MessageBoxW(nullptr, L"Не удалось заменить системный Winre.wim.", L"WinRE",
            MB_OK | MB_ICONERROR);
        if (recoveryDisabled) RunReagentc({ L"/enable" }, exitCode);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }
    replacementStarted = true;

    if (!RunReagentc({ L"/setreimage", L"/path", recoveryDirectory }, exitCode) || exitCode != 0) {
        MessageBoxW(nullptr, L"reagentc /setreimage завершился с ошибкой.", L"WinRE",
            MB_OK | MB_ICONERROR);
        if (replacementStarted) RestorePreviousImage(targetPath, backupPath);
        else if (recoveryDisabled) RunReagentc({ L"/enable" }, exitCode);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    if (!RunReagentc({ L"/enable" }, exitCode) || exitCode != 0) {
        MessageBoxW(nullptr, L"reagentc /enable завершился с ошибкой.", L"WinRE",
            MB_OK | MB_ICONERROR);
        if (replacementStarted) RestorePreviousImage(targetPath, backupPath);
        else if (recoveryDisabled) RunReagentc({ L"/enable" }, exitCode);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    RemoveDirectoryRecursively(workDirectory);

    MessageBoxW(nullptr,
        L"WinRE успешно заменён, очищен от вирусов и настроен для автозапуска приложения.\r\n"
        L"При загрузке в среду восстановления приложение запустится автоматически.",
        L"WinRE", MB_OK | MB_ICONINFORMATION);
    return true;
}

bool ReplaceWinRE(bool useSystemSource) {
    if (!IsRunAsAdmin()) {
        MessageBoxW(nullptr, L"Требуются права администратора.", L"WinRE",
            MB_OK | MB_ICONERROR);
        return false;
    }

    if (IsRunningInWinRE()) {
        MessageBoxW(nullptr,
            L"Нельзя заменить WinRE из запущенной среды восстановления.\r\n"
            L"Загрузите Windows и повторите попытку.",
            L"WinRE", MB_OK | MB_ICONERROR);
        return false;
    }

    wchar_t self[MAX_PATH] = {};
    const DWORD selfLen = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (selfLen == 0 || selfLen >= MAX_PATH) {
        MessageBoxW(nullptr, L"Не удалось получить путь к текущему приложению.",
            L"WinRE", MB_OK | MB_ICONERROR);
        return false;
    }

    std::wstring workDirectory;
    if (!CreateWorkingDirectory(workDirectory)) {
        MessageBoxW(nullptr, L"Не удалось создать временный рабочий каталог.", L"WinRE",
            MB_OK | MB_ICONERROR);
        return false;
    }

    // Исходный WIM
    std::wstring rawWim;
    if (useSystemSource) {
        std::wstring recoveryDir;
        if (!GetRecoveryDirectory(recoveryDir)) {
            MessageBoxW(nullptr, L"C:\\Windows\\System32\\Recovery недоступен.", L"WinRE",
                MB_OK | MB_ICONERROR);
            RemoveDirectoryRecursively(workDirectory);
            return false;
        }
        rawWim = recoveryDir + L"\\Winre.wim";
        if (!IsRegularFile(rawWim)) {
            MessageBoxW(nullptr, L"Системный Winre.wim отсутствует.", L"WinRE",
                MB_OK | MB_ICONERROR);
            RemoveDirectoryRecursively(workDirectory);
            return false;
        }
    }
    else {
        rawWim = workDirectory + L"\\raw_winre.wim";
        if (!DownloadWinRE(WINRE_URL, rawWim)) {
            MessageBoxW(nullptr, L"Не удалось скачать Winre.wim с сервера.", L"WinRE",
                MB_OK | MB_ICONERROR);
            RemoveDirectoryRecursively(workDirectory);
            return false;
        }
    }

    if (!IsValidWinRECandidate(rawWim)) {
        MessageBoxW(nullptr, L"Полученный Winre.wim имеет неверный заголовок или размер.",
            L"WinRE", MB_OK | MB_ICONERROR);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    const std::wstring candidatePath = workDirectory + L"\\winre.wim";
    if (!PrepareHealedAndInjectedWinRE(workDirectory, candidatePath, self, L"sysim.exe")) {
        MessageBoxW(nullptr, L"Не удалось очистить и подготовить образ WinRE.",
            L"WinRE", MB_OK | MB_ICONERROR);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    if (!IsValidWinRECandidate(candidatePath)) {
        MessageBoxW(nullptr, L"Образ WinRE повреждён после обработки.", L"WinRE",
            MB_OK | MB_ICONERROR);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    // Подмена системного Winre.wim
    std::wstring recoveryDir;
    const bool hasRecovery = GetRecoveryDirectory(recoveryDir);
    const std::wstring targetPath = recoveryDir + L"\\Winre.wim";
    if (!hasRecovery || !IsRegularFile(targetPath)) {
        MessageBoxW(nullptr, L"C:\\Windows\\System32\\Recovery\\Winre.wim отсутствует.",
            L"WinRE", MB_OK | MB_ICONERROR);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    const std::wstring backupPath = workDirectory + L"\\previous-winre.wim";
    const std::wstring pendingPath = recoveryDir + L"\\Winre.wim.sysim-new";

    if (!CopyFileW(targetPath.c_str(), backupPath.c_str(), TRUE)) {
        MessageBoxW(nullptr, L"Не удалось создать резервную копию Winre.wim.", L"WinRE",
            MB_OK | MB_ICONERROR);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    DWORD ec = 0;
    if (!RunReagentc({ L"/disable" }, ec) || ec != 0) {
        MessageBoxW(nullptr, L"reagentc /disable завершился с ошибкой.", L"WinRE",
            MB_OK | MB_ICONERROR);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    RemoveFileIfPresent(pendingPath);

    if (!CopyFileW(candidatePath.c_str(), pendingPath.c_str(), TRUE)) {
        RemoveFileIfPresent(pendingPath);
        MessageBoxW(nullptr, L"Не удалось подготовить новый Winre.wim.", L"WinRE",
            MB_OK | MB_ICONERROR);
        RunReagentc({ L"/enable" }, ec);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    if (!MoveFileExW(pendingPath.c_str(), targetPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        RemoveFileIfPresent(pendingPath);
        MessageBoxW(nullptr, L"Не удалось заменить системный Winre.wim.", L"WinRE",
            MB_OK | MB_ICONERROR);
        RunReagentc({ L"/enable" }, ec);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    if (!RunReagentc({ L"/setreimage", L"/path", recoveryDir }, ec) || ec != 0) {
        MessageBoxW(nullptr, L"reagentc /setreimage завершился с ошибкой.", L"WinRE",
            MB_OK | MB_ICONERROR);
        RestorePreviousImage(targetPath, backupPath);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    if (!RunReagentc({ L"/enable" }, ec) || ec != 0) {
        MessageBoxW(nullptr, L"reagentc /enable завершился с ошибкой.", L"WinRE",
            MB_OK | MB_ICONERROR);
        RestorePreviousImage(targetPath, backupPath);
        RemoveDirectoryRecursively(workDirectory);
        return false;
    }

    RemoveDirectoryRecursively(workDirectory);

    MessageBoxW(nullptr,
        L"WinRE заменён, очищен и настроен на автозапуск SYSIM.\r\n"
        L"При следующей загрузке в среду восстановления SYSIM запустится автоматически.",
        L"WinRE", MB_OK | MB_ICONINFORMATION);
    return true;
}

namespace { // Диалог (выбор)

    constexpr int WNR_W = 420;
    constexpr int WNR_H = 170;
    constexpr int WNR_DRAG = 32;

    struct WinREReplaceState {
        int  result = 0;
        bool done = false;
        bool useSystem = true;
        int  hover = -1;
    };

    static RectF WnrClient(HWND h) {
        RECT rc{}; GetClientRect(h, &rc);
        return RectF(0.0f, 0.0f, (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));
    }

    static RectF WnrOptRect(const RectF& c, int idx) {
        return RectF(14.0f, 20.0f + idx * 46.0f, c.Width - 28.0f, 40.0f);
    }

    static void WnrBtnRects(const RectF& c, RectF& ok, RectF& cancel) {
        const float bw = 120.0f, bh = 34.0f, gap = 10.0f, m = 16.0f;
        ok = RectF(c.Width - m - bw, c.Height - m - bh, bw, bh);
        cancel = RectF(ok.X - gap - bw, ok.Y, bw, bh);
    }

    static void WnrDraw(HWND hWnd, HDC hdc) {
        WinREReplaceState* st = (WinREReplaceState*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
        if (!st) return;

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        RectF c = WnrClient(hWnd);
        SolidBrush bg(COLOR_BG);
        g.FillRectangle(&bg, c);

        FontFamily ff(g_fontFamilyName.c_str());
        StringFormat sf;
        sf.SetAlignment(StringAlignmentNear);
        sf.SetLineAlignment(StringAlignmentCenter);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);

        const wchar_t* opts[2] = {
            L"Системный (вылечить)",
            L"URL (скачать чистый)",
        };
        Font optFont(&ff, 13.5f, FontStyleRegular, UnitPixel);

        for (int i = 0; i < 2; ++i) {
            RectF r = WnrOptRect(c, i);
            const bool checked = (i == 0) ? st->useSystem : !st->useSystem;
            const bool hovered = (st->hover == i);

            if (hovered) {
                SolidBrush hb(COLOR_TAB_HOVER);
                g.FillRectangle(&hb, r);
            }

            const REAL cx = r.X + 18.0f;
            const REAL cy = r.Y + r.Height / 2.0f;
            const REAL box = 16.0f;

            RectF boxRect(cx - box / 2.0f, cy - box / 2.0f, box, box);

            Pen border(checked ? COLOR_TAB_ACTIVE : COLOR_BORDER, 1.5f);
            SolidBrush boxBg(checked ? COLOR_TAB_ACTIVE : COLOR_BUTTON_BG);
            g.FillRectangle(&boxBg, boxRect);
            g.DrawRectangle(&border, boxRect);

            if (checked) {
                Pen tick(COLOR_TEXT, 2.0f);
                g.DrawLine(&tick,
                    cx - 5.0f, cy,
                    cx - 1.0f, cy + 4.0f);
                g.DrawLine(&tick,
                    cx - 1.0f, cy + 4.0f,
                    cx + 5.0f, cy - 4.0f);
            }

            SolidBrush tb(checked ? COLOR_TEXT : COLOR_TEXT_MUTED);
            RectF tr(cx + box / 2.0f + 12.0f, r.Y,
                r.Width - (box + 36.0f), r.Height);
            g.DrawString(opts[i], -1, &optFont, tr, &sf, &tb);
        }

        RectF ok, cancel;
        WnrBtnRects(c, ok, cancel);

        Font btnFont(&ff, 13.0f, FontStyleRegular, UnitPixel);
        StringFormat bsf;
        bsf.SetAlignment(StringAlignmentCenter);
        bsf.SetLineAlignment(StringAlignmentCenter);

        auto drawBtn = [&](const RectF& r, const wchar_t* text, bool primary, bool hovered) {
            Color col = primary ? COLOR_TAB_ACTIVE
                : (hovered ? COLOR_TAB_HOVER : COLOR_BUTTON_BG);
            SolidBrush bb(col);
            g.FillRectangle(&bb, r);
            SolidBrush tb(COLOR_TEXT);
            g.DrawString(text, -1, &btnFont, r, &bsf, &tb);
            };
        drawBtn(cancel, L"Отмена", false, st->hover == 3);
        drawBtn(ok, L"Продолжить", true, st->hover == 2);
    }

    static LRESULT CALLBACK WinREReplaceDlgProc(HWND hWnd, UINT msg,
        WPARAM wParam, LPARAM lParam)
    {
        WinREReplaceState* st = (WinREReplaceState*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

        switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            st = (WinREReplaceState*)cs->lpCreateParams;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)st);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rc; GetClientRect(hWnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            if (w > 0 && h > 0) {
                HDC mem = CreateCompatibleDC(hdc);
                HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
                HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
                WnrDraw(hWnd, mem);
                BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            }

            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_NCHITTEST: {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);
            if (pt.y >= 0 && pt.y < WNR_DRAG) return HTCAPTION;
            return HTCLIENT;
        }

        case WM_MOUSEMOVE: {
            if (!st) break;
            const float x = (float)GET_X_LPARAM(lParam);
            const float y = (float)GET_Y_LPARAM(lParam);
            RectF c = WnrClient(hWnd);

            int hover = -1;
            for (int i = 0; i < 2; ++i) {
                if (WnrOptRect(c, i).Contains(x, y)) { hover = i; break; }
            }
            if (hover < 0) {
                RectF ok, cancel;
                WnrBtnRects(c, ok, cancel);
                if (ok.Contains(x, y))          hover = 2;
                else if (cancel.Contains(x, y)) hover = 3;
            }
            if (hover != st->hover) {
                st->hover = hover;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            if (st && st->hover != -1) {
                st->hover = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN: {
            if (!st) break;
            const float x = (float)GET_X_LPARAM(lParam);
            const float y = (float)GET_Y_LPARAM(lParam);
            RectF c = WnrClient(hWnd);

            for (int i = 0; i < 2; ++i) {
                if (WnrOptRect(c, i).Contains(x, y)) {
                    st->useSystem = (i == 0);
                    InvalidateRect(hWnd, nullptr, TRUE);
                    return 0;
                }
            }
            RectF ok, cancel;
            WnrBtnRects(c, ok, cancel);
            if (ok.Contains(x, y)) {
                st->result = st->useSystem ? 1 : 2;
                st->done = true;
                return 0;
            }
            if (cancel.Contains(x, y)) {
                st->result = 0;
                st->done = true;
                return 0;
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (!st) break;
            if (wParam == VK_ESCAPE) { st->result = 0; st->done = true; return 0; }
            if (wParam == VK_RETURN) {
                st->result = st->useSystem ? 1 : 2;
                st->done = true;
                return 0;
            }
            if (wParam == VK_UP || wParam == VK_DOWN) {
                st->useSystem = !st->useSystem;
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }
            return 0;

        case WM_CLOSE:
            if (st) { st->result = 0; st->done = true; }
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    int RunReplaceDialog(HWND parent) {
        static bool reg = false;
        HINSTANCE hInst = GetModuleHandleW(nullptr);

        if (!reg) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = WinREReplaceDlgProc;
            wc.hInstance = hInst;
            wc.lpszClassName = L"SYSIM_WinREReplaceDlg";
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            wc.style = CS_HREDRAW | CS_VREDRAW;
            RegisterClassExW(&wc);
            reg = true;
        }

        RECT pr{};
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        if (parent && GetWindowRect(parent, &pr)) {
            x = pr.left + ((pr.right - pr.left) - WNR_W) / 2;
            y = pr.top + ((pr.bottom - pr.top) - WNR_H) / 2;
        }

        WinREReplaceState st;
        HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"SYSIM_WinREReplaceDlg", L"",
            WS_POPUP,
            x, y, WNR_W, WNR_H, parent, nullptr, hInst, &st);
        if (!h) return 0;

        ShowWindow(h, SW_SHOW);
        UpdateWindow(h);
        if (parent) EnableWindow(parent, FALSE);
        SetForegroundWindow(h);
        SetFocus(h);

        while (!st.done) {
            MSG m{};
            if (GetMessageW(&m, nullptr, 0, 0) <= 0) break;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }

        if (parent) EnableWindow(parent, TRUE);
        DestroyWindow(h);
        if (parent) SetForegroundWindow(parent);
        return st.result;
    }

} // namespace

bool ShowReplaceWinREDialog(HWND owner) {
    const int choice = RunReplaceDialog(owner);
    if (choice == 0) return false;
    return ReplaceWinRE(choice == 1);
}