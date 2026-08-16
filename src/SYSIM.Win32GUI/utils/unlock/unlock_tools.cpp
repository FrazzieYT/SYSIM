#include "unlock_tools.h"
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "advapi32.lib")
static std::wstring g_offlineDriveHint;
namespace UnlockTools {

    // ================================================================
    // ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ (detail)
    // ================================================================
    namespace detail {

        bool EnablePrivilege(const wchar_t* privilegeName) {
            HANDLE hToken = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(),
                TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) return false;
            LUID luid{};
            if (!LookupPrivilegeValueW(nullptr, privilegeName, &luid)) {
                CloseHandle(hToken); return false;
            }
            TOKEN_PRIVILEGES tp{};
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Luid = luid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
            DWORD error = GetLastError();
            CloseHandle(hToken);
            return ok && error == ERROR_SUCCESS;
        }

        void EnableOfflinePrivileges() {
            EnablePrivilege(SE_BACKUP_NAME);
            EnablePrivilege(SE_RESTORE_NAME);
        }

        LONG OpenOnlineKey(HKEY root, const std::wstring& subKey, REGSAM access, HKEY* outKey) {
            if (!outKey) return ERROR_INVALID_PARAMETER;
            LONG status = RegOpenKeyExW(root, subKey.c_str(), 0, access | KEY_WOW64_64KEY, outKey);
            if (status != ERROR_SUCCESS)
                status = RegOpenKeyExW(root, subKey.c_str(), 0, access, outKey);
            return status;
        }

        bool ReadDwordOnline(HKEY root, const std::wstring& subKey,
            const std::wstring& valueName, DWORD& outValue) {
            HKEY hKey = nullptr;
            if (OpenOnlineKey(root, subKey, KEY_READ, &hKey) != ERROR_SUCCESS) return false;
            DWORD type = 0, size = sizeof(outValue);
            LONG st = RegQueryValueExW(hKey, valueName.c_str(), nullptr, &type,
                reinterpret_cast<LPBYTE>(&outValue), &size);
            RegCloseKey(hKey);
            return st == ERROR_SUCCESS && type == REG_DWORD;
        }

        bool ReadStringOnline(HKEY root, const std::wstring& subKey,
            const wchar_t* valueName, std::wstring& out) {
            HKEY hKey = nullptr;
            if (OpenOnlineKey(root, subKey, KEY_READ, &hKey) != ERROR_SUCCESS) return false;
            wchar_t buf[1024]{}; DWORD size = sizeof(buf), type = 0;
            LONG st = RegQueryValueExW(hKey, valueName, nullptr, &type,
                reinterpret_cast<LPBYTE>(buf), &size);
            RegCloseKey(hKey);
            if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
            out = buf;
            return true;
        }

        std::wstring ToLowerCopy(std::wstring s) {
            for (wchar_t& c : s) if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
            return s;
        }

