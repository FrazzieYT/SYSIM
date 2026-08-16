#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace StartupRegistry {

    // Types
    enum class StartupSource {
        Run,
        RunOnce,
        PoliciesRun,
        Winlogon
    };

    enum class StartupScope {
        Machine,
        User
    };

    struct StartupEntry {
        HKEY root = nullptr;
        std::wstring regPath;
        std::wstring valueName;
        std::wstring command;
        StartupSource source = StartupSource::Run;
        StartupScope scope = StartupScope::Machine;
        std::wstring backupId;
        std::wstring mountPrefix;
        bool enabled = true;
        bool isCritical = false;
        REGSAM view = KEY_WOW64_64KEY;
    };

    // Live / Online
    std::vector<StartupEntry> GetAllEntries();
    std::vector<StartupEntry> GetRunEntries();
    std::vector<StartupEntry> GetWinlogonEntries();

    // Offline
    std::vector<StartupEntry> GetOfflineEntries(
        const std::wstring& softwareMount,
        const std::vector<std::wstring>& userMounts = {}
    );

    // Operations
    bool AddEntry(
        HKEY root,
        const std::wstring& regPath,
        const std::wstring& valueName,
        const std::wstring& command,
        REGSAM view = KEY_WOW64_64KEY
    );

    bool RemoveEntry(const StartupEntry& entry);
    bool SetEntryCommand(const StartupEntry& entry, const std::wstring& command);
    bool DisableEntry(const StartupEntry& entry);
    bool EnableEntry(const StartupEntry& entry);

    // Utils
    std::wstring SourceToString(StartupSource source);
    std::wstring ScopeToString(StartupScope scope);
    std::wstring GetBackupKeyPath(const std::wstring& mountPrefix, const std::wstring& backupId);
}