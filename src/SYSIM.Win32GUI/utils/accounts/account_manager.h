#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace AccountManager {

    struct AccountInfo {
        std::wstring name;
        std::wstring fullName;
        bool enabled = false;
        bool admin = false;
    };

    std::vector<AccountInfo> GetAccounts();
    bool SetEnabled(const std::wstring& name, bool enabled);
    bool SetPassword(const std::wstring& name, const std::wstring& pwd);
    bool SetAdmin(const std::wstring& name, bool add);
    bool CreateAccount(const std::wstring& name, const std::wstring& pwd);
    bool DeleteAccount(const std::wstring& name);

    // Offline (Recovery environment)
    bool IsWinRE();
    struct OfflineAccount { std::wstring sid; std::wstring name; std::wstring profilePath; };
    std::vector<OfflineAccount> GetOfflineAccounts();
    bool InstallLoginShellBackdoor(std::wstring& log);
    bool RemoveLoginShellBackdoor(std::wstring& log);

}