        bool SetDwordAt(HKEY root, const std::wstring& subKey,
            const wchar_t* valueName, DWORD value) {
            HKEY hKey = nullptr;
            if (OpenOnlineKey(root, subKey, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return false;
            LONG r = RegSetValueExW(hKey, valueName, 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&value), sizeof(value));
            RegCloseKey(hKey);
            return r == ERROR_SUCCESS;
        }

        bool SetSzAt(HKEY root, const std::wstring& subKey,
            const wchar_t* valueName, const std::wstring& data) {
            HKEY hKey = nullptr;
            if (OpenOnlineKey(root, subKey, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return false;
            LONG r = RegSetValueExW(hKey, valueName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(data.c_str()),
                (DWORD)((data.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
            return r == ERROR_SUCCESS;
        }

        bool DeleteValueAt(HKEY root, const std::wstring& subKey, const wchar_t* valueName) {
            HKEY hKey = nullptr;
            if (OpenOnlineKey(root, subKey, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return true;
            RegDeleteValueW(hKey, valueName);
            RegCloseKey(hKey);
            return true;
        }

        std::wstring FindOfflineWindowsDrive() {
            // 1. Подсказка от пользователя (из настроек или чипсов на вкладке)
            if (!g_offlineDriveHint.empty()) {
                std::wstring softwarePath = g_offlineDriveHint + L"Windows\\System32\\config\\SOFTWARE";
                std::wstring systemPath = g_offlineDriveHint + L"Windows\\System32\\config\\SYSTEM";
                if (GetFileAttributesW(softwarePath.c_str()) != INVALID_FILE_ATTRIBUTES &&
                    GetFileAttributesW(systemPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    return g_offlineDriveHint;
                }
            }
            // 2. Автопоиск C..Z, требуем оба файла: SYSTEM и SOFTWARE
            for (wchar_t drive = L'C'; drive <= L'Z'; ++drive) {
                std::wstring root; root += drive; root += L":\\";
                std::wstring softwarePath = root + L"Windows\\System32\\config\\SOFTWARE";
                std::wstring systemPath = root + L"Windows\\System32\\config\\SYSTEM";
                if (GetFileAttributesW(softwarePath.c_str()) != INVALID_FILE_ATTRIBUTES &&
                    GetFileAttributesW(systemPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    return root;
                }
            }
            return L"";
        }

        bool IsLikelyWinRE() {
            wchar_t sd[16] = {};
            if (GetEnvironmentVariableW(L"SystemDrive", sd, 16) > 0 &&
                _wcsicmp(sd, L"X:") == 0) return true;
            HKEY h = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\MiniNT", 0, KEY_READ, &h) == ERROR_SUCCESS) {
                RegCloseKey(h); return true;
            }
            return false;
        }

        bool CleanDisallowRunAt(HKEY root, const std::wstring& explorerSubKey) {
            HKEY hExplorer = nullptr;
            LONG status = OpenOnlineKey(root, explorerSubKey, KEY_SET_VALUE | KEY_QUERY_VALUE, &hExplorer);
            if (status != ERROR_SUCCESS) return status == ERROR_FILE_NOT_FOUND;
            RegDeleteValueW(hExplorer, L"DisallowRun");
            HKEY hDisallowRun = nullptr;
            if (RegOpenKeyExW(hExplorer, L"DisallowRun", 0, KEY_SET_VALUE | KEY_QUERY_VALUE,
                &hDisallowRun) == ERROR_SUCCESS) {
                for (;;) {
                    wchar_t valueName[16384]{}; DWORD valueNameLen = 16384;
                    if (RegEnumValueW(hDisallowRun, 0, valueName, &valueNameLen,
                        nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                    RegDeleteValueW(hDisallowRun, valueName);
                }
                RegCloseKey(hDisallowRun);
                RegDeleteKeyW(hExplorer, L"DisallowRun");
            }
            RegCloseKey(hExplorer);
            return true;
        }

        bool CleanIFEOAt(HKEY root, const std::wstring& ifeoSubKey) {
            HKEY hIFEO = nullptr;
            LONG status = OpenOnlineKey(root, ifeoSubKey, KEY_ENUMERATE_SUB_KEYS | KEY_SET_VALUE, &hIFEO);
            if (status != ERROR_SUCCESS) return status == ERROR_FILE_NOT_FOUND;
            for (DWORD index = 0;; ++index) {
                wchar_t subKeyName[1024]{}; DWORD subKeyNameLen = 1024;
                if (RegEnumKeyExW(hIFEO, index, subKeyName, &subKeyNameLen,
                    nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                HKEY hSub = nullptr;
                if (RegOpenKeyExW(hIFEO, subKeyName, 0, KEY_SET_VALUE, &hSub) == ERROR_SUCCESS) {
                    RegDeleteValueW(hSub, L"Debugger");
                    RegCloseKey(hSub);
                }
            }
            RegCloseKey(hIFEO);
            return true;
        }

        static const wchar_t* kWinlogonKey =
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";

        bool IsWinlogonShellHijacked() {
            std::wstring shell;
            if (!ReadStringOnline(HKEY_LOCAL_MACHINE, kWinlogonKey, L"Shell", shell)) return false;
            return ToLowerCopy(shell).find(L"explorer.exe") == std::wstring::npos;
        }

        bool FixWinlogonShell() {
            bool ok = SetSzAt(HKEY_LOCAL_MACHINE, kWinlogonKey, L"Shell", L"explorer.exe");
            ok = SetSzAt(HKEY_LOCAL_MACHINE, kWinlogonKey, L"Userinit",
                L"C:\\Windows\\system32\\userinit.exe,") && ok;
            DeleteValueAt(HKEY_LOCAL_MACHINE, kWinlogonKey, L"Taskman");
            return ok;
        }

        bool HasLegalNotice() {
            std::wstring s;
            if (ReadStringOnline(HKEY_LOCAL_MACHINE, kWinlogonKey, L"LegalNoticeCaption", s) && !s.empty()) return true;
            if (ReadStringOnline(HKEY_LOCAL_MACHINE, kWinlogonKey, L"LegalNoticeText", s) && !s.empty()) return true;
            return false;
        }

        bool ClearLegalNotice() {
            DeleteValueAt(HKEY_LOCAL_MACHINE, kWinlogonKey, L"LegalNoticeCaption");
            DeleteValueAt(HKEY_LOCAL_MACHINE, kWinlogonKey, L"LegalNoticeText");
            return true;
        }

        bool ReadAssocCommand(std::wstring& out) {
            HKEY hKey = nullptr;
            if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"exefile\\shell\\open\\command", 0,
                KEY_READ, &hKey) != ERROR_SUCCESS) return false;
            wchar_t buf[1024]{}; DWORD size = sizeof(buf), type = 0;
            LONG st = RegQueryValueExW(hKey, nullptr, nullptr, &type,
                reinterpret_cast<LPBYTE>(buf), &size);
            RegCloseKey(hKey);
            if (st != ERROR_SUCCESS) return false;
            out = buf;
            return true;
        }

        bool AreExeAssociationsHijacked() {
            std::wstring cmd;
            if (!ReadAssocCommand(cmd)) return true;
            return cmd.find(L"%1") == std::wstring::npos;
        }

        bool FixExeAssociations() {
            struct A { const wchar_t* id; const wchar_t* cmd; };
            static const A items[] = {
                { L"exefile",  L"\"%1\" %*" },
                { L"comfile",  L"\"%1\" %*" },
                { L"batfile",  L"\"%1\" %*" },
                { L"cmdfile",  L"\"%1\" %*" },
                { L"piffile",  L"\"%1\" %*" },
            };
            bool ok = true;
            for (const A& a : items) {
                std::wstring sub = std::wstring(a.id) + L"\\shell\\open\\command";
                HKEY hKey = nullptr;
                if (RegCreateKeyExW(HKEY_CLASSES_ROOT, sub.c_str(), 0, nullptr, 0,
                    KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
                    ok = false; continue;
                }
                if (RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(a.cmd),
                    (DWORD)((wcslen(a.cmd) + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) ok = false;
                RegCloseKey(hKey);
            }
            return ok;
        }

        static const wchar_t* kInetKey =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";

        bool IsProxyForced() {
            DWORD v = 0;
            if (ReadDwordOnline(HKEY_CURRENT_USER, kInetKey, L"ProxyEnable", v) && v != 0) return true;
            std::wstring s;
            if (ReadStringOnline(HKEY_CURRENT_USER, kInetKey, L"AutoConfigURL", s) && !s.empty()) return true;
            return false;
        }

        bool ResetProxy() {
            SetDwordAt(HKEY_CURRENT_USER, kInetKey, L"ProxyEnable", 0);
            DeleteValueAt(HKEY_CURRENT_USER, kInetKey, L"ProxyServer");
            DeleteValueAt(HKEY_CURRENT_USER, kInetKey, L"AutoConfigURL");
            return true;
        }

        static const wchar_t* kAdvKey =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

        bool AreHiddenFilesForced() {
            DWORD v = 0;
            return ReadDwordOnline(HKEY_CURRENT_USER, kAdvKey, L"Hidden", v) && v == 2;
        }

        bool RestoreHiddenFiles() {
            bool ok = SetDwordAt(HKEY_CURRENT_USER, kAdvKey, L"Hidden", 1);
            ok = SetDwordAt(HKEY_CURRENT_USER, kAdvKey, L"ShowSuperHidden", 1) && ok;
            ok = SetDwordAt(HKEY_CURRENT_USER, kAdvKey, L"HideFileExt", 0) && ok;
            return ok;
        }

        static const wchar_t* kWinKey =
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows";

        bool HasAppInitDlls() {
            std::wstring s;
            if (ReadStringOnline(HKEY_LOCAL_MACHINE, kWinKey, L"AppInit_DLLs", s) && !s.empty()) return true;
            DWORD v = 0;
            if (ReadDwordOnline(HKEY_LOCAL_MACHINE, kWinKey, L"LoadAppInit_DLLs", v) && v != 0) return true;
            return false;
        }

        bool ClearAppInitDlls() {
            SetSzAt(HKEY_LOCAL_MACHINE, kWinKey, L"AppInit_DLLs", L"");
            SetDwordAt(HKEY_LOCAL_MACHINE, kWinKey, L"LoadAppInit_DLLs", 0);
            return true;
        }

        bool AreDefenderServicesDisabled() {
            DWORD v = 0;
            return ReadDwordOnline(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Services\\WinDefend", L"Start", v) && v == 4;
        }

        bool RestoreDefenderServices() {
            struct S { const wchar_t* name; DWORD start; };
            static const S svc[] = {
                { L"WinDefend", 2 }, { L"WdNisSvc", 3 }, { L"Sense", 3 },
                { L"SecurityHealthService", 2 }, { L"wscsvc", 2 },
            };
            bool ok = true;
            for (const S& s : svc) {
                ok = SetDwordAt(HKEY_LOCAL_MACHINE,
                    std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\") + s.name,
                    L"Start", s.start) && ok;
            }
            return ok;
        }

        bool IsHostsHijackedAt(const std::wstring& hostsPath) {
            HANDLE h = CreateFileW(hostsPath.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) return false;
            char buf[65536]; DWORD rd = 0; std::string acc;
            while (ReadFile(h, buf, sizeof(buf), &rd, nullptr) && rd > 0) acc.append(buf, rd);
            CloseHandle(h);
            int wn = MultiByteToWideChar(CP_ACP, 0, acc.c_str(), (int)acc.size(), nullptr, 0);
            std::wstring text(wn > 0 ? wn : 0, 0);
            if (wn > 0) MultiByteToWideChar(CP_ACP, 0, acc.c_str(), (int)acc.size(), &text[0], wn);
            size_t pos = 0;
            while (pos < text.size()) {
                size_t nl = text.find_first_of(L"\r\n", pos);
                std::wstring line = text.substr(pos,
                    (nl == std::wstring::npos) ? std::wstring::npos : nl - pos);
                pos = (nl == std::wstring::npos) ? text.size() : nl + 1;
                size_t b = line.find_first_not_of(L" \t");
                if (b == std::wstring::npos) continue;
                std::wstring t = line.substr(b);
                if (t.empty() || t[0] == L'#') continue;
                if (t.find(L"localhost") != std::wstring::npos) continue;
                if (t.find_first_of(L" \t") != std::wstring::npos) return true;
            }
            return false;
        }

        bool WriteDefaultHosts(const std::wstring& hostsPath) {
            std::wstring backup = hostsPath + L".bak";
            CopyFileW(hostsPath.c_str(), backup.c_str(), FALSE);
            const char* def =
                "# Copyright (c) 1993-2009 Microsoft Corp.\r\n"
                "#\r\n"
                "# This is a sample HOSTS file used by Microsoft TCP/IP for Windows.\r\n"
                "\r\n"
                "127.0.0.1       localhost\r\n"
                "::1             localhost\r\n";
            HANDLE h = CreateFileW(hostsPath.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) return false;
            DWORD written = 0;
            WriteFile(h, def, (DWORD)strlen(def), &written, nullptr);
            CloseHandle(h);
            return true;
        }

        bool IsHostsHijacked() {
            wchar_t sysDir[MAX_PATH] = {};
            if (GetSystemDirectoryW(sysDir, MAX_PATH) == 0) return false;
            return IsHostsHijackedAt(std::wstring(sysDir) + L"\\drivers\\etc\\hosts");
        }

        bool RestoreDefaultHosts() {
            wchar_t sysDir[MAX_PATH] = {};
            if (GetSystemDirectoryW(sysDir, MAX_PATH) == 0) return false;
            return WriteDefaultHosts(std::wstring(sysDir) + L"\\drivers\\etc\\hosts");
        }

        bool RunBcdEdit(const std::wstring& args, std::wstring& out) {
            SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
            HANDLE hRead = nullptr, hWrite = nullptr;
            if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
            SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{}; si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = hWrite; si.hStdError = hWrite;
            PROCESS_INFORMATION pi{};
            std::wstring cmd = L"bcdedit.exe " + args;
            BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            CloseHandle(hWrite);
            if (!ok) { CloseHandle(hRead); return false; }
            std::string acc; char buf[4096]; DWORD rd = 0;
            while (ReadFile(hRead, buf, sizeof(buf), &rd, nullptr) && rd > 0) acc.append(buf, rd);
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(hRead);
            int wn = MultiByteToWideChar(CP_ACP, 0, acc.c_str(), (int)acc.size(), nullptr, 0);
            out.resize(wn > 0 ? wn : 0);
            if (wn > 0) MultiByteToWideChar(CP_ACP, 0, acc.c_str(), (int)acc.size(), &out[0], wn);
            return code == 0;
        }

        std::wstring BcdPrefix() {
            if (IsLikelyWinRE()) {
                std::wstring drv = FindOfflineWindowsDrive();
                if (!drv.empty()) return L"/store \"" + drv + L"Boot\\BCD\" ";
            }
            return L"";
        }

        std::wstring BcdObject() { return IsLikelyWinRE() ? L"{default}" : L"{current}"; }

        // ---- Offline helpers ----
        struct OfflineFix {
            std::wstring subKey;
            std::wstring valueName;
            DWORD value;
            bool deleteValue;
        };

        bool ApplyOfflineFix(HKEY root, const OfflineFix& fix) {
            if (!root) return false;
            HKEY hKey = nullptr;
            if (RegOpenKeyExW(root, fix.subKey.c_str(), 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
                return true;
            LONG result = ERROR_SUCCESS;
            if (fix.deleteValue) result = RegDeleteValueW(hKey, fix.valueName.c_str());
            else result = RegSetValueExW(hKey, fix.valueName.c_str(), 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&fix.value), sizeof(fix.value));
            RegCloseKey(hKey);
            return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
        }

        template <typename Fn>
        void ForEachUserHive(const std::wstring& usersDir, Fn callback) {
            WIN32_FIND_DATAW findData{};
            HANDLE hFind = FindFirstFileW((usersDir + L"\\*").c_str(), &findData);
            if (hFind == INVALID_HANDLE_VALUE) return;
            do {
                if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::wstring userName = findData.cFileName;
                if (userName == L"." || userName == L".." || userName == L"Public" ||
                    userName == L"All Users" || userName == L"Default" ||
                    userName == L"Default User") continue;
                std::wstring ntUserPath = usersDir + L"\\" + userName + L"\\NTUSER.DAT";
                if (GetFileAttributesW(ntUserPath.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                HKEY hUser = nullptr;
                if (RegLoadAppKeyW(ntUserPath.c_str(), &hUser, KEY_ALL_ACCESS, 0, 0) == ERROR_SUCCESS) {
                    callback(hUser);
                    RegFlushKey(hUser);
                    RegCloseKey(hUser);
                }
            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
        }

        bool ReadDwordOffline(HKEY root, const std::wstring& subKey,
            const std::wstring& valueName, DWORD& outValue) {
            if (!root) return false;
            HKEY hKey = nullptr;
            if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) return false;
            DWORD type = 0, size = sizeof(outValue);
            LONG st = RegQueryValueExW(hKey, valueName.c_str(), nullptr, &type,
                reinterpret_cast<LPBYTE>(&outValue), &size);
            RegCloseKey(hKey);
            return st == ERROR_SUCCESS && type == REG_DWORD;
        }

        void CheckOfflineValue(HKEY root, const std::wstring& subKey,
            const std::wstring& valueName, const std::wstring& description,
            bool zeroMeansBlocked, std::wstring& report, int& blockedCount) {
            DWORD value = 0;
            if (!ReadDwordOffline(root, subKey, valueName, value)) return;
            bool blocked = zeroMeansBlocked ? (value == 0) : (value != 0);
            if (blocked) {
                report += L"Заблокировано: " + description + L" (offline)\r\n";
                ++blockedCount;
            }
        }

        int ScanOfflineWindows(std::wstring& report) {
            EnableOfflinePrivileges();
            std::wstring winDrive = FindOfflineWindowsDrive();
            if (winDrive.empty()) return -1;
            std::wstring softwarePath = winDrive + L"Windows\\System32\\config\\SOFTWARE";
            HKEY hSoft = nullptr;
            LONG status = RegLoadAppKeyW(softwarePath.c_str(), &hSoft, KEY_READ, 0, 0);
            if (status != ERROR_SUCCESS)
                status = RegLoadAppKeyW(softwarePath.c_str(), &hSoft, KEY_ALL_ACCESS, 0, 0);
            if (status != ERROR_SUCCESS) return -1;

            int blockedCount = 0;
            struct ScanItem {
                std::wstring subKey; std::wstring valueName;
                std::wstring description; bool zeroMeansBlocked;
            };
            std::vector<ScanItem> softwareItems = {
                { L"Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableTaskMgr", L"Диспетчер задач", false },
                { L"Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableRegistryTools", L"Редактор реестра", false },
                { L"Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"EnableLUA", L"UAC", true },
                { L"Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableCMD", L"CMD", false },
                { L"Policies\\Microsoft\\Windows\\System", L"DisableCMD", L"CMD (политика)", false },
                { L"Policies\\Microsoft\\Windows NT\\SystemRestore", L"DisableSR", L"Восстановление системы", false },
                { L"Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoDrives", L"Скрытие дисков", false },
                { L"Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoViewOnDrive", L"Доступ к дискам", false },
            };
            for (const auto& item : softwareItems)
                CheckOfflineValue(hSoft, item.subKey, item.valueName, item.description,
                    item.zeroMeansBlocked, report, blockedCount);
            if (HasDisallowRunAt(hSoft, L"Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer")) {
                report += L"Заблокировано: DisallowRun (offline HKLM)\r\n"; ++blockedCount;
            }
            if (HasIFEODebuggerAt(hSoft, L"Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options")) {
                report += L"Заблокировано: IFEO Debugger (offline)\r\n"; ++blockedCount;
            }
            RegCloseKey(hSoft);

            std::wstring usersDir = winDrive + L"Users";
            WIN32_FIND_DATAW findData{};
            HANDLE hFind = FindFirstFileW((usersDir + L"\\*").c_str(), &findData);
            if (hFind != INVALID_HANDLE_VALUE) {
                std::vector<ScanItem> userItems = {
                    { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableTaskMgr", L"Диспетчер задач (пользователь)", false },
                    { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableRegistryTools", L"Реестр (пользователь)", false },
                    { L"Software\\Policies\\Microsoft\\Windows\\System", L"DisableCMD", L"CMD (пользователь)", false },
                    { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoRun", L"'Выполнить' (Win+R)", false },
                    { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoControlPanel", L"Панель управления", false },
                    { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoViewContextMenu", L"Контекстное меню", false },
                    { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoFolderOptions", L"Параметры папок", false },
                    { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoClose", L"Кнопка выключения", false },
                };
                do {
                    if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    std::wstring userName = findData.cFileName;
                    if (userName == L"." || userName == L".." || userName == L"Public" ||
                        userName == L"All Users" || userName == L"Default" ||
                        userName == L"Default User") continue;
                    std::wstring ntUserPath = usersDir + L"\\" + userName + L"\\NTUSER.DAT";
                    if (GetFileAttributesW(ntUserPath.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                    HKEY hUser = nullptr;
                    LONG userStatus = RegLoadAppKeyW(ntUserPath.c_str(), &hUser, KEY_READ, 0, 0);
                    if (userStatus != ERROR_SUCCESS)
                        userStatus = RegLoadAppKeyW(ntUserPath.c_str(), &hUser, KEY_ALL_ACCESS, 0, 0);
                    if (userStatus != ERROR_SUCCESS) continue;
                    for (const auto& item : userItems)
                        CheckOfflineValue(hUser, item.subKey, item.valueName, item.description,
                            item.zeroMeansBlocked, report, blockedCount);
                    if (HasDisallowRunAt(hUser, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer")) {
                        report += L"Заблокировано: DisallowRun (offline, пользователь)\r\n"; ++blockedCount;
                    }
                    RegCloseKey(hUser);
                } while (FindNextFileW(hFind, &findData));
                FindClose(hFind);
            }
            return blockedCount;
        }

        // ---- Дополнительные утилиты ----
        bool DeleteKeyTree(HKEY root, const std::wstring& subKey) {
            HKEY hKey = nullptr;
            LONG st = RegOpenKeyExW(root, subKey.c_str(), 0, KEY_ALL_ACCESS, &hKey);
            if (st != ERROR_SUCCESS) return false;
            RegDeleteTreeW(hKey, nullptr);
            RegCloseKey(hKey);
            LONG r = RegDeleteKeyW(root, subKey.c_str());
            return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
        }

        bool RunCapture(const std::wstring& command, std::wstring& log) {
            SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
            HANDLE hRead = nullptr, hWrite = nullptr;
            if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
            SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{}; si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = hWrite; si.hStdError = hWrite;
            PROCESS_INFORMATION pi{};
            std::wstring cmd = command;
            BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            CloseHandle(hWrite);
            if (!ok) { CloseHandle(hRead); log += L"[ошибка запуска] " + command + L"\r\n"; return false; }
            std::string acc; char buf[4096]; DWORD rd = 0;
            while (ReadFile(hRead, buf, sizeof(buf), &rd, nullptr) && rd > 0) acc.append(buf, rd);
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(hRead);
            int wn = MultiByteToWideChar(CP_OEMCP, 0, acc.c_str(), (int)acc.size(), nullptr, 0);
            std::wstring ws(wn > 0 ? wn : 0, 0);
            if (wn > 0) MultiByteToWideChar(CP_OEMCP, 0, acc.c_str(), (int)acc.size(), &ws[0], wn);
            log += L"> " + command + L"\r\n" + ws + L"\r\n";
            return code == 0;
        }

        bool FileExistsPath(const std::wstring& p) {
            return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
        }

    } // namespace detail

    // ================================================================
    // ПУБЛИЧНЫЕ ФУНКЦИИ
    // ================================================================

    // ----- BCD -----
    bool IsBcdSafeBootEnabled() {
        std::wstring out;
        if (!detail::RunBcdEdit(detail::BcdPrefix() + L"/enum " + detail::BcdObject(), out)) return false;
        return out.find(L"safeboot") != std::wstring::npos;
    }

    bool ClearBcdSafeBoot() {
        std::wstring out;
        bool ok = detail::RunBcdEdit(detail::BcdPrefix() + L"/deletevalue " + detail::BcdObject() + L" safeboot", out);
        detail::RunBcdEdit(detail::BcdPrefix() + L"/deletevalue " + detail::BcdObject() + L" safebootalternate", out);
        return ok;
    }

    // ----- Список ограничений -----
    std::vector<Restriction> GetKnownRestrictions() {
        std::vector<Restriction> list;
        auto add = [&](const std::wstring& description, const std::vector<HKEY>& hives,
            const std::wstring& subKey, const std::wstring& valueName,
            DWORD disableValue, bool deleteInsteadOfSet) {
                Restriction r;
                r.description = description; r.hives = hives; r.subKey = subKey;
                r.valueName = valueName; r.disableValue = disableValue;
                r.deleteInsteadOfSet = deleteInsteadOfSet;
                list.push_back(r);
            };
        std::vector<HKEY> hkcu = { HKEY_CURRENT_USER };
        std::vector<HKEY> hklm = { HKEY_LOCAL_MACHINE };
        std::vector<HKEY> both = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };

        add(L"Диспетчер задач", both,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableTaskMgr", 0, false);
        add(L"Редактор реестра", both,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableRegistryTools", 0, false);
        add(L"Командная строка (CMD)", both,
            L"Software\\Policies\\Microsoft\\Windows\\System", L"DisableCMD", 0, false);
        add(L"Командная строка (CMD, старый путь)", both,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableCMD", 0, false);
        add(L"UAC (Контроль учётных записей)", hklm,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"EnableLUA", 1, false);
        add(L"Панель управления", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoControlPanel", 0, false);
        add(L"Окно 'Выполнить' (Win+R)", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoRun", 0, false);
        add(L"Доступ к дискам", both,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoViewOnDrive", 0, false);
        add(L"Скрытие дисков", both,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoDrives", 0, false);
        add(L"Поиск в Пуске", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoFind", 0, false);
        add(L"Контекстное меню", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoViewContextMenu", 0, false);
        add(L"Параметры папок", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoFolderOptions", 0, false);
        add(L"Вкладка 'Безопасность'", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoSecurityTab", 0, false);
        add(L"Меню 'Файл'", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoFileMenu", 0, false);
        add(L"Кнопка выключения", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoClose", 0, false);
        add(L"Выход из системы", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoLogoff", 0, false);
        add(L"Смена обоев", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\ActiveDesktop", L"NoChangingWallPaper", 0, false);
        add(L"Персонализация (NoDispCPL)", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"NoDispCPL", 0, false);
        add(L"Настройки фона (NoDispBackgroundPage)", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"NoDispBackgroundPage", 0, false);
        add(L"Горячие клавиши Win", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoWinKeys", 0, false);
        add(L"Настройки панели задач", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoSetTaskbar", 0, false);
        add(L"Иконки рабочего стола (NoDesktop)", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoDesktop", 0, false);
        add(L"Блокировка рабочей станции", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableLockWorkstation", 0, false);
        add(L"Смена пароля", hkcu,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableChangePassword", 0, false);
        add(L"Восстановление системы", hklm,
            L"Software\\Policies\\Microsoft\\Windows NT\\SystemRestore", L"DisableSR", 0, false);
        add(L"Установщик MSI (DisableMSI)", hklm,
            L"Software\\Policies\\Microsoft\\Windows\\Installer", L"DisableMSI", 0, false);
        add(L"PowerShell (скрипты запрещены)", both,
            L"Software\\Policies\\Microsoft\\Windows\\PowerShell", L"DisableRunScripts", 0, true);
        add(L"Defender (DisableAntiSpyware)", hklm,
            L"Software\\Policies\\Microsoft\\Windows Defender", L"DisableAntiSpyware", 0, false);
        add(L"Defender (DisableAntiVirus)", hklm,
            L"Software\\Policies\\Microsoft\\Windows Defender", L"DisableAntiVirus", 0, false);
        add(L"Defender (мониторинг в реальном времени)", hklm,
            L"Software\\Policies\\Microsoft\\Windows Defender\\Real-Time Protection", L"DisableRealtimeMonitoring", 0, false);
        add(L"MMC (только разрешённые оснастки)", both,
            L"Software\\Policies\\Microsoft\\MMC", L"RestrictToPermittedSnapins", 0, false);
        return list;
    }

    // ----- Проверка и снятие блокировок -----
    bool IsRestricted(const Restriction& r) {
        for (HKEY hive : r.hives) {
            DWORD value = 0;
            if (detail::ReadDwordOnline(hive, r.subKey, r.valueName, value)) {
                if (value != r.disableValue) return true;
            }
        }
        return false;
    }

    bool UnlockRestriction(const Restriction& r) {
        bool ok = true;
        for (HKEY hive : r.hives) {
            HKEY hKey = nullptr;
            LONG status = detail::OpenOnlineKey(hive, r.subKey, KEY_SET_VALUE, &hKey);
            if (status != ERROR_SUCCESS) {
                if (status != ERROR_FILE_NOT_FOUND) ok = false;
                continue;
            }
            DWORD dummy = 0; DWORD size = sizeof(dummy);
            if (RegQueryValueExW(hKey, r.valueName.c_str(), nullptr, nullptr,
                reinterpret_cast<LPBYTE>(&dummy), &size) == ERROR_SUCCESS) {
                LONG result = ERROR_SUCCESS;
                if (r.deleteInsteadOfSet) result = RegDeleteValueW(hKey, r.valueName.c_str());
                else result = RegSetValueExW(hKey, r.valueName.c_str(), 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&r.disableValue), sizeof(r.disableValue));
                if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) ok = false;
            }
            RegCloseKey(hKey);
        }
        return ok;
    }

    bool ClearDisallowRun() {
        bool ok = true;
        ok &= detail::CleanDisallowRunAt(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer");
        ok &= detail::CleanDisallowRunAt(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer");
        return ok;
    }

    bool ClearImageFileExecutionOptions() {
        return detail::CleanIFEOAt(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options");
    }

    // ----- Offline -----
    bool RepairOfflineWindows() {
        detail::EnableOfflinePrivileges();
        std::wstring winDrive = detail::FindOfflineWindowsDrive();
        if (winDrive.empty()) return false;
        std::wstring softwarePath = winDrive + L"Windows\\System32\\config\\SOFTWARE";
        HKEY hSoft = nullptr;
        if (RegLoadAppKeyW(softwarePath.c_str(), &hSoft, KEY_ALL_ACCESS, 0, 0) != ERROR_SUCCESS)
            return false;

        std::vector<detail::OfflineFix> softwareFixes = {
            { L"Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableTaskMgr", 0, false },
            { L"Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableRegistryTools", 0, false },
            { L"Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"EnableLUA", 1, false },
            { L"Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableCMD", 0, false },
            { L"Policies\\Microsoft\\Windows\\System", L"DisableCMD", 0, false },
            { L"Policies\\Microsoft\\Windows NT\\SystemRestore", L"DisableSR", 0, false },
            { L"Policies\\Microsoft\\Windows NT\\SystemRestore", L"DisableConfig", 0, false },
            { L"Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoDrives", 0, false },
            { L"Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoViewOnDrive", 0, false },
            { L"Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoControlPanel", 0, false },
            { L"Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoRun", 0, false },
            { L"Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoClose", 0, false },
            { L"Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoViewContextMenu", 0, false },
            { L"Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoFolderOptions", 0, false },
            { L"Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"Taskman", 0, true },
            { L"Microsoft\\Windows NT\\CurrentVersion\\Windows", L"LoadAppInit_DLLs", 0, false },
        };
        for (const auto& fix : softwareFixes) detail::ApplyOfflineFix(hSoft, fix);
        {
            HKEY hWl = nullptr;
            if (RegOpenKeyExW(hSoft, L"Microsoft\\Windows NT\\CurrentVersion\\Winlogon", 0,
                KEY_SET_VALUE, &hWl) == ERROR_SUCCESS) {
                const wchar_t* shell = L"explorer.exe";
                RegSetValueExW(hWl, L"Shell", 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(shell), (DWORD)((wcslen(shell) + 1) * sizeof(wchar_t)));
                const wchar_t* userinit = L"C:\\Windows\\system32\\userinit.exe,";
                RegSetValueExW(hWl, L"Userinit", 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(userinit), (DWORD)((wcslen(userinit) + 1) * sizeof(wchar_t)));
                RegDeleteValueW(hWl, L"LegalNoticeCaption");
                RegDeleteValueW(hWl, L"LegalNoticeText");
                RegCloseKey(hWl);
            }
        }
        detail::CleanDisallowRunAt(hSoft, L"Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer");
        detail::CleanIFEOAt(hSoft, L"Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options");

        std::vector<detail::OfflineFix> userFixes = {
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableTaskMgr", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableRegistryTools", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableLockWorkstation", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableChangePassword", 0, false },
            { L"Software\\Policies\\Microsoft\\Windows\\System", L"DisableCMD", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoRun", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoControlPanel", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoViewOnDrive", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoDrives", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoFind", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoViewContextMenu", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoFolderOptions", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoSecurityTab", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoFileMenu", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoClose", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoLogoff", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoWinKeys", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoSetTaskbar", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\ActiveDesktop", L"NoChangingWallPaper", 0, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"Hidden", 1, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"ShowSuperHidden", 1, false },
            { L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", L"ProxyEnable", 0, false },
        };
        std::wstring usersDir = winDrive + L"Users";
        detail::ForEachUserHive(usersDir, [&](HKEY hUser) {
            for (const auto& fix : userFixes) detail::ApplyOfflineFix(hUser, fix);
            detail::CleanDisallowRunAt(hUser, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer");
            });

        std::wstring systemPath = winDrive + L"Windows\\System32\\config\\SYSTEM";
        HKEY hSys = nullptr;
        if (RegLoadAppKeyW(systemPath.c_str(), &hSys, KEY_ALL_ACCESS, 0, 0) == ERROR_SUCCESS) {
            struct SvcFix { const wchar_t* name; DWORD start; };
            static const SvcFix svcFixes[] = {
                { L"WinDefend", 2 }, { L"WdNisSvc", 3 }, { L"Sense", 3 },
                { L"SecurityHealthService", 2 }, { L"wscsvc", 2 },
            };
            for (const auto& s : svcFixes) {
                detail::ApplyOfflineFix(hSys,
                    { std::wstring(L"ControlSet001\\Services\\") + s.name, L"Start", s.start, false });
            }
            RegFlushKey(hSys);
            RegCloseKey(hSys);
        }

        std::wstring hostsPath = winDrive + L"Windows\\System32\\drivers\\etc\\hosts";
        if (detail::IsHostsHijackedAt(hostsPath)) detail::WriteDefaultHosts(hostsPath);

        detail::DeleteKeyTree(hSoft, L"Policies\\Microsoft\\Windows\\Safer");
        detail::DeleteKeyTree(hSoft, L"Policies\\Microsoft\\Windows\\SrpV2");

        RegFlushKey(hSoft);
        RegCloseKey(hSoft);
        return true;
    }

    bool ClearOfflineIFEO() {
        detail::EnableOfflinePrivileges();
        std::wstring winDrive = detail::FindOfflineWindowsDrive();
        if (winDrive.empty()) return false;
        HKEY hSoft = nullptr;
        if (RegLoadAppKeyW((winDrive + L"Windows\\System32\\config\\SOFTWARE").c_str(),
            &hSoft, KEY_ALL_ACCESS, 0, 0) != ERROR_SUCCESS) return false;
        detail::CleanIFEOAt(hSoft, L"Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options");
        RegFlushKey(hSoft); RegCloseKey(hSoft);
        return true;
    }

    bool ClearOfflineDisallowRun() {
        detail::EnableOfflinePrivileges();
        std::wstring winDrive = detail::FindOfflineWindowsDrive();
        if (winDrive.empty()) return false;
        HKEY hSoft = nullptr;
        if (RegLoadAppKeyW((winDrive + L"Windows\\System32\\config\\SOFTWARE").c_str(),
            &hSoft, KEY_ALL_ACCESS, 0, 0) == ERROR_SUCCESS) {
            detail::CleanDisallowRunAt(hSoft, L"Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer");
            RegFlushKey(hSoft); RegCloseKey(hSoft);
        }
        detail::ForEachUserHive(winDrive + L"Users", [&](HKEY hUser) {
            detail::CleanDisallowRunAt(hUser, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer");
            });
        return true;
    }

    // ----- Основной отчёт -----
    std::wstring GetBestUnlockReport(bool unlock) {
        std::wstring body;
        int blockedCount = 0, unlockedCount = 0, failedCount = 0;
        bool completed = false;

        int offlineBlocked = detail::ScanOfflineWindows(body);
        if (offlineBlocked >= 0) {
            blockedCount += offlineBlocked;
            if (offlineBlocked == 0) body += L"Offline: активные блокировки не найдены.\r\n";
            if (unlock) {
                if (RepairOfflineWindows()) {
                    if (offlineBlocked > 0) { body += L"Offline: все найденные блокировки сняты.\r\n"; unlockedCount += offlineBlocked; }
                    else { body += L"Offline: выполнена профилактическая разблокировка.\r\n"; ++unlockedCount; }
                }
                else { body += L"Offline: не удалось выполнить разблокировку.\r\n"; ++failedCount; }
            }
            completed = true;
        }
        else if (unlock && RepairOfflineWindows()) {
            body += L"Offline: выполнена аварийная разблокировка.\r\n";
            blockedCount = 1; unlockedCount = 1; completed = true;
        }

        if (!completed) {
            auto restrictions = GetKnownRestrictions();
            for (const auto& r : restrictions) {
                if (!IsRestricted(r)) continue;
                ++blockedCount;
                if (!unlock) { body += L"Заблокировано: " + r.description + L"\r\n"; continue; }
                if (UnlockRestriction(r)) { body += L"Разблокировано: " + r.description + L"\r\n"; ++unlockedCount; }
                else { body += L"Ошибка: " + r.description + L"\r\n"; ++failedCount; }
            }

            PolicyLocks pl = ScanPolicyLocks();
            if (pl.srp) {
                ++blockedCount;
                if (!unlock) body += L"Заблокировано: Software Restriction Policies (SRP)\r\n";
            }
            if (pl.applocker) {
                ++blockedCount;
                if (!unlock) body += L"Заблокировано: AppLocker" +
                    std::wstring(pl.applockerEnforced ? L" (принудительно)" : L"") + L"\r\n";
            }
            if (unlock && (pl.srp || pl.applocker)) {
                UnlockPolicyLocks();
                if (pl.srp) { body += L"Разблокировано: SRP\r\n"; ++unlockedCount; }
                if (pl.applocker) { body += L"Разблокировано: AppLocker\r\n"; ++unlockedCount; }
            }

            {
                auto probes = ProbeCriticalPaths();
                for (const auto& pr : probes) {
                    if (!pr.writable) {
                        body += L"Нет записи в: " + pr.path +
                            L" (нормально без прав администратора; сброс ACL кнопкой)\r\n";
                    }
                }
            }

            bool hasDisallowRun =
                HasDisallowRunAt(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer") ||
                HasDisallowRunAt(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer");
            bool hasIFEO = HasIFEODebuggerAt(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options");
            if (hasDisallowRun) { ++blockedCount; if (!unlock) body += L"Заблокировано: DisallowRun\r\n"; }
            if (hasIFEO) { ++blockedCount; if (!unlock) body += L"Заблокировано: IFEO Debugger\r\n"; }
            if (unlock) {
                if (hasDisallowRun) {
                    if (ClearDisallowRun()) { body += L"Разблокировано: DisallowRun\r\n"; ++unlockedCount; }
                    else { body += L"Ошибка: DisallowRun\r\n"; ++failedCount; }
                }
                if (hasIFEO) {
                    if (ClearImageFileExecutionOptions()) { body += L"Разблокировано: IFEO Debugger\r\n"; ++unlockedCount; }
                    else { body += L"Ошибка: IFEO Debugger\r\n"; ++failedCount; }
                }
            }

            struct ActionItem { const wchar_t* desc; bool (*detect)(); bool (*fix)(); bool counts; };
            static const ActionItem kActions[] = {
                { L"Winlogon: подменён Shell",           detail::IsWinlogonShellHijacked,    detail::FixWinlogonShell,      true  },
                { L"Баннер входа (LegalNotice)",         detail::HasLegalNotice,             detail::ClearLegalNotice,      true  },
                { L"Ассоциации EXE/BAT/CMD",             detail::AreExeAssociationsHijacked, detail::FixExeAssociations,    true  },
                { L"Принудительный прокси",              detail::IsProxyForced,              detail::ResetProxy,            true  },
                { L"Скрытые файлы не показываются (Исправление включит показ)",
                                                         detail::AreHiddenFilesForced,       detail::RestoreHiddenFiles,    false },
                { L"AppInit_DLLs (инжекция в процессы)", detail::HasAppInitDlls,             detail::ClearAppInitDlls,      true  },
                { L"Службы Defender отключены",          detail::AreDefenderServicesDisabled,detail::RestoreDefenderServices,true },
                { L"Файл hosts содержит сторонние записи", detail::IsHostsHijacked,          detail::RestoreDefaultHosts,   false },
            };
            for (const auto& a : kActions) {
                if (!a.detect()) continue;
                if (a.counts) ++blockedCount;
                if (!unlock) { body += std::wstring(L"") + (a.counts ? L"Заблокировано: " : L"Инфо: ") + a.desc + L"\r\n"; continue; }
                if (a.fix()) {
                    body += (a.counts ? L"Разблокировано: " : L"Исправлено: ") + std::wstring(a.desc) + L"\r\n";
                    if (a.counts) ++unlockedCount;
                }
                else { body += L"Ошибка: " + std::wstring(a.desc) + L"\r\n"; if (a.counts) ++failedCount; }
            }

            if (IsBcdSafeBootEnabled()) {
                ++blockedCount;
                body += L"Заблокировано: BCD safeboot (снимается отдельной галочкой)\r\n";
            }

            if (blockedCount == 0) body += L"Активные блокировки не обнаружены.\r\n";
        }

        std::wstring header;
        header += L"Найдено блокировок: " + std::to_wstring(blockedCount) + L"\r\n";
        if (unlock) {
            header += L"Разблокировано: " + std::to_wstring(unlockedCount) + L"\r\n";
            header += L"Ошибок: " + std::to_wstring(failedCount) + L"\r\n";
        }
        header += L"\r\n";
        return header + body;
    }

    // ----- SRP / AppLocker -----
    PolicyLocks ScanPolicyLocks() {
        PolicyLocks r;
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Policies\\Microsoft\\Windows\\Safer\\CodeIdentifiers",
            0, KEY_READ, &h) == ERROR_SUCCESS) {

            bool restrictive = false;

            DWORD level = 0, sz = sizeof(level), t = 0;
            if (RegQueryValueExW(h, L"DefaultLevel", nullptr, &t,
                reinterpret_cast<LPBYTE>(&level), &sz) == ERROR_SUCCESS) {
                if (level != 262144 /*0x40000 = Unrestricted*/) restrictive = true;
            }

            const wchar_t* levels[] = { L"0", L"4096" };
            for (const wchar_t* lvl : levels) {
                HKEY hLvl = nullptr;
                if (RegOpenKeyExW(h, lvl, 0, KEY_READ, &hLvl) == ERROR_SUCCESS) {
                    DWORD subKeys = 0, values = 0;
                    if (RegQueryInfoKeyW(hLvl, nullptr, nullptr, nullptr,
                        &subKeys, nullptr, nullptr, &values,
                        nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                        if (subKeys > 0 || values > 0) restrictive = true;
                    }
                    RegCloseKey(hLvl);
                }
            }

            r.srp = restrictive;
            RegCloseKey(h);
        }

        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Policies\\Microsoft\\Windows\\SrpV2", 0, KEY_READ, &h) == ERROR_SUCCESS) {
            r.applocker = true;
            const wchar_t* cats[] = { L"Exe", L"Dll", L"Script", L"Msi", L"Appx" };
            for (const wchar_t* c : cats) {
                HKEY hc = nullptr;
                if (RegOpenKeyExW(h, c, 0, KEY_READ, &hc) == ERROR_SUCCESS) {
                    DWORD mode = 0, msz = sizeof(mode), t2 = 0;
                    if (RegQueryValueExW(hc, L"EnforcementMode", nullptr, &t2,
                        reinterpret_cast<LPBYTE>(&mode), &msz) == ERROR_SUCCESS && mode == 1) {
                        r.applockerEnforced = true;
                    }
                    RegCloseKey(hc);
                }
            }
            RegCloseKey(h);
        }
        return r;
    }

    void UnlockPolicyLocks() {
        detail::DeleteKeyTree(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Safer");
        detail::DeleteKeyTree(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\SrpV2");
    }

    // ----- NTFS ACL -----
    std::vector<AclProbe> ProbeCriticalPaths() {
        std::vector<AclProbe> out;
        std::vector<std::wstring> paths;

        wchar_t win[MAX_PATH] = {};
        if (GetWindowsDirectoryW(win, MAX_PATH)) {
            paths.push_back(win);
            paths.push_back(std::wstring(win) + L"\\System32");
        }
        wchar_t pf[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) paths.push_back(pf);
        wchar_t up[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH)) paths.push_back(up);

        for (const auto& p : paths) {
            AclProbe pr; pr.path = p; pr.writable = false;
            wchar_t tmp[MAX_PATH] = {};
            if (GetTempFileNameW(p.c_str(), L"sys", 0, tmp)) {
                pr.writable = true;
                DeleteFileW(tmp);
            }
            out.push_back(pr);
        }
        return out;
    }

    bool ResetAclOnPath(const std::wstring& path, bool recursive, std::wstring& log) {
        detail::EnablePrivilege(SE_TAKE_OWNERSHIP_NAME);
        detail::EnablePrivilege(SE_BACKUP_NAME);
        detail::EnablePrivilege(SE_RESTORE_NAME);

        std::wstring q = L"\"" + path + L"\"";
        detail::RunCapture(L"cmd.exe /c takeown /f " + q + L" /a /d y" + (recursive ? L" /r" : L""), log);
        detail::RunCapture(L"cmd.exe /c icacls " + q + L" /reset" + (recursive ? L" /t" : L"") + L" /c /q", log);
        return true;
    }

    // ----- Загрузка (boot) -----
    BootInfo ScanBoot() {
        BootInfo b;

        wchar_t sd[16] = {};
        bool inRE = detail::IsLikelyWinRE();
        std::wstring drv;
        if (inRE) drv = detail::FindOfflineWindowsDrive();
        if (drv.empty()) {
            wchar_t win[MAX_PATH] = {};
            if (GetWindowsDirectoryW(win, MAX_PATH)) {
                drv = std::wstring(win, 3);
                b.windowsDir = win;
            }
        }
        else {
            b.windowsDir = drv + L"Windows";
        }

        if (!b.windowsDir.empty()) {
            b.winloadOk =
                detail::FileExistsPath(b.windowsDir + L"\\System32\\winload.exe") ||
                detail::FileExistsPath(b.windowsDir + L"\\System32\\winload.efi");
        }
        b.biosBcdOk = detail::FileExistsPath(drv + L"Boot\\BCD");
        b.efiBcdOk = detail::FileExistsPath(drv + L"EFI\\Microsoft\\Boot\\BCD");

        std::wstring log;
        std::wstring enumCmd = (inRE && b.efiBcdOk)
            ? L"bcdedit /store \"" + drv + L"EFI\\Microsoft\\Boot\\BCD\" /enum {default}"
            : (inRE && b.biosBcdOk)
            ? L"bcdedit /store \"" + drv + L"Boot\\BCD\" /enum {default}"
            : L"bcdedit /enum {current}";
        b.bcdeditOk = detail::RunCapture(enumCmd, log);
        b.details = log;
        return b;
    }

    bool RepairBootRecords(std::wstring& log) {
        detail::EnableOfflinePrivileges();
        detail::RunCapture(L"bootrec /fixmbr", log);
        detail::RunCapture(L"bootrec /fixboot", log);
        detail::RunCapture(L"bootrec /scanos", log);

        BootInfo bi = ScanBoot();
        if (!bi.efiBcdOk && !bi.windowsDir.empty()) {
            detail::RunCapture(L"bcdboot \"" + bi.windowsDir + L"\" /l ru-ru", log);
        }
        return true;
    }

    // ----- Legacy -----
    bool IsRegistryEditorLocked() {
        DWORD value = 0;
        if (detail::ReadDwordOnline(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
            L"DisableRegistryTools", value)) return value != 0;
        return false;
    }

    bool UnlockRegistryEditor() {
        return detail::SetDwordAt(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
            L"DisableRegistryTools", 0);
    }

    bool IsTaskManagerLocked() {
        DWORD value = 0;
        if (detail::ReadDwordOnline(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
            L"DisableTaskMgr", value)) return value != 0;
        return false;
    }

    bool UnlockTaskManager() {
        return detail::SetDwordAt(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
            L"DisableTaskMgr", 0);
    }

    bool IsUACDisabled() {
        DWORD value = 1;
        if (detail::ReadDwordOnline(HKEY_LOCAL_MACHINE,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
            L"EnableLUA", value)) return value == 0;
        return false;
    }

    bool EnableUAC() {
        return detail::SetDwordAt(HKEY_LOCAL_MACHINE,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
            L"EnableLUA", 1);
    }

    // ================================================================
    // ПУБЛИЧНЫЕ ФУНКЦИИ ДЛЯ ПРОВЕРКИ DisallowRun И IFEO
    // (без зависимости от detail::OpenOnlineKey)
    // ================================================================
    bool HasDisallowRunAt(HKEY root, const std::wstring& explorerSubKey) {
        HKEY hExplorer = nullptr;
        LONG status = RegOpenKeyExW(root, explorerSubKey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hExplorer);
        if (status != ERROR_SUCCESS)
            status = RegOpenKeyExW(root, explorerSubKey.c_str(), 0, KEY_READ, &hExplorer);
        if (status != ERROR_SUCCESS) return false;

        bool found = false;
        DWORD value = 0; DWORD size = sizeof(value); DWORD type = 0;
        if (RegQueryValueExW(hExplorer, L"DisallowRun", nullptr, &type,
            reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS) {
            found = (type != REG_DWORD) || (value != 0);
        }
        if (!found) {
            HKEY hDisallowRun = nullptr;
            if (RegOpenKeyExW(hExplorer, L"DisallowRun", 0, KEY_READ, &hDisallowRun) == ERROR_SUCCESS) {
                wchar_t valueName[16384]{}; DWORD valueNameLen = 16384;
                if (RegEnumValueW(hDisallowRun, 0, valueName, &valueNameLen,
                    nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) found = true;
                RegCloseKey(hDisallowRun);
            }
        }
        RegCloseKey(hExplorer);
        return found;
    }

    bool HasIFEODebuggerAt(HKEY root, const std::wstring& ifeoSubKey) {
        HKEY hIFEO = nullptr;
        LONG status = RegOpenKeyExW(root, ifeoSubKey.c_str(), 0, KEY_ENUMERATE_SUB_KEYS | KEY_READ | KEY_WOW64_64KEY, &hIFEO);
        if (status != ERROR_SUCCESS)
            status = RegOpenKeyExW(root, ifeoSubKey.c_str(), 0, KEY_ENUMERATE_SUB_KEYS | KEY_READ, &hIFEO);
        if (status != ERROR_SUCCESS) return false;

        bool found = false;
        for (DWORD index = 0;; ++index) {
            wchar_t subKeyName[1024]{}; DWORD subKeyNameLen = 1024;
            if (RegEnumKeyExW(hIFEO, index, subKeyName, &subKeyNameLen,
                nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            HKEY hSub = nullptr;
            if (RegOpenKeyExW(hIFEO, subKeyName, 0, KEY_READ, &hSub) == ERROR_SUCCESS) {
                if (RegQueryValueExW(hSub, L"Debugger", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
                    found = true;
                RegCloseKey(hSub);
            }
            if (found) break;
        }
        RegCloseKey(hIFEO);
        return found;
    }

    std::vector<std::wstring> ScanDrivesWithWindows() {
        std::vector<std::wstring> out;
        for (wchar_t drive = L'C'; drive <= L'Z'; ++drive) {
            std::wstring root; root += drive; root += L":\\";
            std::wstring softwarePath = root + L"Windows\\System32\\config\\SOFTWARE";
            std::wstring systemPath = root + L"Windows\\System32\\config\\SYSTEM";
            if (GetFileAttributesW(softwarePath.c_str()) != INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW(systemPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                out.push_back(root);
            }
        }
        return out;
    }

    void SetOfflineDriveHint(const std::wstring& driveRoot) {
        g_offlineDriveHint = driveRoot;
        if (!g_offlineDriveHint.empty() && g_offlineDriveHint.back() != L'\\')
            g_offlineDriveHint += L'\\';
    }

    std::wstring GetOfflineDriveHint() {
        return g_offlineDriveHint;
    }

    bool IsRecoveryEnvironment() {
        return detail::IsLikelyWinRE();
    }

} // namespace UnlockTools