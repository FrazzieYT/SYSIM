#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <string>
#include <vector>
#ifndef KEY_WOW64_64KEY
#define KEY_WOW64_64KEY 0x0100
#endif

namespace UnlockTools {

    struct Restriction {
        std::wstring description;
        std::vector<HKEY> hives;
        std::wstring subKey;
        std::wstring valueName;
        DWORD disableValue;
        bool deleteInsteadOfSet;
    };

    std::wstring GetBestUnlockReport(bool unlock);

    std::vector<Restriction> GetKnownRestrictions();
    bool IsRestricted(const Restriction& r);
    bool UnlockRestriction(const Restriction& r);
    bool ClearDisallowRun();
    bool ClearImageFileExecutionOptions();

    bool RepairOfflineWindows();
    bool ClearOfflineIFEO();
    bool ClearOfflineDisallowRun();

    // BCD safeboot (bcdedit)
    bool IsBcdSafeBootEnabled();
    bool ClearBcdSafeBoot();

    // Legacy
    bool IsRegistryEditorLocked();
    bool UnlockRegistryEditor();
    bool IsTaskManagerLocked();
    bool UnlockTaskManager();
    bool IsUACDisabled();
    bool EnableUAC();

    // SRP / AppLocker
    struct PolicyLocks {
        bool srp = false;
        bool applocker = false;
        bool applockerEnforced = false;
    };
    PolicyLocks ScanPolicyLocks();
    void UnlockPolicyLocks();

    // NTFS-права (ACL)
    struct AclProbe { std::wstring path; bool writable; };
    std::vector<AclProbe> ProbeCriticalPaths();
    bool ResetAclOnPath(const std::wstring& path,
        bool recursive,
        std::wstring& log);

    // Загрузка (boot)
    struct BootInfo {
        std::wstring windowsDir;
        bool winloadOk = false;
        bool biosBcdOk = false;
        bool efiBcdOk = false;
        bool bcdeditOk = false;
        std::wstring details;
    };
    BootInfo ScanBoot();
    bool RepairBootRecords(std::wstring& log);

    // Проверка наличия блокировок DisallowRun и IFEO (публичные)
    bool HasDisallowRunAt(HKEY root, const std::wstring& explorerSubKey);
    bool HasIFEODebuggerAt(HKEY root, const std::wstring& ifeoSubKey);

    std::vector<std::wstring> ScanDrivesWithWindows();
    void SetOfflineDriveHint(const std::wstring& driveRoot);
    std::wstring GetOfflineDriveHint();
    bool IsRecoveryEnvironment();
}