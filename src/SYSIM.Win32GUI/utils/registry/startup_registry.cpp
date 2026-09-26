#include "startup_registry.h"
#include "registry_editor.h"
#include <cstring>
#include <cwchar>

namespace StartupRegistry {

    // Winlogon values
    static bool IsWinlogonValue(const std::wstring& name) {
        return _wcsicmp(name.c_str(), L"Shell") == 0 ||
            _wcsicmp(name.c_str(), L"Userinit") == 0 ||
            _wcsicmp(name.c_str(), L"CmdLine") == 0 ||
            _wcsicmp(name.c_str(), L"Taskman") == 0 ||
            _wcsicmp(name.c_str(), L"AppSetup") == 0 ||
            _wcsicmp(name.c_str(), L"Notify") == 0 ||
            _wcsicmp(name.c_str(), L"AutoLogon") == 0 ||
            _wcsicmp(name.c_str(), L"DefaultUserName") == 0 ||
            _wcsicmp(name.c_str(), L"DefaultPassword") == 0 ||
            _wcsicmp(name.c_str(), L"SetupType") == 0 ||
            _wcsicmp(name.c_str(), L"EnableCursorSuppression") == 0;
    }

    static bool IsBootExecuteValue(const std::wstring& name) {
        return _wcsicmp(name.c_str(), L"BootExecute") == 0;
    }

    static bool IsAppInitValue(const std::wstring& name) {
        return _wcsicmp(name.c_str(), L"AppInit_DLLs") == 0 ||
            _wcsicmp(name.c_str(), L"LoadAppInit_DLLs") == 0 ||
            _wcsicmp(name.c_str(), L"RequireSignedAppInit_DLLs") == 0;
    }

