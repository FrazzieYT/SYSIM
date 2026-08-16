#include "startup_registry.h"
#include "registry_editor.h"
#include <cstring>
#include <cwchar>

namespace StartupRegistry {

    // Winlogon values
    static bool IsWinlogonValue(const std::wstring& name) {
        return _wcsicmp(name.c_str(), L"Userinit") == 0 ||
            _wcsicmp(name.c_str(), L"Shell") == 0 ||
            _wcsicmp(name.c_str(), L"Taskman") == 0 ||
            _wcsicmp(name.c_str(), L"AppSetup") == 0;
    }

    // Userinit & Shell: deletion breaks Windows login
    static bool IsCriticalWinlogonValue(const std::wstring& name) {
        return _wcsicmp(name.c_str(), L"Userinit") == 0 ||
            _wcsicmp(name.c_str(), L"Shell") == 0;
    }

    // Description of keys to collect
    struct KeyDef {
        HKEY root;
        std::wstring regPath;
        StartupSource source;
        StartupScope scope;
        std::wstring backupId;
        bool isWinlogon;
        REGSAM view;
    };

    static std::vector<KeyDef> GetLiveKeyDefs() {
        return {
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
              StartupSource::Run, StartupScope::Machine, L"HKLM_Run", false, KEY_WOW64_64KEY },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
              StartupSource::Run, StartupScope::Machine, L"HKLM_Run_Wow64", false, KEY_WOW64_32KEY },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
              StartupSource::RunOnce, StartupScope::Machine, L"HKLM_RunOnce", false, KEY_WOW64_64KEY },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
              StartupSource::RunOnce, StartupScope::Machine, L"HKLM_RunOnce_Wow64", false, KEY_WOW64_32KEY },

            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
              StartupSource::Run, StartupScope::User, L"HKCU_Run", false, KEY_WOW64_64KEY },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
              StartupSource::Run, StartupScope::User, L"HKCU_Run_Wow64", false, KEY_WOW64_32KEY },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
              StartupSource::RunOnce, StartupScope::User, L"HKCU_RunOnce", false, KEY_WOW64_64KEY },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
              StartupSource::RunOnce, StartupScope::User, L"HKCU_RunOnce_Wow64", false, KEY_WOW64_32KEY },

            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run",
              StartupSource::PoliciesRun, StartupScope::Machine, L"HKLM_PoliciesRun", false, KEY_WOW64_64KEY },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run",
              StartupSource::PoliciesRun, StartupScope::User, L"HKCU_PoliciesRun", false, KEY_WOW64_64KEY },

            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
              StartupSource::Winlogon, StartupScope::Machine, L"HKLM_Winlogon", true, KEY_WOW64_64KEY },
        };
    }

    // Offline: SOFTWARE hive is mounted into HKLM as <mount>
    static std::vector<KeyDef> GetOfflineSoftwareKeyDefs(const std::wstring& mount) {
        std::wstring p = mount + L"\\";
        return {
            { HKEY_LOCAL_MACHINE, p + L"Microsoft\\Windows\\CurrentVersion\\Run",
              StartupSource::Run, StartupScope::Machine, L"HKLM_Run", false, 0 },
            { HKEY_LOCAL_MACHINE, p + L"Microsoft\\Windows\\CurrentVersion\\RunOnce",
              StartupSource::RunOnce, StartupScope::Machine, L"HKLM_RunOnce", false, 0 },
            { HKEY_LOCAL_MACHINE, p + L"Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Run",
              StartupSource::Run, StartupScope::Machine, L"HKLM_Run_Wow64", false, 0 },
            { HKEY_LOCAL_MACHINE, p + L"Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
              StartupSource::RunOnce, StartupScope::Machine, L"HKLM_RunOnce_Wow64", false, 0 },
            { HKEY_LOCAL_MACHINE, p + L"Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run",
              StartupSource::PoliciesRun, StartupScope::Machine, L"HKLM_PoliciesRun", false, 0 },
            { HKEY_LOCAL_MACHINE, p + L"Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
              StartupSource::Winlogon, StartupScope::Machine, L"HKLM_Winlogon", true, 0 },
        };
    }

    // Offline: user hive is mounted into HKU as <mount>
    static std::vector<KeyDef> GetOfflineUserKeyDefs(const std::wstring& mount) {
        std::wstring p = mount + L"\\";
        return {
            { HKEY_USERS, p + L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
              StartupSource::Run, StartupScope::User, L"HKCU_Run", false, 0 },
            { HKEY_USERS, p + L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
              StartupSource::RunOnce, StartupScope::User, L"HKCU_RunOnce", false, 0 },
            { HKEY_USERS, p + L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Run",
              StartupSource::Run, StartupScope::User, L"HKCU_Run_Wow64", false, 0 },
            { HKEY_USERS, p + L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
              StartupSource::RunOnce, StartupScope::User, L"HKCU_RunOnce_Wow64", false, 0 },
            { HKEY_USERS, p + L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run",
              StartupSource::PoliciesRun, StartupScope::User, L"HKCU_PoliciesRun", false, 0 },
        };
    }

    // Backup
    std::wstring GetBackupKeyPath(const std::wstring& mountPrefix, const std::wstring& backupId) {
        std::wstring base = mountPrefix.empty() ? L"" : mountPrefix + L"\\";
        return base + L"Software\\SYSIM\\DisabledStartup\\" + backupId;
    }

    // Record collection
    static void CollectEntries(
        const std::vector<KeyDef>& defs,
        const std::wstring& mountPrefix,
        std::vector<StartupEntry>& out
    ) {
        for (const auto& def : defs) {
            HANDLE hKey = RegistryEditor::OpenKey(def.root, def.regPath, KEY_READ | def.view);
            if (!hKey) continue;

            auto values = RegistryEditor::EnumValues(hKey);
            RegistryEditor::CloseKey(hKey);

            for (const auto& val : values) {
                if (val.name.empty()) continue;

                if (def.isWinlogon && !IsWinlogonValue(val.name)) continue;

                StartupEntry e;
                e.root = def.root;
                e.regPath = def.regPath;
                e.valueName = val.name;
                e.command = RegistryEditor::ValueDataToDisplay(val);
                e.source = def.source;
                e.scope = def.scope;
                e.backupId = def.backupId;
                e.mountPrefix = mountPrefix;
                e.enabled = true;
                e.isCritical = def.isWinlogon && IsCriticalWinlogonValue(val.name);
                e.view = def.view;

                out.push_back(e);
            }
        }
    }

    static void CollectDisabledEntries(
        const std::vector<KeyDef>& defs,
        const std::wstring& mountPrefix,
        std::vector<StartupEntry>& out
    ) {
        for (const auto& def : defs) {
            if (def.isWinlogon) continue;

            std::wstring backupPath = GetBackupKeyPath(mountPrefix, def.backupId);
            HANDLE hBackup = RegistryEditor::OpenKey(def.root, backupPath, KEY_READ | def.view);
            if (!hBackup) continue;

            auto values = RegistryEditor::EnumValues(hBackup);
            RegistryEditor::CloseKey(hBackup);

            for (const auto& val : values) {
                if (val.name.empty()) continue;

                StartupEntry e;
                e.root = def.root;
                e.regPath = def.regPath;
                e.valueName = val.name;
                e.command = RegistryEditor::ValueDataToDisplay(val);
                e.source = def.source;
                e.scope = def.scope;
                e.backupId = def.backupId;
                e.mountPrefix = mountPrefix;
                e.enabled = false;
                e.isCritical = false;
                e.view = def.view;

                out.push_back(e);
            }
        }
    }

    // Public collection functions
    std::vector<StartupEntry> GetAllEntries() {
        std::vector<StartupEntry> result;
        auto defs = GetLiveKeyDefs();
        CollectEntries(defs, L"", result);
        CollectDisabledEntries(defs, L"", result);
        return result;
    }

    std::vector<StartupEntry> GetRunEntries() {
        auto all = GetAllEntries();
        std::vector<StartupEntry> result;
        for (const auto& e : all) {
            if (e.source != StartupSource::Winlogon) {
                result.push_back(e);
            }
        }
        return result;
    }

    std::vector<StartupEntry> GetWinlogonEntries() {
        auto all = GetAllEntries();
        std::vector<StartupEntry> result;
        for (const auto& e : all) {
            if (e.source == StartupSource::Winlogon) {
                result.push_back(e);
            }
        }
        return result;
    }

    std::vector<StartupEntry> GetOfflineEntries(
        const std::wstring& softwareMount,
        const std::vector<std::wstring>& userMounts
    ) {
        std::vector<StartupEntry> result;

        if (!softwareMount.empty()) {
            auto defs = GetOfflineSoftwareKeyDefs(softwareMount);
            CollectEntries(defs, softwareMount, result);
            CollectDisabledEntries(defs, softwareMount, result);
        }

        for (const auto& userMount : userMounts) {
            if (userMount.empty()) continue;
            auto defs = GetOfflineUserKeyDefs(userMount);
            CollectEntries(defs, userMount, result);
            CollectDisabledEntries(defs, userMount, result);
        }

        return result;
    }

    // Operations
    bool AddEntry(
        HKEY root,
        const std::wstring& regPath,
        const std::wstring& valueName,
        const std::wstring& command,
        REGSAM view
    ) {
        HANDLE hKey = nullptr;

        if (!RegistryEditor::CreateKey(root, regPath, hKey)) {
            hKey = RegistryEditor::OpenKey(root, regPath, KEY_SET_VALUE | view);
            if (!hKey) return false;
        }

        RegistryEditor::RegValue val;
        val.name = valueName;
        val.type = RegistryEditor::RegValueType::String;

        std::vector<BYTE> data((command.size() + 1) * sizeof(wchar_t));
        memcpy(data.data(), command.c_str(), (command.size() + 1) * sizeof(wchar_t));
        val.data = data;

        bool ok = RegistryEditor::WriteValue(hKey, val);
        RegistryEditor::CloseKey(hKey);
        return ok;
    }

    bool RemoveEntry(const StartupEntry& entry) {
        if (entry.isCritical) return false;

        if (entry.enabled) {
            HANDLE hKey = RegistryEditor::OpenKey(entry.root, entry.regPath, KEY_SET_VALUE | entry.view);
            if (!hKey) return false;
            bool ok = RegistryEditor::DeleteValue(hKey, entry.valueName);
            RegistryEditor::CloseKey(hKey);
            return ok;
        }
        else {
            std::wstring backupPath = GetBackupKeyPath(entry.mountPrefix, entry.backupId);
            HANDLE hBackup = RegistryEditor::OpenKey(entry.root, backupPath, KEY_SET_VALUE | entry.view);
            if (!hBackup) return false;
            bool ok = RegistryEditor::DeleteValue(hBackup, entry.valueName);
            RegistryEditor::CloseKey(hBackup);
            return ok;
        }
    }

    bool SetEntryCommand(const StartupEntry& entry, const std::wstring& command) {
        HANDLE hKey = RegistryEditor::OpenKey(entry.root, entry.regPath, KEY_READ | KEY_SET_VALUE | entry.view);
        if (!hKey) return false;

        auto val = RegistryEditor::ReadValue(hKey, entry.valueName);

        RegistryEditor::RegValue newVal = val;

        if (val.type == RegistryEditor::RegValueType::String ||
            val.type == RegistryEditor::RegValueType::ExpandString) {
            std::vector<BYTE> data((command.size() + 1) * sizeof(wchar_t));
            memcpy(data.data(), command.c_str(), (command.size() + 1) * sizeof(wchar_t));
            newVal.data = data;
        }
        else {
            newVal.type = RegistryEditor::RegValueType::String;
            std::vector<BYTE> data((command.size() + 1) * sizeof(wchar_t));
            memcpy(data.data(), command.c_str(), (command.size() + 1) * sizeof(wchar_t));
            newVal.data = data;
        }

        bool ok = RegistryEditor::WriteValue(hKey, newVal);
        RegistryEditor::CloseKey(hKey);
        return ok;
    }

    bool DisableEntry(const StartupEntry& entry) {
        if (entry.isCritical || !entry.enabled) return false;

        HANDLE hSrc = RegistryEditor::OpenKey(entry.root, entry.regPath, KEY_READ | KEY_SET_VALUE | entry.view);
        if (!hSrc) return false;

        auto val = RegistryEditor::ReadValue(hSrc, entry.valueName);
        if (val.name.empty()) {
            RegistryEditor::CloseKey(hSrc);
            return false;
        }

        std::wstring backupPath = GetBackupKeyPath(entry.mountPrefix, entry.backupId);
        HANDLE hBackup = nullptr;

        if (!RegistryEditor::CreateKey(entry.root, backupPath, hBackup)) {
            hBackup = RegistryEditor::OpenKey(entry.root, backupPath, KEY_SET_VALUE | entry.view);
            if (!hBackup) {
                RegistryEditor::CloseKey(hSrc);
                return false;
            }
        }

        bool ok = RegistryEditor::WriteValue(hBackup, val);
        RegistryEditor::CloseKey(hBackup);

        if (!ok) {
            RegistryEditor::CloseKey(hSrc);
            return false;
        }

        ok = RegistryEditor::DeleteValue(hSrc, entry.valueName);
        RegistryEditor::CloseKey(hSrc);
        return ok;
    }

    bool EnableEntry(const StartupEntry& entry) {
        if (entry.enabled) return false;

        std::wstring backupPath = GetBackupKeyPath(entry.mountPrefix, entry.backupId);
        HANDLE hBackup = RegistryEditor::OpenKey(entry.root, backupPath, KEY_READ | KEY_SET_VALUE | entry.view);
        if (!hBackup) return false;

        auto val = RegistryEditor::ReadValue(hBackup, entry.valueName);
        if (val.name.empty()) {
            RegistryEditor::CloseKey(hBackup);
            return false;
        }

        HANDLE hOrig = RegistryEditor::OpenKey(entry.root, entry.regPath, KEY_SET_VALUE | entry.view);
        if (!hOrig) {
            RegistryEditor::CloseKey(hBackup);
            return false;
        }

        bool ok = RegistryEditor::WriteValue(hOrig, val);
        RegistryEditor::CloseKey(hOrig);

        if (!ok) {
            RegistryEditor::CloseKey(hBackup);
            return false;
        }

        ok = RegistryEditor::DeleteValue(hBackup, entry.valueName);
        RegistryEditor::CloseKey(hBackup);
        return ok;
    }

    // Utils
    std::wstring SourceToString(StartupSource source) {
        switch (source) {
        case StartupSource::Run:         return L"Run";
        case StartupSource::RunOnce:     return L"RunOnce";
        case StartupSource::PoliciesRun: return L"Policies";
        case StartupSource::Winlogon:    return L"Winlogon";
        default:                         return L"Unknown";
        }
    }

    std::wstring ScopeToString(StartupScope scope) {
        switch (scope) {
        case StartupScope::Machine: return L"HKLM";
        case StartupScope::User:    return L"HKCU";
        default:                    return L"Unknown";
        }
    }
}