    // Userinit & Shell: deletion breaks Windows login
    bool IsCriticalWinlogonValue(const std::wstring& name) {
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
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
              StartupSource::RunOnce, StartupScope::Machine, L"HKLM_RunOnce", false, KEY_WOW64_64KEY },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx",
              StartupSource::RunOnceEx, StartupScope::Machine, L"HKLM_RunOnceEx", false, KEY_WOW64_64KEY },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServices",
              StartupSource::RunServices, StartupScope::Machine, L"HKLM_RunServices", false, KEY_WOW64_64KEY },
            { HKEY_LOCAL_MACHINE, L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Run",
              StartupSource::Wow64Run, StartupScope::Machine, L"HKLM_Run_Wow64", false, KEY_WOW64_32KEY },
            { HKEY_LOCAL_MACHINE, L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
              StartupSource::Wow64RunOnce, StartupScope::Machine, L"HKLM_RunOnce_Wow64", false, KEY_WOW64_32KEY },
            { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Active Setup\\Installed Components",
              StartupSource::ActiveSetup, StartupScope::Machine, L"HKLM_ActiveSetup", false, KEY_WOW64_64KEY },

            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
              StartupSource::Run, StartupScope::User, L"HKCU_Run", false, KEY_WOW64_64KEY },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
              StartupSource::RunOnce, StartupScope::User, L"HKCU_RunOnce", false, KEY_WOW64_64KEY },

            { HKEY_USERS, L".DEFAULT\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
              StartupSource::DefaultProfileRun, StartupScope::User, L"HKU_Default_Run", false, KEY_WOW64_64KEY },
            { HKEY_USERS, L".DEFAULT\\Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
              StartupSource::DefaultProfileRunOnce, StartupScope::User, L"HKU_Default_RunOnce", false, KEY_WOW64_64KEY },

            { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
              StartupSource::Winlogon, StartupScope::Machine, L"HKLM_Winlogon", true, KEY_WOW64_64KEY },
            { HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager",
              StartupSource::BootExecute, StartupScope::Machine, L"HKLM_BootExecute", false, KEY_WOW64_64KEY },
            { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
              StartupSource::AppInitDlls, StartupScope::Machine, L"HKLM_AppInit", false, KEY_WOW64_64KEY },
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
            { HKEY_LOCAL_MACHINE, p + L"Microsoft\\Windows\\CurrentVersion\\RunOnceEx",
              StartupSource::RunOnceEx, StartupScope::Machine, L"HKLM_RunOnceEx", false, 0 },
            { HKEY_LOCAL_MACHINE, p + L"Microsoft\\Windows\\CurrentVersion\\RunServices",
              StartupSource::RunServices, StartupScope::Machine, L"HKLM_RunServices", false, 0 },
            { HKEY_LOCAL_MACHINE, p + L"Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Run",
              StartupSource::Wow64Run, StartupScope::Machine, L"HKLM_Run_Wow64", false, 0 },
            { HKEY_LOCAL_MACHINE, p + L"Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
              StartupSource::Wow64RunOnce, StartupScope::Machine, L"HKLM_RunOnce_Wow64", false, 0 },
            { HKEY_LOCAL_MACHINE, p + L"Microsoft\\Active Setup\\Installed Components",
              StartupSource::ActiveSetup, StartupScope::Machine, L"HKLM_ActiveSetup", false, 0 },
            { HKEY_LOCAL_MACHINE, p + L"Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
              StartupSource::Winlogon, StartupScope::Machine, L"HKLM_Winlogon", true, 0 },
            { HKEY_LOCAL_MACHINE, p + L"System\\CurrentControlSet\\Control\\Session Manager",
              StartupSource::BootExecute, StartupScope::Machine, L"HKLM_BootExecute", false, 0 },
            { HKEY_LOCAL_MACHINE, p + L"Microsoft\\Windows NT\\CurrentVersion\\Windows",
              StartupSource::AppInitDlls, StartupScope::Machine, L"HKLM_AppInit", false, 0 },
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
            { HKEY_USERS, p + L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx",
              StartupSource::RunOnceEx, StartupScope::User, L"HKCU_RunOnceEx", false, 0 },
            { HKEY_USERS, p + L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServices",
              StartupSource::RunServices, StartupScope::User, L"HKCU_RunServices", false, 0 },
            { HKEY_USERS, p + L"Software\\Microsoft\\Active Setup\\Installed Components",
              StartupSource::ActiveSetup, StartupScope::User, L"HKCU_ActiveSetup", false, 0 },
            { HKEY_USERS, p + L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
              StartupSource::Winlogon, StartupScope::User, L"HKCU_Winlogon", true, 0 },
            { HKEY_USERS, p + L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
              StartupSource::AppInitDlls, StartupScope::User, L"HKCU_AppInit", false, 0 },
        };
    }

    // Backup
    std::wstring GetBackupKeyPath(const std::wstring& mountPrefix, const std::wstring& backupId) {
        std::wstring base = mountPrefix.empty() ? L"" : mountPrefix + L"\\";
        return base + L"Software\\SYSIM\\DisabledStartup\\" + backupId;
    }

    // Record collection
    static void CollectActiveSetupEntries(
        const KeyDef& def,
        const std::wstring& mountPrefix,
        std::vector<StartupEntry>& out
    ) {
        HANDLE hKey = RegistryEditor::OpenKey(def.root, def.regPath, KEY_READ | def.view);
        if (!hKey) return;

        auto subkeys = RegistryEditor::EnumSubKeys(hKey);
        RegistryEditor::CloseKey(hKey);

        for (const auto& subkey : subkeys) {
            std::wstring fullKey = def.regPath + L"\\" + subkey;
            HANDLE hItem = RegistryEditor::OpenKey(def.root, fullKey, KEY_READ | def.view);
            if (!hItem) continue;

            auto stub = RegistryEditor::ReadValue(hItem, L"StubPath");
            RegistryEditor::CloseKey(hItem);
            if (stub.name.empty()) continue;

            StartupEntry e;
            e.root = def.root;
            e.regPath = fullKey;
            e.valueName = subkey;
            e.command = RegistryEditor::ValueDataToDisplay(stub);
            e.source = def.source;
            e.scope = def.scope;
            e.backupId = def.backupId;
            e.mountPrefix = mountPrefix;
            e.enabled = true;
            e.isCritical = false;
            e.view = def.view;
            out.push_back(e);
        }
    }

    static void CollectWinlogonNotifyEntries(
        const KeyDef& def,
        const std::wstring& mountPrefix,
        std::vector<StartupEntry>& out
    ) {
        std::wstring notifyPath = def.regPath + L"\\Notify";
        HANDLE hNotify = RegistryEditor::OpenKey(def.root, notifyPath, KEY_READ | def.view);
        if (!hNotify) return;

        auto subkeys = RegistryEditor::EnumSubKeys(hNotify);
        RegistryEditor::CloseKey(hNotify);

        for (const auto& subkey : subkeys) {
            std::wstring fullKey = notifyPath + L"\\" + subkey;
            HANDLE hItem = RegistryEditor::OpenKey(def.root, fullKey, KEY_READ | def.view);
            if (!hItem) continue;

            auto values = RegistryEditor::EnumValues(hItem);
            RegistryEditor::CloseKey(hItem);

            for (const auto& val : values) {
                if (val.name.empty()) continue;
                StartupEntry e;
                e.root = def.root;
                e.regPath = fullKey;
                e.valueName = L"Notify\\" + subkey + L"\\" + val.name;
                e.command = RegistryEditor::ValueDataToDisplay(val);
                e.source = def.source;
                e.scope = def.scope;
                e.backupId = def.backupId;
                e.mountPrefix = mountPrefix;
                e.enabled = true;
                e.isCritical = false;
                e.view = def.view;
                out.push_back(e);
            }
        }
    }

    static void CollectSpecialSetupEntries(
        const std::wstring& mountPrefix,
        std::vector<StartupEntry>& out,
        HKEY root,
        const std::wstring& regPath,
        REGSAM view,
        StartupScope scope,
        const std::wstring& backupId
    ) {
        HANDLE hKey = RegistryEditor::OpenKey(root, regPath, KEY_READ | view);
        if (!hKey) return;

        auto values = RegistryEditor::EnumValues(hKey);
        RegistryEditor::CloseKey(hKey);

        for (const auto& val : values) {
            if (val.name.empty()) continue;
            if (_wcsicmp(val.name.c_str(), L"CmdLine") != 0 &&
                _wcsicmp(val.name.c_str(), L"SetupType") != 0 &&
                _wcsicmp(val.name.c_str(), L"EnableCursorSuppression") != 0) {
                continue;
            }

            StartupEntry e;
            e.root = root;
            e.regPath = regPath;
            e.valueName = val.name;
            e.command = RegistryEditor::ValueDataToDisplay(val);
            e.source = StartupSource::AppInitDlls;
            e.scope = scope;
            e.backupId = backupId;
            e.mountPrefix = mountPrefix;
            e.enabled = true;
            e.isCritical = false;
            e.view = view;
            out.push_back(e);
        }
    }

    static void CollectEntries(
        const std::vector<KeyDef>& defs,
        const std::wstring& mountPrefix,
        std::vector<StartupEntry>& out
    ) {
        CollectSpecialSetupEntries(mountPrefix, out, HKEY_LOCAL_MACHINE,
            L"SYSTEM\\Setup", KEY_WOW64_64KEY, StartupScope::Machine, L"HKLM_Setup");
        CollectSpecialSetupEntries(mountPrefix, out, HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
            KEY_WOW64_64KEY, StartupScope::Machine, L"HKLM_PoliciesSystem");

        for (const auto& def : defs) {
            if (def.source == StartupSource::ActiveSetup) {
                CollectActiveSetupEntries(def, mountPrefix, out);
                continue;
            }

            if (def.isWinlogon) {
                HANDLE hKey = RegistryEditor::OpenKey(def.root, def.regPath, KEY_READ | def.view);
                if (hKey) {
                    auto values = RegistryEditor::EnumValues(hKey);
                    RegistryEditor::CloseKey(hKey);

                    for (const auto& val : values) {
                        if (val.name.empty() || !IsWinlogonValue(val.name)) continue;
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
                        e.isCritical = IsCriticalWinlogonValue(val.name);
                        e.view = def.view;
                        out.push_back(e);
                    }
                }

                CollectWinlogonNotifyEntries(def, mountPrefix, out);
                continue;
            }

            if (def.source == StartupSource::BootExecute || def.source == StartupSource::AppInitDlls) {
                HANDLE hKey = RegistryEditor::OpenKey(def.root, def.regPath, KEY_READ | def.view);
                if (!hKey) continue;

                auto values = RegistryEditor::EnumValues(hKey);
                RegistryEditor::CloseKey(hKey);

                for (const auto& val : values) {
                    if (val.name.empty()) continue;
                    bool isTarget = (def.source == StartupSource::BootExecute && IsBootExecuteValue(val.name)) ||
                        (def.source == StartupSource::AppInitDlls && IsAppInitValue(val.name));
                    if (!isTarget) continue;

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
                    e.isCritical = false;
                    e.view = def.view;
                    out.push_back(e);
                }
                continue;
            }

            HANDLE hKey = RegistryEditor::OpenKey(def.root, def.regPath, KEY_READ | def.view);
            if (!hKey) continue;

            auto values = RegistryEditor::EnumValues(hKey);
            RegistryEditor::CloseKey(hKey);

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
                e.enabled = true;
                e.isCritical = false;
                e.view = def.view;

                out.push_back(e);
            }
        }
    }

    static void AppendMissingAppInitFallbacks(
        const std::wstring& mountPrefix,
        std::vector<StartupEntry>& out
    ) {
        auto exists = [&](const std::wstring& regPath, const std::wstring& valueName) {
            for (const auto& e : out) {
                if (e.source == StartupSource::AppInitDlls &&
                    e.regPath == regPath &&
                    e.valueName == valueName) {
                    return true;
                }
            }
            return false;
        };

        auto addFallback = [&](const std::wstring& regPath,
            const std::wstring& valueName,
            const std::wstring& command,
            HKEY root,
            REGSAM view
        ) {
            if (exists(regPath, valueName)) return;

            StartupEntry e;
            e.root = root;
            e.regPath = regPath;
            e.valueName = valueName;
            e.command = command;
            e.source = StartupSource::AppInitDlls;
            e.scope = StartupScope::Machine;
            e.backupId.clear();
            e.mountPrefix = mountPrefix;
            e.enabled = true;
            e.isCritical = false;
            e.view = view;
            out.push_back(e);
        };

        addFallback(L"SYSTEM\\Setup", L"CmdLine", L"", HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY);
        addFallback(L"SYSTEM\\Setup", L"SetupType", L"0", HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY);
        addFallback(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
            L"EnableCursorSuppression", L"0", HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY);
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
        AppendMissingAppInitFallbacks(L"", result);
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
            AppendMissingAppInitFallbacks(softwareMount, result);
            CollectDisabledEntries(defs, softwareMount, result);
        }

        for (const auto& userMount : userMounts) {
            if (userMount.empty()) continue;
            auto defs = GetOfflineUserKeyDefs(userMount);
            CollectEntries(defs, userMount, result);
            AppendMissingAppInitFallbacks(userMount, result);
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
        if (entry.isCritical) {
            if (entry.source == StartupSource::Winlogon &&
                (IsCriticalWinlogonValue(entry.valueName) ||
                 _wcsicmp(entry.valueName.c_str(), L"Shell") == 0 ||
                 _wcsicmp(entry.valueName.c_str(), L"Userinit") == 0)) {
                return RestoreWinlogonSafeDefaults(entry.root, entry.regPath, entry.view);
            }
            return false;
        }

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

    bool ClearEntryCommand(const StartupEntry& entry) {
        if (entry.source == StartupSource::Winlogon &&
            (IsCriticalWinlogonValue(entry.valueName) ||
             _wcsicmp(entry.valueName.c_str(), L"Shell") == 0 ||
             _wcsicmp(entry.valueName.c_str(), L"Userinit") == 0)) {
            return RestoreWinlogonSafeDefaults(entry.root, entry.regPath, entry.view);
        }

        HANDLE hKey = RegistryEditor::OpenKey(entry.root, entry.regPath, KEY_READ | KEY_SET_VALUE | entry.view);
        if (!hKey) return false;

        RegistryEditor::RegValue val;
        val.name = entry.valueName;
        val.type = RegistryEditor::RegValueType::String;
        std::wstring emptyString;
        std::vector<BYTE> data((emptyString.size() + 1) * sizeof(wchar_t));
        if (!data.empty()) {
            wchar_t terminator = L'\0';
            memcpy(data.data(), &terminator, sizeof(wchar_t));
        }
        val.data = data;

        bool ok = RegistryEditor::WriteValue(hKey, val);
        RegistryEditor::CloseKey(hKey);
        return ok;
    }

    bool RestoreWinlogonSafeDefaults(HKEY root, const std::wstring& regPath, REGSAM view) {
        HANDLE hKey = RegistryEditor::OpenKey(root, regPath, KEY_SET_VALUE | view);
        if (!hKey) return false;

        bool ok = true;

        auto writeString = [&](const wchar_t* valueName, const wchar_t* value) {
            RegistryEditor::RegValue val;
            val.name = valueName;
            val.type = RegistryEditor::RegValueType::String;
            std::vector<BYTE> data((wcslen(value) + 1) * sizeof(wchar_t));
            memcpy(data.data(), value, data.size());
            val.data = data;
            if (!RegistryEditor::WriteValue(hKey, val)) ok = false;
        };

        writeString(L"Shell", L"explorer.exe");
        writeString(L"Userinit", L"C:\\Windows\\system32\\userinit.exe,");

        RegistryEditor::CloseKey(hKey);
        return ok;
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
        case StartupSource::Run:                    return L"Run";
        case StartupSource::RunOnce:                return L"RunOnce";
        case StartupSource::RunOnceEx:              return L"RunOnceEx";
        case StartupSource::RunServices:            return L"RunServices";
        case StartupSource::PoliciesRun:            return L"Policies";
        case StartupSource::Wow64Run:               return L"Run (WOW64)";
        case StartupSource::Wow64RunOnce:           return L"RunOnce (WOW64)";
        case StartupSource::DefaultProfileRun:      return L"Run (.DEFAULT)";
        case StartupSource::DefaultProfileRunOnce:  return L"RunOnce (.DEFAULT)";
        case StartupSource::ActiveSetup:            return L"Active Setup";
        case StartupSource::Winlogon:               return L"Winlogon";
        case StartupSource::BootExecute:            return L"BootExecute";
        case StartupSource::AppInitDlls:            return L"AppInit_DLLs";
        default:                                  return L"Unknown";
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