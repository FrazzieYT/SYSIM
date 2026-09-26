#include "taskmgr.h"
#include "core/globals.h"
#include "core/app.h"
#include "ui/widgets.h"
#include "features/explorer.h"
#include "../../WinCtrl/include/Process.h"
#include "../../WinCtrl/include/Scanner.h"
#include "../../WinCtrl/include/System.h"
#include "../../WinCtrl/include/Registry.h"
#include "utils/registry/registry_editor.h"
#include "utils/registry/startup_registry.h"
#include "utils/process/service_manager.h"
#include "utils/process/offline_service_manager.h"
#include "utils/unlock/unlock_tools.h"
#include "utils/startup/startup_edit_dialog.h"
#include "utils/startup/service_edit_dialog.h"
#include <string>
#include <vector>
#include <algorithm>
#include <commdlg.h>
#include <unordered_map>
#include <set>
#include <functional>
#include <psapi.h>
#include <tlhelp32.h>
#include <winsvc.h>
#include <atomic>
#include <process.h>
#include <shellapi.h>
#include <memory>
#include <commctrl.h>
#include <cwctype>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "comctl32.lib")

#define TASKMGR_SHOW_RECOVERY_MESSAGE 1
#define TASKMGR_AUTO_REFRESH_INTERVAL_MS 1000

using namespace Gdiplus;
using SigStatus = WinCtrl::Scanner::ScanResult::SigStatus;

// Context menu item IDs
enum : int {
    IDM_PROC_TERMINATE = 1001,
    IDM_PROC_TERMINATE_TREE,
    IDM_PROC_SUSPEND,
    IDM_PROC_RESUME,
    IDM_PROC_CLEAR_CRITICAL,
    IDM_PROC_PRIORITY_REALTIME,
    IDM_PROC_PRIORITY_HIGH,
    IDM_PROC_PRIORITY_ABOVE_NORMAL,
    IDM_PROC_PRIORITY_NORMAL,
    IDM_PROC_PRIORITY_BELOW_NORMAL,
    IDM_PROC_PRIORITY_LOW,
    IDM_PROC_OPEN_LOCATION,
    IDM_PROC_INJECT_DLL,
    IDM_PROC_COPY_PID,
    IDM_PROC_COPY_NAME,
    IDM_PROC_COPY_PATH,

    IDM_SVC_START = 1100,
    IDM_SVC_STOP,
    IDM_SVC_RESTART,
    IDM_SVC_AUTO,
    IDM_SVC_MANUAL,
    IDM_SVC_DISABLED,
    IDM_SVC_EDIT = 1110,

    IDM_STARTUP_OPEN_LOC = 1200,
    IDM_STARTUP_DISABLE,
    IDM_STARTUP_ENABLE,
    IDM_STARTUP_REMOVE,
    IDM_STARTUP_CLEAR,
    IDM_STARTUP_REMOVE_ALL,
    IDM_STARTUP_EDIT = 1210,
    IDM_DRV_EDIT = 1305,

    IDM_DRV_OPEN_LOC = 1300,
    IDM_DRV_COPY_PATH,
    IDM_DRV_VERIFY_SIG,
    IDM_DRV_START,
    IDM_DRV_STOP,

    IDM_DRV_BOOT = 1310,
    IDM_DRV_SYSTEM = 1311,
    IDM_DRV_AUTO = 1312,
    IDM_DRV_MANUAL = 1313,
    IDM_DRV_DISABLED = 1314,
};

struct ProcessDisplayInfo {
    DWORD pid = 0;
    DWORD parentPid = 0;
    std::wstring name;
    std::wstring fullPath;
    std::wstring userName;
    SIZE_T memoryUsage = 0;
    int threadCount = 0;
    int handleCount = 0;
    DWORD priority = 0;
    FILETIME kernelTime{};
    FILETIME userTime{};
    bool critical = false;
    bool suspended = false;
    bool wow64 = false;
    float cpuPercent = 0.0f;

    FILETIME creationTime{};
    int riskScore = 0;

    // Sig проверка подписи по клику
    bool sigChecked = false;
    bool sigValid = false;
    bool sigInvalid = false;
    bool sigMicrosoft = false;
    std::wstring signer;
};

struct DriverDisplayInfo {
    std::wstring serviceName;
    std::wstring displayName;
    std::wstring imagePath;
    DWORD startType = 4;
    DWORD state = 0;
    bool isKernel = false;
    bool isFs = false;

    // Подпись (ленивая)
    bool sigChecked = false;
    bool sigValid = false;
    bool sigInvalid = false;
    bool sigMicrosoft = false;
    std::wstring signer;
    bool sigInProgress = false; // воркер проверяет
};

// Глобальные данные
static std::vector<ProcessDisplayInfo> g_processes;
static std::vector<ServiceManager::ServiceInfo> g_services;
static std::vector<StartupRegistry::StartupEntry> g_startupEntries;
static std::vector<DriverDisplayInfo> g_drivers;
static bool g_servicesOffline = false;
static bool g_driversOffline = false;

// === Фоновый воркер проверки подписей драйверов ===
static std::atomic<bool> g_sigWorkerRunning{ false };
static std::atomic<bool> g_sigWorkerStop{ false };
static HANDLE            g_sigWorkerThread = nullptr;
static CRITICAL_SECTION  g_driversLock;
static bool              g_driversLockInit = false;

static void StartSigWorker(HWND hwnd); // forward decl
static void StopSigWorker();          // forward decl

// === Фоновый воркер обновления списка процессов ===
static std::atomic<bool> g_procWorkerRunning{ false };
static std::atomic<bool> g_procWorkerStop{ false };
static HANDLE            g_procWorkerThread = nullptr;
static CRITICAL_SECTION  g_procLock;
static bool              g_procLockInit = false;
static std::vector<ProcessDisplayInfo> g_procPending;

static void StartProcWorker(HWND hwnd); // forward decl
static void StopProcWorker();          // forward decl

static int g_selectedProcessIndex = -1;
static int g_selectedServiceIndex = -1;
static int g_selectedStartupIndex = -1;
static int g_selectedDriverIndex = -1;

static bool g_autoClearCriticalOnTerminate = false;
static bool g_forceRefresh = false;
static unsigned long long g_lastRefreshTick = 0;
static std::atomic<bool> g_autoRefresh{ true };
static std::atomic<bool> g_procRefreshRequested{ false };

static std::wstring g_searchText;
static bool g_searchActive = false;

enum class SortColumn { None, Name, Pid, Memory, Threads, Cpu, User };
static SortColumn g_sortColumn = SortColumn::Memory;
static bool g_sortAscending = false;

static RectF g_btnEndProcess;
static RectF g_btnRefresh;
static RectF g_btnHideDuplicates;
static RectF g_hideDuplicatesBox;
static RectF g_btnAutoRefresh;
static RectF g_autoRefreshBox;
static RectF g_btnAutoClearCritical;
static RectF g_autoClearCriticalBox;
static RectF g_btnBlockProcessLaunches;
static RectF g_blockProcessLaunchesBox;
static RectF g_btnDeleteRecentStartup;
static RectF g_searchBoxRect;
static RectF g_btnCreate;
static std::vector<RectF> g_startupFilterRects;
static std::vector<int> g_startupFilterVisibleIndices;
static int g_startupFilter = 1;
static float g_startupFilterScrollX = 0.0f;
static bool g_hideDuplicates = false;
static int g_pathScrollX = 0;
static HWND g_pathTooltip = nullptr;
static std::wstring g_pathTooltipText;
static std::set<DWORD> g_launchGuardKnownPids;
static std::set<DWORD> g_launchGuardSuspendedPids;
static bool g_launchGuardInitialized = false;

static bool IsKnownCriticalSystemProcess(const std::wstring& name) {
    static const wchar_t* names[] = {
        L"system", L"smss.exe", L"csrss.exe", L"wininit.exe", L"services.exe",
        L"lsass.exe", L"winlogon.exe"
    };
    for (const auto* known : names) {
        if (_wcsicmp(name.c_str(), known) == 0) return true;
    }
    return false;
}

static const float MARGIN = 8.0f;
static const float TOOLBAR_HEIGHT = 28.0f;
static const float SUBTAB_HEIGHT = 26.0f;
static const float ROW_HEIGHT = 40.0f;
static const float SEARCH_HEIGHT = 26.0f;

static const float COL_MIN_WIDTH = 40.0f;
static const float COL_MAX_WIDTH = 600.0f;
static const float RESIZE_HIT_ZONE = 5.0f;

static float g_procColWidths[6] = { 210.0f, 70.0f, 170.0f, 100.0f, 110.0f, 420.0f };
static float g_svcColWidths[3] = { 200.0f, 120.0f, 120.0f };
static float g_startupColWidths[4] = { 150.0f, 250.0f, 120.0f, 100.0f };
static float g_driverColWidths[6] = { 150.0f, 180.0f, 80.0f, 90.0f, 250.0f, 40.0f };

static bool g_colWidthsInitialized = false;
static float g_lastColumnAreaWidth = 0.0f;
static int g_resizingColIndex = -1;
static float g_resizingStartX = 0.0f;
static float g_resizingStartWidth = 0.0f;

struct TaskmgrLayout {
    RectF subTabs;
    RectF toolbar;
    RectF searchBox;
    RectF listArea;
    float listDataTop = 0.0f;
};

static TaskmgrLayout GetLayout(const RectF& contentArea, bool showSubTabs) {
    TaskmgrLayout L;
    float y = contentArea.Y + 4.0f;
    float x = contentArea.X + MARGIN;
    float w = contentArea.Width - 2.0f * MARGIN;

    if (showSubTabs) {
        L.subTabs = RectF(x, y, w, SUBTAB_HEIGHT);
        y += SUBTAB_HEIGHT + 4.0f;
    }
    else {
        L.subTabs = RectF(0, 0, 0, 0);
    }

    const float toolbarWidth = (g_activeSubTab == SUBTAB_STARTUP || g_activeSubTab == SUBTAB_SUSPICIOUS)
        ? ((w < 760.0f) ? w : 820.0f)
        : ((w < 620.0f) ? w : 620.0f);
    const float toolbarHeight = (g_activeSubTab == SUBTAB_STARTUP || g_activeSubTab == SUBTAB_SUSPICIOUS) ? 44.0f : TOOLBAR_HEIGHT;
    L.toolbar = RectF(x, y, toolbarWidth, toolbarHeight);
    y += toolbarHeight + 4.0f;

    L.searchBox = RectF(x, y, w, SEARCH_HEIGHT);
    y += SEARCH_HEIGHT + 4.0f;

    float listHeight = contentArea.Height - (y - contentArea.Y) - MARGIN;
    if (listHeight < 0.0f) listHeight = 0.0f;
    L.listArea = RectF(x, y, w, listHeight);
    L.listDataTop = L.listArea.Y + 24.0f + 6.0f;
    return L;
}

static bool IsLikelyRecoveryEnvironment() {
    return RegistryEditor::IsLikelyRecoveryEnvironment();
}

static std::wstring FormatMemory(unsigned long long bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" Б";
    else if (bytes < 1024 * 1024) return std::to_wstring(bytes / 1024) + L" КБ";
    else if (bytes < 1024ULL * 1024 * 1024) return std::to_wstring(bytes / (1024 * 1024)) + L" МБ";
    else return std::to_wstring(bytes / (1024ULL * 1024 * 1024)) + L" ГБ";
}

static std::wstring FormatCpu(float percent) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f%%", percent);
    return buf;
}

static std::wstring ServiceStatusToString(ServiceManager::ServiceState state) {
    using State = ServiceManager::ServiceState;
    switch (state) {
    case State::Stopped:          return L"Остановлена";
    case State::StartPending:     return L"Запускается...";
    case State::StopPending:      return L"Останавливается...";
    case State::Running:          return L"Работает";
    case State::ContinuePending:  return L"Возобновляется...";
    case State::PausePending:     return L"Приостанавливается...";
    case State::Paused:           return L"Приостановлена";
    default:                      return L"Неизвестно";
    }
}

static std::wstring ServiceStartTypeToString(ServiceManager::ServiceStartType st) {
    using ST = ServiceManager::ServiceStartType;
    switch (st) {
    case ST::Boot:    return L"Загрузочная";
    case ST::System:  return L"Системная";
    case ST::Auto:    return L"Авто";
    case ST::Demand:  return L"Вручную";
    case ST::Disabled:return L"Отключена";
    default:          return L"Неизвестно";
    }
}

static std::wstring DriverStartTypeToString(DWORD st) {
    switch (st) {
    case SERVICE_BOOT_START:   return L"Boot";
    case SERVICE_SYSTEM_START: return L"System";
    case SERVICE_AUTO_START:   return L"Авто";
    case SERVICE_DEMAND_START: return L"Вручную";
    case SERVICE_DISABLED:     return L"Отключена";
    default:                   return L"Неизвестно";
    }
}

static std::wstring DriverStateToString(DWORD state) {
    switch (state) {
    case SERVICE_RUNNING:       return L"Работает";
    case SERVICE_STOPPED:       return L"Остановлен";
    case SERVICE_START_PENDING: return L"Запускается...";
    case SERVICE_STOP_PENDING:  return L"Останавливается...";
    default:                    return L"—";
    }
}

// \SystemRoot\System32\drivers\foo.sys -> C:\Windows\System32\drivers\foo.sys
static std::wstring ResolveDriverPath(const std::wstring& imagePath) {
    if (imagePath.empty()) return L"";

    std::wstring p = imagePath;

    // \SystemRoot\...
    const std::wstring sysRoot = L"\\SystemRoot\\";
    if (p.size() >= sysRoot.size() &&
        _wcsnicmp(p.c_str(), sysRoot.c_str(), sysRoot.size()) == 0) {
        wchar_t winDir[MAX_PATH] = {};
        (void)GetWindowsDirectoryW(winDir, MAX_PATH);
        p = std::wstring(winDir) + p.substr(sysRoot.size() - 1);
    }
    // \??\C:\...
    else if (p.size() >= 4 && p[0] == L'\\' && p[1] == L'?' && p[2] == L'?' && p[3] == L'\\') {
        p = p.substr(4);
    }

    // Убираем кавычки
    if (!p.empty() && p[0] == L'"') {
        size_t e = p.find(L'"', 1);
        if (e != std::wstring::npos) p = p.substr(1, e - 1);
    }

    // Отрезаем аргументы, оставляем до .sys включительно
    size_t sysEnd = p.find(L".sys");
    if (sysEnd != std::wstring::npos) p = p.substr(0, sysEnd + 4);

    return p;
}

static unsigned long long FileTimeToULL(const FILETIME& ft) {
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static void LoadProcessDetails(int idx) {
    if (idx < 0 || idx >= (int)g_processes.size()) return;
    auto& info = g_processes[idx];

    auto procInfo = WinCtrl::Process::GetProcessInfo(info.pid);

    info.userName = procInfo.userName;
    info.critical = procInfo.isCritical;
    info.suspended = procInfo.isSuspended;
    info.handleCount = procInfo.handleCount;
    info.priority = procInfo.priorityClass;

    if (!info.sigChecked && !info.fullPath.empty()) {
        std::wstring signer;
        bool isMs = false, isTrusted = false;
        auto st = WinCtrl::Scanner::VerifySignature(
            info.fullPath, &signer, nullptr, &isMs, &isTrusted, true);
        info.signer = signer;
        info.sigMicrosoft = isMs;
        info.sigValid = (st == WinCtrl::Scanner::ScanResult::SigStatus::Valid);
        info.sigInvalid = (st == WinCtrl::Scanner::ScanResult::SigStatus::Invalid);
        info.sigChecked = true;
    }
}

static void LoadDriverSignature(int idx) {
    if (idx < 0 || idx >= (int)g_drivers.size()) return;
    auto& d = g_drivers[idx];
    if (d.sigChecked || d.sigInProgress) return;
    if (d.imagePath.empty()) {
        d.sigChecked = true;
        return;
    }
    std::wstring signer;
    bool isMs = false, isTrusted = false;
    auto st = WinCtrl::Scanner::VerifySignature(
        d.imagePath, &signer, nullptr, &isMs, &isTrusted, true);
    d.signer = signer;
    d.sigMicrosoft = isMs;
    d.sigValid = (st == WinCtrl::Scanner::ScanResult::SigStatus::Valid);
    d.sigInvalid = (st == WinCtrl::Scanner::ScanResult::SigStatus::Invalid);
    d.sigChecked = true;
}

static void RefreshProcessList() {
    g_procRefreshRequested.store(true);
    if (!g_procWorkerRunning.load())
        StartProcWorker(App::Instance()->GetHWND());
}

// === WinRE offline startup ===
static void RefreshStartupList();   // forward decl (нужен для fallback в *Offline)

struct OfflineMounts {
    std::wstring softwareMount;                // "OfflineSoftware" под HKLM
    std::wstring systemMount;                  // "OfflineSystem" под HKLM
    std::vector<std::wstring> userMounts;      // "OfflineUser_<name>" под HKU
    std::vector<std::wstring> ownedMounts;     // выгрузить на выходе
    bool attemptDone = false;
};

static OfflineMounts g_offlineMounts;

// Идемпотентно: если уже смонтировано, ничего не делает
static void EnsureOfflineStartupMounts() {
    if (g_offlineMounts.attemptDone) return;
    g_offlineMounts.attemptDone = true;

    std::wstring winPath = WinCtrl::System::GetOfflineWindowsPath(
        UnlockTools::GetOfflineDriveHint());
    if (winPath.empty()) return;

    // --- SOFTWARE ---
    const wchar_t* softMount = L"OfflineSoftware";
    std::wstring softFile = winPath + L"\\System32\\config\\SOFTWARE";
    if (RegistryEditor::LoadHive(HKEY_LOCAL_MACHINE, softMount, softFile)) {
        g_offlineMounts.softwareMount = softMount;
        g_offlineMounts.ownedMounts.push_back(softMount);
    }

    // --- SYSTEM ---
    const wchar_t* sysMount = L"OfflineSystem";
    std::wstring sysFile = winPath + L"\\System32\\config\\SYSTEM";
    if (RegistryEditor::LoadHive(HKEY_LOCAL_MACHINE, sysMount, sysFile)) {
        g_offlineMounts.systemMount = sysMount;
        g_offlineMounts.ownedMounts.push_back(sysMount);
    }

    // --- NTUSER.DAT каждого профиля ---
    std::wstring drive = winPath.substr(0, 2);
    std::wstring usersDir = drive + L"\\Users";

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((usersDir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (_wcsicmp(fd.cFileName, L"All Users") == 0) continue;
        if (_wcsicmp(fd.cFileName, L"Default User") == 0) continue;

        std::wstring ntPath = usersDir + L"\\" + fd.cFileName + L"\\NTUSER.DAT";
        DWORD attr = GetFileAttributesW(ntPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (attr & FILE_ATTRIBUTE_DIRECTORY) continue;

        std::wstring mountName = L"OfflineUser_" + std::wstring(fd.cFileName);
        if (RegistryEditor::LoadHive(HKEY_USERS, mountName.c_str(), ntPath)) {
            g_offlineMounts.userMounts.push_back(mountName);
            g_offlineMounts.ownedMounts.push_back(mountName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void RefreshServicesList() {
    g_servicesOffline = false;

    if (IsLikelyRecoveryEnvironment()) {
        EnsureOfflineStartupMounts();
        if (!g_offlineMounts.systemMount.empty()) {
            std::wstring winPath = WinCtrl::System::GetOfflineWindowsPath(
                UnlockTools::GetOfflineDriveHint());
            auto infos = OfflineServiceManager::Enumerate(
                g_offlineMounts.systemMount, winPath, /*driversOnly=*/false);

            g_services.clear();
            g_services.reserve(infos.size());
            for (const auto& o : infos) {
                ServiceManager::ServiceInfo s;
                s.name = o.name;
                s.displayName = o.displayName;
                s.description = o.description;
                s.state = ServiceManager::ServiceState::Unknown;
                s.startType = (o.start <= 4)
                    ? static_cast<ServiceManager::ServiceStartType>(o.start)
                    : ServiceManager::ServiceStartType::Unknown;
                s.pid = 0;
                g_services.push_back(std::move(s));
            }
            g_servicesOffline = true;
            return;
        }
    }

    g_services = ServiceManager::GetServices();
}

// Список локаций для диалога "Создать" в офлайн-режиме
static std::vector<StartupEditDialog::Location> BuildOfflineStartupLocations() {
    std::vector<StartupEditDialog::Location> locs;

    // HKLM\...\Run и RunOnce (SOFTWARE целевой системы)
    if (!g_offlineMounts.softwareMount.empty()) {
        const std::wstring& m = g_offlineMounts.softwareMount;
        locs.push_back({ HKEY_LOCAL_MACHINE,
            m + L"\\Microsoft\\Windows\\CurrentVersion\\Run",
            L"HKLM \\ Run (офлайн)", KEY_WOW64_64KEY });
        locs.push_back({ HKEY_LOCAL_MACHINE,
            m + L"\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
            L"HKLM \\ RunOnce (офлайн)", KEY_WOW64_64KEY });
    }

    // HKCU\...\Run каждого офлайн-профиля
    for (const auto& userMount : g_offlineMounts.userMounts) {
        // Извлекаем имя профиля из "OfflineUser_Jack" -> "Jack"
        std::wstring profileName = userMount;
        const std::wstring prefix = L"OfflineUser_";
        if (profileName.rfind(prefix, 0) == 0)
            profileName = profileName.substr(prefix.size());

        locs.push_back({ HKEY_USERS,
            userMount + L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            L"HKCU (" + profileName + L") \\ Run (офлайн)", KEY_WOW64_64KEY });
        locs.push_back({ HKEY_USERS,
            userMount + L"\\Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
            L"HKCU (" + profileName + L") \\ RunOnce (офлайн)", KEY_WOW64_64KEY });
    }

    return locs;
}

static bool IsStartupEntrySuspicious(const StartupRegistry::StartupEntry& entry);

static void RefreshStartupListOffline() {
    g_startupEntries.clear();
    EnsureOfflineStartupMounts();

    if (g_offlineMounts.softwareMount.empty() &&
        g_offlineMounts.userMounts.empty())
    {
        // Не смогли достать ни один улей, читаем как есть
        g_startupEntries = StartupRegistry::GetAllEntries();
        return;
    }

    g_startupEntries = StartupRegistry::GetOfflineEntries(
        g_offlineMounts.softwareMount,
        g_offlineMounts.userMounts);
}

static void RefreshStartupList() {
    if (IsLikelyRecoveryEnvironment()) {
        RefreshStartupListOffline();
        return;
    }
    g_startupEntries = StartupRegistry::GetAllEntries();
}

bool TaskManagerDeleteRecentStartupEntries() {
    std::vector<StartupRegistry::StartupEntry> entries = StartupRegistry::GetAllEntries();
    int removed = 0;
    for (const auto& entry : entries) {
        if (entry.isCritical || !IsStartupEntrySuspicious(entry)) continue;
        if (StartupRegistry::RemoveEntry(entry)) ++removed;
    }
    if (removed > 0) {
        RefreshStartupList();
        return true;
    }
    return false;
}

void TaskManagerShutdown() {
    StopSigWorker();
    StopProcWorker();
    for (const auto& m : g_offlineMounts.ownedMounts) {
        if (m.rfind(L"OfflineUser_", 0) == 0)
            RegistryEditor::UnloadHive(HKEY_USERS, m.c_str());
        else
            RegistryEditor::UnloadHive(HKEY_LOCAL_MACHINE, m.c_str());
    }
    g_offlineMounts.ownedMounts.clear();
    g_offlineMounts.userMounts.clear();
    g_offlineMounts.softwareMount.clear();
    g_offlineMounts.systemMount.clear();
    g_offlineMounts.attemptDone = false;

    if (g_driversLockInit) {
        DeleteCriticalSection(&g_driversLock);
        g_driversLockInit = false;
    }
    if (g_procLockInit) {
        DeleteCriticalSection(&g_procLock);
        g_procLockInit = false;
    }
}

// === Фоновый воркер проверки подписей ===
static unsigned __stdcall SigWorkerProc(void* p);

static void StartSigWorker(HWND hwnd) {
    if (g_sigWorkerRunning.load()) return;
    if (!g_driversLockInit) {
        InitializeCriticalSection(&g_driversLock);
        g_driversLockInit = true;
    }

    g_sigWorkerStop.store(false);
    unsigned tid = 0;
    g_sigWorkerThread = (HANDLE)_beginthreadex(
        nullptr, 0, SigWorkerProc, (void*)hwnd, 0, &tid);
    if (g_sigWorkerThread) {
        g_sigWorkerRunning.store(true);
    }
}

static void StopSigWorker() {
    if (!g_sigWorkerRunning.load() && !g_sigWorkerThread) return;
    g_sigWorkerStop.store(true);
    if (g_sigWorkerThread) {
        WaitForSingleObject(g_sigWorkerThread, 5000);
        CloseHandle(g_sigWorkerThread);
        g_sigWorkerThread = nullptr;
    }
    g_sigWorkerRunning.store(false);
}

// === Быстрые эвристики процесса ===
// Vетаданные (путь, имя, время)
static int ComputeProcessRisk(const ProcessDisplayInfo& p,
    const std::vector<ProcessDisplayInfo>& all)
{
    if (p.fullPath.empty()) return 0;
    int score = 0;

    std::wstring lower = p.fullPath;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

    // 1. Запущен из TEMP / AppData / Windows\Temp
    {
        wchar_t temp[MAX_PATH] = {};
        wchar_t appdata[MAX_PATH] = {};
        GetEnvironmentVariableW(L"TEMP", temp, MAX_PATH);
        GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);

        auto startsWith = [&](const wchar_t* s) {
            if (!s || !*s) return false;
            std::wstring sub = s;
            std::transform(sub.begin(), sub.end(), sub.begin(), ::towlower);
            return lower.compare(0, sub.size(), sub) == 0;
            };

        if (startsWith(temp) || startsWith(appdata))
            score += 3;
        else if (lower.find(L"\\windows\\temp\\") != std::wstring::npos)
            score += 3;
    }

    // 2. Системный бинарь не из System32 / SysWOW64
    {
        std::wstring n = p.name;
        std::transform(n.begin(), n.end(), n.begin(), ::towlower);

        static const wchar_t* kSys[] = {
            L"svchost.exe", L"lsass.exe", L"services.exe", L"csrss.exe",
            L"winlogon.exe", L"smss.exe", L"wininit.exe", L"spoolsv.exe",
            L"taskhost.exe", L"taskhostw.exe", L"dwm.exe", L"conhost.exe",
            L"runtimebroker.exe", L"searchindexer.exe", L"sihost.exe",
            L"fontdrvhost.exe", L"ctfmon.exe", L"explorer.exe", L"wmiprvse.exe"
        };
        for (auto* s : kSys) {
            if (n == s) {
                if (lower.find(L"\\system32\\") == std::wstring::npos &&
                    lower.find(L"\\syswow64\\") == std::wstring::npos)
                    score += 5;
                break;
            }
        }
    }

    // 3. Свежий файл (создан < 7 дней назад)
    if (p.creationTime.dwHighDateTime || p.creationTime.dwLowDateTime) {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER a, b;
        a.LowPart = p.creationTime.dwLowDateTime;
        a.HighPart = p.creationTime.dwHighDateTime;
        b.LowPart = now.dwLowDateTime;
        b.HighPart = now.dwHighDateTime;
        ULONGLONG ageSec = (b.QuadPart - a.QuadPart) / 10000000ULL;
        if (ageSec < 7ULL * 24 * 3600) score += 2;
    }

    // 4. Дубликат имени из другого пути
    {
        int same = 0, diffPath = 0;
        for (const auto& q : all) {
            if (_wcsicmp(q.name.c_str(), p.name.c_str()) == 0) {
                ++same;
                if (_wcsicmp(q.fullPath.c_str(), p.fullPath.c_str()) != 0)
                    ++diffPath;
            }
        }
        if (same >= 2 && diffPath >= 1) score += 2;
    }

    return score;
}

// === Фоновый воркер обновления списка процессов ===
static unsigned __stdcall ProcWorkerProc(void* p) {
    HWND hwnd = (HWND)p;

    std::unordered_map<DWORD, unsigned long long> prevKernel, prevUser;
    unsigned long long prevTick = GetTickCount64();

    while (!g_procWorkerStop.load()) {
        if (!g_autoRefresh.load() && !g_procRefreshRequested.exchange(false)) {
            Sleep(100);
            continue;
        }
        g_procRefreshRequested.store(false);

        // 1. Снимок процессов
        auto pidList = WinCtrl::Process::GetList();

        std::unordered_map<DWORD, int> threadCounts;
        HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (hThreadSnap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te{ sizeof(te) };
            if (Thread32First(hThreadSnap, &te)) {
                do { threadCounts[te.th32OwnerProcessID]++; } while (Thread32Next(hThreadSnap, &te));
            }
            CloseHandle(hThreadSnap);
        }

        unsigned long long now = GetTickCount64();
        unsigned long long deltaTime = now - prevTick;
        if (deltaTime == 0) deltaTime = 1;

        SYSTEM_INFO si; GetSystemInfo(&si);
        float maxCpu = 100.0f * si.dwNumberOfProcessors;

        std::vector<ProcessDisplayInfo> fresh;
        fresh.reserve(pidList.size());

        std::unordered_map<DWORD, unsigned long long> newKernel, newUser;

        for (const auto& pe : pidList) {
            if (g_procWorkerStop.load()) break;

            DWORD pid = pe.th32ProcessID;
            ProcessDisplayInfo info{};
            info.pid = pid;
            info.parentPid = pe.th32ParentProcessID;
            info.name = pe.szExeFile;
            info.threadCount = threadCounts.count(pid) ? threadCounts[pid] : 0;
            auto details = WinCtrl::Process::GetProcessInfo(pid);
            info.critical = details.isCritical || IsKnownCriticalSystemProcess(info.name);

            HANDLE hProc = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (hProc) {
                wchar_t path[MAX_PATH] = {};
                DWORD sz = MAX_PATH;
                if (QueryFullProcessImageNameW(hProc, 0, path, &sz))
                    info.fullPath = path;

                PROCESS_MEMORY_COUNTERS_EX pmc{ sizeof(pmc) };
                if (GetProcessMemoryInfo(hProc,
                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
                    info.memoryUsage = pmc.WorkingSetSize;

                BOOL wow64 = FALSE;
                if (IsWow64Process(hProc, &wow64)) info.wow64 = wow64 != FALSE;

                FILETIME ct, et, kt, ut;
                if (GetProcessTimes(hProc, &ct, &et, &kt, &ut)) {
                    info.kernelTime = kt;
                    info.userTime = ut;
                    info.creationTime = ct;
                }
                CloseHandle(hProc);
            }

            if (info.fullPath.empty())
                info.fullPath = WinCtrl::Process::GetImagePath(pid);

            unsigned long long kernel = FileTimeToULL(info.kernelTime);
            unsigned long long user = FileTimeToULL(info.userTime);
            auto itK = prevKernel.find(pid);
            auto itU = prevUser.find(pid);
            if (itK != prevKernel.end() && itU != prevUser.end()) {
                unsigned long long prevTotal = itK->second + itU->second;
                unsigned long long total = kernel + user;
                unsigned long long deltaCpu = (total > prevTotal) ? (total - prevTotal) : 0;
                float pct = (float)((double)deltaCpu / 10000.0 / (double)deltaTime * 100.0);
                if (pct > maxCpu) pct = maxCpu;
                info.cpuPercent = pct;
            }
            newKernel[pid] = kernel;
            newUser[pid] = user;

            fresh.push_back(std::move(info));
        }

        // Эвристики считаем один раз на снимок
        for (auto& pr : fresh)
            pr.riskScore = ComputeProcessRisk(pr, fresh);

        prevKernel = std::move(newKernel);
        prevUser = std::move(newUser);
        prevTick = now;

        if (g_procWorkerStop.load()) break;

        // 2. Перенос пользовательских данных из старого списка (по PID)
        EnterCriticalSection(&g_procLock);
        {
            std::unordered_map<DWORD, int> oldIdx;
            oldIdx.reserve(g_processes.size());
            for (int i = 0; i < (int)g_processes.size(); ++i)
                oldIdx[g_processes[i].pid] = i;

            for (auto& f : fresh) {
                auto it = oldIdx.find(f.pid);
                if (it == oldIdx.end()) continue;
                const auto& o = g_processes[it->second];
                f.sigChecked = o.sigChecked;
                f.sigValid = o.sigValid;
                f.sigInvalid = o.sigInvalid;
                f.sigMicrosoft = o.sigMicrosoft;
                f.signer = o.signer;
                f.userName = o.userName;
                f.critical = o.critical;
                f.suspended = o.suspended;
                f.handleCount = o.handleCount;
                f.priority = o.priority;
            }

            g_procPending = std::move(fresh);
        }
        LeaveCriticalSection(&g_procLock);

        // 3. Уведомить UI
        if (hwnd && IsWindow(hwnd))
            PostMessageW(hwnd, WM_TASKMGR_PROC_READY, 0, 0);

        // 4. Пауза ~2 сек с проверкой отмены
        for (int i = 0; i < 20 && !g_procWorkerStop.load() && g_autoRefresh.load(); ++i)
            Sleep(100);
    }
    return 0;
}

static void StartProcWorker(HWND hwnd) {
    if (g_procWorkerRunning.load()) return;
    if (!g_procLockInit) {
        InitializeCriticalSection(&g_procLock);
        g_procLockInit = true;
    }
    g_procWorkerStop.store(false);
    unsigned tid = 0;
    g_procWorkerThread = (HANDLE)_beginthreadex(
        nullptr, 0, ProcWorkerProc, (void*)hwnd, 0, &tid);
    if (g_procWorkerThread)
        g_procWorkerRunning.store(true);
}

static void StopProcWorker() {
    if (!g_procWorkerRunning.load() && !g_procWorkerThread) return;
    g_procWorkerStop.store(true);
    if (g_procWorkerThread) {
        WaitForSingleObject(g_procWorkerThread, 5000);
        CloseHandle(g_procWorkerThread);
        g_procWorkerThread = nullptr;
    }
    g_procWorkerRunning.store(false);
}

static unsigned __stdcall SigWorkerProc(void* p) {
    HWND hwnd = (HWND)p;

    while (!g_sigWorkerStop.load()) {
        // 1. Найти следующий непроверенный драйвер
        int idx = -1;
        std::wstring path;

        EnterCriticalSection(&g_driversLock);
        for (int i = 0; i < (int)g_drivers.size(); ++i) {
            if (!g_drivers[i].sigChecked && !g_drivers[i].sigInProgress) {
                g_drivers[i].sigInProgress = true;
                idx = i;
                path = g_drivers[i].imagePath;
                break;
            }
        }
        LeaveCriticalSection(&g_driversLock);

        if (idx < 0) {
            // Задач нет, ждём появления (refresh может добавить)
            for (int i = 0; i < 20 && !g_sigWorkerStop.load(); ++i) {
                Sleep(25);
            }
            continue;
        }

        // 2. Пустой путь, нечего проверять
        if (path.empty()) {
            EnterCriticalSection(&g_driversLock);
            if (idx < (int)g_drivers.size()) {
                g_drivers[idx].sigChecked = true;
                g_drivers[idx].sigInProgress = false;
            }
            LeaveCriticalSection(&g_driversLock);
            continue;
        }

        // 3. Проверка подписи (медленная часть, вне лока)
        std::wstring signer;
        bool isMs = false, isTrusted = false;
        auto st = WinCtrl::Scanner::VerifySignature(
            path, &signer, nullptr, &isMs, &isTrusted, true);

        if (g_sigWorkerStop.load()) break;

        // 4. Записать результат под локом
        EnterCriticalSection(&g_driversLock);
        if (idx < (int)g_drivers.size()) {
            g_drivers[idx].signer = signer;
            g_drivers[idx].sigMicrosoft = isMs;
            g_drivers[idx].sigValid =
                (st == WinCtrl::Scanner::ScanResult::SigStatus::Valid);
            g_drivers[idx].sigInvalid =
                (st == WinCtrl::Scanner::ScanResult::SigStatus::Invalid);
            g_drivers[idx].sigChecked = true;
            g_drivers[idx].sigInProgress = false;
        }
        LeaveCriticalSection(&g_driversLock);

        // 5. Уведомить UI
        if (hwnd && IsWindow(hwnd)) {
            PostMessageW(hwnd, WM_TASKMGR_SIG_READY, 0, 0);
        }
        // 6. Отдых
        Sleep(10);
    }

    return 0;
}

bool OnTaskManagerMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)wParam; (void)lParam;

    if (msg == WM_TASKMGR_SIG_READY) {
        if (g_activeMainTab == 1 && g_activeSubTab == SUBTAB_DRIVERS)
            InvalidateRect(App::Instance()->GetHWND(), nullptr, FALSE);
        return true;
    }

    if (msg == WM_TASKMGR_PROC_READY) {
        if (g_procLockInit) {
            EnterCriticalSection(&g_procLock);
            std::swap(g_processes, g_procPending);
            g_procPending.clear();
            LeaveCriticalSection(&g_procLock);
        }
        if (g_activeMainTab == 1 && g_activeSubTab == SUBTAB_PROCESSES)
            InvalidateRect(App::Instance()->GetHWND(), nullptr, FALSE);
        return true;
    }

    return false;
}

static void RefreshDriversList() {
    if (!g_driversLockInit) {
        InitializeCriticalSection(&g_driversLock);
        g_driversLockInit = true;
    }

    StopSigWorker();

    std::vector<DriverDisplayInfo> fresh;
    bool offline = false;

    if (IsLikelyRecoveryEnvironment()) {
        EnsureOfflineStartupMounts();
        if (!g_offlineMounts.systemMount.empty()) {
            std::wstring winPath = WinCtrl::System::GetOfflineWindowsPath(
                UnlockTools::GetOfflineDriveHint());
            auto infos = OfflineServiceManager::Enumerate(
                g_offlineMounts.systemMount, winPath, /*driversOnly=*/true);

            fresh.reserve(infos.size());
            for (const auto& o : infos) {
                DriverDisplayInfo d;
                d.serviceName = o.name;
                d.displayName = o.displayName;
                d.imagePath = o.imagePath;
                d.startType = o.start;
                d.state = 0;
                d.isKernel = (o.type & SERVICE_KERNEL_DRIVER) != 0;
                d.isFs = (o.type & SERVICE_FILE_SYSTEM_DRIVER) != 0;
                fresh.push_back(std::move(d));
            }
            offline = true;
        }
    }

    if (!offline) {
        HKEY hServices = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Services",
            0, KEY_ENUMERATE_SUB_KEYS | KEY_READ, &hServices) == ERROR_SUCCESS)
        {
            SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);

            wchar_t subName[512];
            DWORD index = 0;
            while (true) {
                DWORD len = 512;
                if (RegEnumKeyExW(hServices, index++, subName, &len,
                    nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;

                HKEY hSvc = nullptr;
                if (RegOpenKeyExW(hServices, subName, 0, KEY_READ, &hSvc) != ERROR_SUCCESS)
                    continue;

                DWORD type = 0, typeSize = sizeof(type);
                RegQueryValueExW(hSvc, L"Type", nullptr, nullptr, (LPBYTE)&type, &typeSize);

                DWORD baseType = type & 0x3;
                bool isKernel = (baseType == 1 || baseType == 3);
                bool isFs = (baseType == 2 || baseType == 3);

                if (!isKernel && !isFs) {
                    RegCloseKey(hSvc);
                    continue;
                }

                DriverDisplayInfo info;
                info.serviceName = subName;
                info.isKernel = isKernel;
                info.isFs = isFs;

                wchar_t buf[4096] = {};
                DWORD bufSize = sizeof(buf);
                if (RegQueryValueExW(hSvc, L"ImagePath", nullptr, nullptr,
                    (LPBYTE)buf, &bufSize) == ERROR_SUCCESS) {
                    info.imagePath = ResolveDriverPath(buf);
                }

                bufSize = sizeof(buf);
                if (RegQueryValueExW(hSvc, L"DisplayName", nullptr, nullptr,
                    (LPBYTE)buf, &bufSize) == ERROR_SUCCESS) {
                    info.displayName = buf;
                }
                else {
                    info.displayName = subName;
                }

                DWORD startType = 4, stSize = sizeof(startType);
                RegQueryValueExW(hSvc, L"Start", nullptr, nullptr,
                    (LPBYTE)&startType, &stSize);
                info.startType = startType;

                if (scm) {
                    SC_HANDLE hService = OpenServiceW(scm, subName, SERVICE_QUERY_STATUS);
                    if (hService) {
                        SERVICE_STATUS status{};
                        if (QueryServiceStatus(hService, &status))
                            info.state = status.dwCurrentState;
                        CloseServiceHandle(hService);
                    }
                }

                RegCloseKey(hSvc);
                fresh.push_back(std::move(info));
            }

            if (scm) CloseServiceHandle(scm);
            RegCloseKey(hServices);
        }

        std::sort(fresh.begin(), fresh.end(),
            [](const DriverDisplayInfo& a, const DriverDisplayInfo& b) {
                return _wcsicmp(a.serviceName.c_str(), b.serviceName.c_str()) < 0;
            });
    }

    // Атомарно подменяем вектор
    EnterCriticalSection(&g_driversLock);
    g_drivers = std::move(fresh);
    LeaveCriticalSection(&g_driversLock);

    g_driversOffline = offline;

    // Запускаем воркер, он пройдёт по всем sigChecked == false
    StartSigWorker(App::Instance()->GetHWND());
}

static void DoRefresh() {
    int tab = g_activeSubTab;
    switch (tab) {
    case SUBTAB_PROCESSES:
        RefreshProcessList();
        break;
    case SUBTAB_STARTUP:
    case SUBTAB_SUSPICIOUS:
        RefreshStartupList();
        break;
    case SUBTAB_SERVICES:
        RefreshServicesList();
        break;
    case SUBTAB_DRIVERS:
        RefreshDriversList();
        break;
    }
    g_lastRefreshTick = GetTickCount64();
    g_forceRefresh = false;
    InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
}

static bool IsLaunchGuardProtected(DWORD pid, const std::wstring& name) {
    if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) return true;
    static const wchar_t* protectedNames[] = {
        L"system", L"smss.exe", L"csrss.exe", L"wininit.exe", L"services.exe",
        L"lsass.exe", L"winlogon.exe", L"svchost.exe", L"dwm.exe",
        L"fontdrvhost.exe", L"conhost.exe"
    };
    for (const auto* protectedName : protectedNames) {
        if (_wcsicmp(name.c_str(), protectedName) == 0) return true;
    }
    return false;
}

static void ResumeLaunchGuardProcesses() {
    for (DWORD pid : g_launchGuardSuspendedPids) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) continue;
        THREADENTRY32 entry{ sizeof(entry) };
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID != pid) continue;
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
                if (thread) {
                    ResumeThread(thread);
                    CloseHandle(thread);
                }
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    g_launchGuardSuspendedPids.clear();
    g_launchGuardKnownPids.clear();
    g_launchGuardInitialized = false;
}

bool HasFrozenProcesses() {
    return !g_launchGuardSuspendedPids.empty();
}

void ResumeFrozenProcesses() {
    if (!g_launchGuardSuspendedPids.empty()) {
        ResumeLaunchGuardProcesses();
    }
    else {
        g_launchGuardKnownPids.clear();
        g_launchGuardInitialized = false;
    }
}

static void EnforceLaunchGuard() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    std::set<DWORD> currentPids;
    PROCESSENTRY32W entry{ sizeof(entry) };
    if (Process32FirstW(snapshot, &entry)) {
        do {
            currentPids.insert(entry.th32ProcessID);
            if (!g_launchGuardInitialized) {
                g_launchGuardKnownPids.insert(entry.th32ProcessID);
                continue;
            }

            if (g_launchGuardKnownPids.count(entry.th32ProcessID) != 0 ||
                g_launchGuardSuspendedPids.count(entry.th32ProcessID) != 0 ||
                IsLaunchGuardProtected(entry.th32ProcessID, entry.szExeFile)) {
                continue;
            }

            HANDLE threadSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (threadSnapshot == INVALID_HANDLE_VALUE) continue;
            THREADENTRY32 threadEntry{ sizeof(threadEntry) };
            bool suspendedAny = false;
            if (Thread32First(threadSnapshot, &threadEntry)) {
                do {
                    if (threadEntry.th32OwnerProcessID != entry.th32ProcessID) continue;
                    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadEntry.th32ThreadID);
                    if (thread) {
                        SuspendThread(thread);
                        CloseHandle(thread);
                        suspendedAny = true;
                    }
                } while (Thread32Next(threadSnapshot, &threadEntry));
            }
            CloseHandle(threadSnapshot);
            if (suspendedAny) {
                g_launchGuardSuspendedPids.insert(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    if (!g_launchGuardInitialized) {
        g_launchGuardKnownPids = std::move(currentPids);
        g_launchGuardInitialized = true;
        return;
    }

    g_launchGuardKnownPids = std::move(currentPids);
}

void TaskManagerOnTimer() {
    if (g_blockProcessLaunches) EnforceLaunchGuard();
    else if (!g_launchGuardSuspendedPids.empty()) ResumeLaunchGuardProcesses();

    if (!g_autoRefresh.load()) return;
    unsigned long long now = GetTickCount64();
    if (now - g_lastRefreshTick >= TASKMGR_AUTO_REFRESH_INTERVAL_MS) {
        DoRefresh();
    }
}

struct SystemStatsEx {
    unsigned long long totalRam = 0;
    unsigned long long usedRam = 0;
    float cpuUsagePercent = 0.0f;
    int totalProcesses = 0;
};

static SystemStatsEx GetSystemStatsEx() {
    SystemStatsEx stats;

    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        stats.totalRam = memStatus.ullTotalPhys;
        stats.usedRam = memStatus.ullTotalPhys - memStatus.ullAvailPhys;
    }

    static FILETIME prevIdleTime{};
    static FILETIME prevKernelTime{};
    static FILETIME prevUserTime{};
    static bool initialized = false;

    FILETIME idleTime{}, kernelTime{}, userTime{};
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        if (initialized) {
            auto toULL = [](const FILETIME& ft) -> unsigned long long {
                return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
                };
            unsigned long long idle = toULL(idleTime) - toULL(prevIdleTime);
            unsigned long long kernel = toULL(kernelTime) - toULL(prevKernelTime);
            unsigned long long user = toULL(userTime) - toULL(prevUserTime);
            unsigned long long total = kernel + user;
            if (total > 0) {
                stats.cpuUsagePercent = static_cast<float>((total - idle) * 100.0 / total);
            }
        }
        prevIdleTime = idleTime;
        prevKernelTime = kernelTime;
        prevUserTime = userTime;
        initialized = true;
    }

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W) };
        if (Process32FirstW(hSnapshot, &pe)) {
            do {
                stats.totalProcesses++;
            } while (Process32NextW(hSnapshot, &pe));
        }
        CloseHandle(hSnapshot);
    }

    return stats;
}

static bool MatchesSearch(const std::wstring& text) {
    if (g_searchText.empty()) return true;
    std::wstring lower = text, search = g_searchText;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });
    std::transform(search.begin(), search.end(), search.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });
    return lower.find(search) != std::wstring::npos;
}

static std::vector<int> GetFilteredProcessIndices() {
    std::vector<int> result;
    std::set<std::wstring> seen;
    for (int i = 0; i < (int)g_processes.size(); ++i) {
        const auto& p = g_processes[i];
        if (MatchesSearch(p.name) || MatchesSearch(std::to_wstring(p.pid)) ||
            MatchesSearch(p.fullPath) || MatchesSearch(p.userName)) {
            if (g_hideDuplicates) {
                std::wstring key = p.fullPath.empty() ? p.name : p.fullPath;
                std::transform(key.begin(), key.end(), key.begin(), ::towlower);
                if (!seen.insert(key).second) continue;
            }
            result.push_back(i);
        }
    }
    std::sort(result.begin(), result.end(), [](int a, int b) {
        const auto& pa = g_processes[a];
        const auto& pb = g_processes[b];
        int cmp = 0;
        switch (g_sortColumn) {
        case SortColumn::Name:    cmp = _wcsicmp(pa.name.c_str(), pb.name.c_str()); break;
        case SortColumn::Pid:     cmp = (pa.pid < pb.pid) ? -1 : (pa.pid > pb.pid ? 1 : 0); break;
        case SortColumn::Memory:  cmp = (pa.memoryUsage < pb.memoryUsage) ? -1 : (pa.memoryUsage > pb.memoryUsage ? 1 : 0); break;
        case SortColumn::Threads: cmp = (pa.threadCount < pb.threadCount) ? -1 : (pa.threadCount > pb.threadCount ? 1 : 0); break;
        case SortColumn::Cpu:
            cmp = (pa.cpuPercent < pb.cpuPercent) ? -1
                : (pa.cpuPercent > pb.cpuPercent ? 1 : 0);
            break;
        case SortColumn::User: cmp = _wcsicmp(pa.userName.c_str(), pb.userName.c_str()); break;
        default: cmp = 0;
        }
        return g_sortAscending ? cmp < 0 : cmp > 0;
        });
    return result;
}

static std::vector<int> GetFilteredServiceIndices() {
    std::vector<int> result;
    for (int i = 0; i < (int)g_services.size(); ++i) {
        const auto& s = g_services[i];
        if (MatchesSearch(s.name) || MatchesSearch(s.displayName)) result.push_back(i);
    }
    std::sort(result.begin(), result.end(), [](int a, int b) {
        return _wcsicmp(g_services[a].displayName.c_str(), g_services[b].displayName.c_str()) < 0;
        });
    return result;
}

static bool IsStartupCommandSystemSafe(const std::wstring& command) {
    std::wstring lower = command;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (lower.empty()) return false;

    const wchar_t* safePrefixes[] = {
        L"c:\\windows\\", L"c:\\program files\\", L"c:\\program files (x86)\\",
        L"\\\\?\\", L"%systemroot%\\", L"%windir%\\", L"%programfiles%\\"
    };
    for (const auto* prefix : safePrefixes) {
        if (lower.compare(0, wcslen(prefix), prefix) == 0) return true;
    }
    return false;
}

static bool IsStartupEntryRecent(const StartupRegistry::StartupEntry& entry) {
    if (entry.root == nullptr || entry.regPath.empty()) return false;

    HKEY hKey = reinterpret_cast<HKEY>(RegistryEditor::OpenKey(entry.root, entry.regPath, KEY_READ | entry.view));
    if (!hKey) return false;

    FILETIME lastWrite{};
    LONG status = RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, &lastWrite);
    RegistryEditor::CloseKey(reinterpret_cast<HANDLE>(hKey));
    if (status != ERROR_SUCCESS) return false;

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER uNow, uLast;
    uNow.LowPart = now.dwLowDateTime;
    uNow.HighPart = now.dwHighDateTime;
    uLast.LowPart = lastWrite.dwLowDateTime;
    uLast.HighPart = lastWrite.dwHighDateTime;
    const ULONGLONG thirtyDays = 30ULL * 24ULL * 60ULL * 60ULL * 10000000ULL;
    return (uNow.QuadPart - uLast.QuadPart) <= thirtyDays;
}

static bool IsStartupEntrySuspicious(const StartupRegistry::StartupEntry& entry) {
    if (entry.isCritical || !entry.enabled || entry.command.empty()) return false;

    std::wstring command = entry.command;
    if (!command.empty() && command.front() == L'"') {
        size_t end = command.find(L'"', 1);
        if (end != std::wstring::npos) command = command.substr(1, end - 1);
    }
    else {
        size_t space = command.find(L' ');
        if (space != std::wstring::npos) command = command.substr(0, space);
    }

    std::wstring lower = command;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (lower.empty()) return false;
    if (IsStartupCommandSystemSafe(lower)) return false;

    bool lowRiskDefault = lower.find(L"\\windows\\") != std::wstring::npos ||
        lower.find(L"\\system32\\") != std::wstring::npos ||
        lower.find(L"\\syswow64\\") != std::wstring::npos ||
        lower.find(L"\\wininit\\") != std::wstring::npos;
    if (lowRiskDefault) return false;

    bool userPath = lower.find(L"\\appdata\\") != std::wstring::npos ||
        lower.find(L"\\temp\\") != std::wstring::npos ||
        lower.find(L"\\users\\") != std::wstring::npos ||
        lower.find(L"\\downloads\\") != std::wstring::npos ||
        lower.find(L"\\desktop\\") != std::wstring::npos ||
        lower.find(L"\\documents\\") != std::wstring::npos;

    return userPath && IsStartupEntryRecent(entry);
}

static std::wstring NormalizeStartupPathText(const std::wstring& text) {
    std::wstring out = text;
    std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) {
        return (wchar_t)std::towlower(c);
    });
    return out;
}

static bool IsStartupFolderEntry(const StartupRegistry::StartupEntry& entry) {
    const std::wstring lowReg = NormalizeStartupPathText(entry.regPath);
    const std::wstring lowCmd = NormalizeStartupPathText(entry.command);
    const bool hasStartupFolder = lowCmd.find(L"\\start menu\\programs\\startup") != std::wstring::npos ||
        lowCmd.find(L"\\startup") != std::wstring::npos && lowCmd.find(L"start menu") != std::wstring::npos ||
        lowCmd.find(L"\\appdata\\") != std::wstring::npos && lowCmd.find(L"startup") != std::wstring::npos ||
        lowCmd.find(L"\\programdata\\") != std::wstring::npos && lowCmd.find(L"startup") != std::wstring::npos;
    return hasStartupFolder ||
        lowReg.find(L"\\start menu\\programs\\startup") != std::wstring::npos ||
        lowReg.find(L"\\startup") != std::wstring::npos && lowReg.find(L"start menu") != std::wstring::npos;
}

static bool IsTaskSchedulerEntry(const StartupRegistry::StartupEntry& entry) {
    const std::wstring lowReg = NormalizeStartupPathText(entry.regPath);
    const std::wstring lowCmd = NormalizeStartupPathText(entry.command);
    const std::wstring lowText = lowReg + L" " + lowCmd;
    return lowText.find(L"taskcache") != std::wstring::npos ||
           lowText.find(L"\\tasks") != std::wstring::npos ||
           lowText.find(L"schedule") != std::wstring::npos ||
           lowText.find(L"taskschd.msc") != std::wstring::npos ||
           entry.source == StartupRegistry::StartupSource::RunServices ||
           entry.source == StartupRegistry::StartupSource::ActiveSetup;
}

static bool IsDriverEntry(const StartupRegistry::StartupEntry& entry) {
    const std::wstring lowReg = NormalizeStartupPathText(entry.regPath);
    const std::wstring lowCmd = NormalizeStartupPathText(entry.command);
    const std::wstring lowText = lowReg + L" " + lowCmd;
    return entry.source == StartupRegistry::StartupSource::BootExecute ||
           entry.source == StartupRegistry::StartupSource::RunServices ||
           lowText.find(L"\\drivers") != std::wstring::npos ||
           lowText.find(L"\\currentcontrolset\\services") != std::wstring::npos ||
           lowText.find(L"\\currentcontrolset\\control\\class") != std::wstring::npos;
}

static bool IsSystemDllEntry(const StartupRegistry::StartupEntry& entry) {
    const std::wstring lowReg = NormalizeStartupPathText(entry.regPath);
    const std::wstring lowCmd = NormalizeStartupPathText(entry.command);
    const std::wstring lowText = lowReg + L" " + lowCmd;
    return entry.source == StartupRegistry::StartupSource::AppInitDlls ||
           entry.source == StartupRegistry::StartupSource::BootExecute ||
           lowText.find(L"\\system32") != std::wstring::npos ||
           lowText.find(L"\\syswow64") != std::wstring::npos ||
           lowText.find(L"\\dllcache") != std::wstring::npos ||
           lowText.find(L"\\winsxs") != std::wstring::npos;
}

static bool IsInstalledProgramEntry(const StartupRegistry::StartupEntry& entry) {
    const std::wstring lowReg = NormalizeStartupPathText(entry.regPath);
    const std::wstring lowCmd = NormalizeStartupPathText(entry.command);
    const std::wstring lowText = lowReg + L" " + lowCmd;
    return entry.source == StartupRegistry::StartupSource::ActiveSetup ||
           lowText.find(L"\\program files") != std::wstring::npos ||
           lowText.find(L"\\programdata") != std::wstring::npos ||
           lowText.find(L"\\localappdata") != std::wstring::npos ||
           lowText.find(L"\\appdata") != std::wstring::npos ||
           lowText.find(L"\\uninstall") != std::wstring::npos ||
           lowText.find(L"\\wow6432node\\microsoft\\windows\\currentversion\\uninstall") != std::wstring::npos;
}

static bool StartupEntryMatchesFilter(const StartupRegistry::StartupEntry& entry) {
    switch (g_startupFilter) {
    case 1: return entry.source == StartupRegistry::StartupSource::Run ||
                   entry.source == StartupRegistry::StartupSource::Wow64Run ||
                   entry.source == StartupRegistry::StartupSource::DefaultProfileRun;
    case 2: return entry.source == StartupRegistry::StartupSource::RunOnce ||
                   entry.source == StartupRegistry::StartupSource::Wow64RunOnce ||
                   entry.source == StartupRegistry::StartupSource::DefaultProfileRunOnce;
    case 3: return entry.source == StartupRegistry::StartupSource::Winlogon;
    case 4: return entry.source == StartupRegistry::StartupSource::BootExecute;
    case 5: return entry.source == StartupRegistry::StartupSource::AppInitDlls;
    default: return true;
    }
}

static std::vector<int> GetFilteredStartupIndices(bool suspiciousOnly = false) {
    std::vector<int> result;
    for (int i = 0; i < (int)g_startupEntries.size(); ++i) {
        const auto& s = g_startupEntries[i];
        bool matches = MatchesSearch(s.valueName) || MatchesSearch(s.command);
        bool suspicious = IsStartupEntrySuspicious(s);
        if (matches && StartupEntryMatchesFilter(s) && (!suspiciousOnly || suspicious)) result.push_back(i);
    }
    return result;
}

static std::vector<int> GetFilteredDriverIndices() {
    std::vector<int> result;
    for (int i = 0; i < (int)g_drivers.size(); ++i) {
        const auto& d = g_drivers[i];
        if (MatchesSearch(d.serviceName) || MatchesSearch(d.displayName) ||
            MatchesSearch(d.imagePath)) {
            result.push_back(i);
        }
    }
    return result;
}

static std::wstring TaskMgr_BrowseDll() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = App::Instance()->GetHWND();
    ofn.lpstrFilter = L"DLL files (*.dll)\0*.dll\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) return file;
    return L"";
}

static bool TaskMgr_IsInjectionAllowedByBitness(DWORD pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return false;
    BOOL targetWow64 = FALSE, currentWow64 = FALSE;
    bool ok = IsWow64Process(hProcess, &targetWow64) && IsWow64Process(GetCurrentProcess(), &currentWow64);
    CloseHandle(hProcess);
    return ok && targetWow64 == currentWow64;
}

static void CopyToClipboard(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (hGlobal) {
        wchar_t* buffer = static_cast<wchar_t*>(GlobalLock(hGlobal));
        if (buffer) {
            wcscpy_s(buffer, text.size() + 1, text.c_str());
            GlobalUnlock(hGlobal);
            SetClipboardData(CF_UNICODETEXT, hGlobal);
        }
    }
    CloseClipboard();
}

static void ShowProcessTerminationError(HWND hwnd, DWORD pid, DWORD errorCode, bool processTree) {
    wchar_t message[256]{};
    swprintf_s(message, ARRAYSIZE(message),
        processTree
            ? L"Не удалось завершить дерево процессов (PID %lu). Код Windows: %lu"
            : L"Не удалось завершить процесс (PID %lu). Код Windows: %lu",
        pid, errorCode);
    MessageBoxW(hwnd, message, L"Диспетчер задач", MB_OK | MB_ICONERROR);
}

static void ExecuteProcessCommand(int cmd, const ProcessDisplayInfo& proc) {
    HWND hwnd = App::Instance()->GetHWND();
    switch (cmd) {
    case IDM_PROC_TERMINATE: {
        if (g_autoClearCriticalOnTerminate && proc.critical) {
            WinCtrl::Process::SetProcessCritical(proc.pid, false);
        }
        DWORD errorCode = ERROR_SUCCESS;
        if (WinCtrl::Process::Terminate(proc.pid, &errorCode)) DoRefresh();
        else ShowProcessTerminationError(hwnd, proc.pid, errorCode, false);
        break;
    }
    case IDM_PROC_TERMINATE_TREE: {
        if (g_autoClearCriticalOnTerminate && proc.critical) {
            WinCtrl::Process::SetProcessCritical(proc.pid, false);
        }
        DWORD errorCode = ERROR_SUCCESS;
        if (WinCtrl::Process::KillProcessTree(proc.pid, &errorCode)) DoRefresh();
        else ShowProcessTerminationError(hwnd, proc.pid, errorCode, true);
        break;
    }
    case IDM_PROC_SUSPEND:
        WinCtrl::Process::SuspendProcess(proc.pid);
        DoRefresh();
        break;
    case IDM_PROC_RESUME:
        WinCtrl::Process::ResumeProcess(proc.pid);
        DoRefresh();
        break;
    case IDM_PROC_PRIORITY_REALTIME:
        WinCtrl::Process::SetProcessPriority(proc.pid, REALTIME_PRIORITY_CLASS);
        DoRefresh();
        break;
    case IDM_PROC_PRIORITY_HIGH:
        WinCtrl::Process::SetProcessPriority(proc.pid, HIGH_PRIORITY_CLASS);
        DoRefresh();
        break;
    case IDM_PROC_PRIORITY_ABOVE_NORMAL:
        WinCtrl::Process::SetProcessPriority(proc.pid, ABOVE_NORMAL_PRIORITY_CLASS);
        DoRefresh();
        break;
    case IDM_PROC_PRIORITY_NORMAL:
        WinCtrl::Process::SetProcessPriority(proc.pid, NORMAL_PRIORITY_CLASS);
        DoRefresh();
        break;
    case IDM_PROC_PRIORITY_BELOW_NORMAL:
        WinCtrl::Process::SetProcessPriority(proc.pid, BELOW_NORMAL_PRIORITY_CLASS);
        DoRefresh();
        break;
    case IDM_PROC_PRIORITY_LOW:
        WinCtrl::Process::SetProcessPriority(proc.pid, IDLE_PRIORITY_CLASS);
        DoRefresh();
        break;
    case IDM_PROC_OPEN_LOCATION: {
        std::wstring path = proc.fullPath.empty() ? WinCtrl::Process::GetImagePath(proc.pid) : proc.fullPath;
        OpenPathInExplorer(path);
        break;
    }
    case IDM_PROC_COPY_PID:  CopyToClipboard(std::to_wstring(proc.pid)); break;
    case IDM_PROC_COPY_NAME: CopyToClipboard(proc.name); break;
    case IDM_PROC_COPY_PATH: CopyToClipboard(proc.fullPath); break;
    case IDM_PROC_INJECT_DLL: {
        if (IsProcessLaunchBlocked(hwnd)) break;
        if (!TaskMgr_IsInjectionAllowedByBitness(proc.pid)) {
            MessageBoxW(hwnd, L"Разрядность не совпадает", L"Ошибка", MB_OK | MB_ICONERROR);
            break;
        }
        std::wstring dllPath = TaskMgr_BrowseDll();
        if (dllPath.empty()) break;
        if (WinCtrl::Process::InjectDLL(proc.pid, dllPath)) {
            MessageBoxW(hwnd, L"DLL инжектирована", L"Успех", MB_OK | MB_ICONINFORMATION);
        }
        else {
            MessageBoxW(hwnd, L"Ошибка инжекта", L"Ошибка", MB_OK | MB_ICONERROR);
        }
        break;
    }
    }
}

static bool ResolveStartupLocation(
    const StartupRegistry::StartupEntry& entry,
    StartupEditDialog::Location& out)
{
    out.root = entry.root;
    out.subKey = entry.regPath;
    out.view = entry.view;

    std::wstring scope = (entry.scope == StartupRegistry::StartupScope::Machine)
        ? L"HKLM" : L"HKCU";
    std::wstring src;
    switch (entry.source) {
    case StartupRegistry::StartupSource::Run:                src = L"Run";             break;
    case StartupRegistry::StartupSource::RunOnce:            src = L"RunOnce";         break;
    case StartupRegistry::StartupSource::RunOnceEx:          src = L"RunOnceEx";       break;
    case StartupRegistry::StartupSource::RunServices:        src = L"RunServices";     break;
    case StartupRegistry::StartupSource::PoliciesRun:        src = L"Policies";        break;
    case StartupRegistry::StartupSource::Wow64Run:           src = L"Run (WOW64)";     break;
    case StartupRegistry::StartupSource::Wow64RunOnce:      src = L"RunOnce (WOW64)"; break;
    case StartupRegistry::StartupSource::DefaultProfileRun:  src = L"Run (.DEFAULT)"; break;
    case StartupRegistry::StartupSource::DefaultProfileRunOnce: src = L"RunOnce (.DEFAULT)"; break;
    case StartupRegistry::StartupSource::ActiveSetup:        src = L"Active Setup";    break;
    case StartupRegistry::StartupSource::Winlogon:           src = L"Winlogon";        break;
    case StartupRegistry::StartupSource::BootExecute:        src = L"BootExecute";     break;
    case StartupRegistry::StartupSource::AppInitDlls:        src = L"AppInit_DLLs";    break;
    default:                                              src = L"?";              break;
    }
    out.label = scope + L" \\ " + src;
    return true;
}

static std::wstring FormatStartupSourceDisplay(const StartupRegistry::StartupEntry& entry) {
    const std::wstring reg = entry.regPath;
    const std::wstring flow = (entry.root == HKEY_CURRENT_USER || entry.scope == StartupRegistry::StartupScope::User)
        ? L"HKCU" : (entry.root == HKEY_USERS ? L"HKU\\.DEFAULT" : L"HKLM");

    if (reg.find(L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Run") != std::wstring::npos ||
        reg.find(L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce") != std::wstring::npos) {
        std::wstring suffix = (reg.find(L"RunOnce") != std::wstring::npos) ? L"Once" : L"";
        return std::wstring(L"HKLM\\Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run") + suffix;
    }

    if (reg.find(L"Software\\Microsoft\\Windows\\CurrentVersion\\Run") != std::wstring::npos ||
        reg.find(L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce") != std::wstring::npos) {
        std::wstring suffix = (reg.find(L"RunOnce") != std::wstring::npos) ? L"Once" : L"";
        return flow + std::wstring(L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Run") + suffix;
    }

    if (reg.find(L"Schedule\\TaskCache") != std::wstring::npos || reg.find(L"TaskCache") != std::wstring::npos) {
        return L"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache";
    }
    if (reg.find(L"CurrentControlSet\\Services") != std::wstring::npos) {
        return L"HKLM\\SYSTEM\\CurrentControlSet\\Services";
    }
    if (reg.find(L"CurrentControlSet\\Control\\Class") != std::wstring::npos) {
        return L"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Class";
    }
    if (reg.find(L"Uninstall") != std::wstring::npos) {
        return L"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    }
    if (reg.find(L"Winlogon") != std::wstring::npos) {
        return L"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
    }
    if (reg.find(L"Session Manager") != std::wstring::npos) {
        return L"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager";
    }
    if (reg.find(L"Active Setup") != std::wstring::npos || reg.find(L"Installed Components") != std::wstring::npos) {
        return L"HKLM\\Software\\Microsoft\\Active Setup\\Installed Components";
    }

    std::wstring path = entry.command;
    if (!path.empty()) {
        if (path.find(L"\\Microsoft\\Windows\\Start Menu\\Programs\\Startup") != std::wstring::npos ||
            path.find(L"\\Start Menu\\Programs\\Startup") != std::wstring::npos) {
            return (entry.scope == StartupRegistry::StartupScope::Machine)
                ? L"%ProgramData%\\Microsoft\\Windows\\Start Menu\\Programs\\StartUp"
                : L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
        }
        if (path.find(L"\\Windows\\System32\\Tasks") != std::wstring::npos ||
            path.find(L"\\System32\\Tasks") != std::wstring::npos) {
            return L"C:\\Windows\\System32\\Tasks";
        }
        if (path.find(L"\\System32\\drivers") != std::wstring::npos || path.find(L"\\drivers\\") != std::wstring::npos) {
            return L"C:\\Windows\\System32\\drivers";
        }
        if (path.find(L"\\System32\\dllcache") != std::wstring::npos) {
            return L"C:\\Windows\\System32\\dllcache";
        }
        if (path.find(L"\\WinSxS") != std::wstring::npos) {
            return L"C:\\Windows\\WinSxS";
        }
        if (path.find(L"\\Program Files") != std::wstring::npos || path.find(L"\\ProgramData") != std::wstring::npos ||
            path.find(L"\\AppData\\") != std::wstring::npos || path.find(L"\\LocalAppData\\") != std::wstring::npos) {
            return path;
        }
    }

    return StartupRegistry::SourceToString(entry.source);
}

static void ExecuteServiceCommand(int cmd, const ServiceManager::ServiceInfo& svc) {
    if (g_servicesOffline) {
        if (g_offlineMounts.systemMount.empty()) { DoRefresh(); return; }
        switch (cmd) {
        case IDM_SVC_AUTO:
            OfflineServiceManager::SetStartType(
                g_offlineMounts.systemMount, svc.name, SERVICE_AUTO_START);
            break;
        case IDM_SVC_MANUAL:
            OfflineServiceManager::SetStartType(
                g_offlineMounts.systemMount, svc.name, SERVICE_DEMAND_START);
            break;
        case IDM_SVC_DISABLED:
            OfflineServiceManager::SetStartType(
                g_offlineMounts.systemMount, svc.name, SERVICE_DISABLED);
            break;
        default: break;
        }
        DoRefresh();
        return;
    }
    switch (cmd) {
    case IDM_SVC_START:
        ServiceManager::StartService(svc.name);
        break;
    case IDM_SVC_STOP:
        ServiceManager::StopService(svc.name);
        break;
    case IDM_SVC_RESTART: {
        ServiceManager::StopService(svc.name);
        Sleep(500);
        ServiceManager::StartService(svc.name);
        break;
    }
    case IDM_SVC_AUTO:
        ServiceManager::SetStartType(svc.name, ServiceManager::ServiceStartType::Auto);
        break;
    case IDM_SVC_MANUAL:
        ServiceManager::SetStartType(svc.name, ServiceManager::ServiceStartType::Demand);
        break;
    case IDM_SVC_DISABLED:
        ServiceManager::SetStartType(svc.name, ServiceManager::ServiceStartType::Disabled);
        break;
    case IDM_SVC_EDIT:
        ServiceEditDialog::ShowEdit(App::Instance()->GetHWND(), svc.name);
        break;
    }
    DoRefresh();
}

static void EditStartupEntry(const StartupRegistry::StartupEntry& entry) {
    HWND hwnd = App::Instance()->GetHWND();

    StartupEditDialog::Location loc;
    if (!ResolveStartupLocation(entry, loc)) {
        MessageBoxW(hwnd, L"Источник не поддерживает редактирование.", L"Инфо", MB_OK);
        return;
    }
    if (StartupEditDialog::ShowEdit(hwnd, loc, entry.valueName)) {
        g_forceRefresh = true;
        RefreshStartupList();
        InvalidateRect(hwnd, nullptr, TRUE);
        DoRefresh();
    }
}

static bool TryDeleteStartupFile(const std::wstring& command) {
    if (command.empty()) return false;

    std::wstring path = command;
    if (!path.empty() && path[0] == L'"') {
        size_t endQuote = path.find(L'"', 1);
        if (endQuote != std::wstring::npos) {
            path = path.substr(1, endQuote - 1);
        }
    }
    else {
        size_t spacePos = path.find(L' ');
        if (spacePos != std::wstring::npos) path = path.substr(0, spacePos);
    }

    if (path.empty()) return false;
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return false;
    return DeleteFileW(path.c_str()) != FALSE;
}

static void ExecuteStartupCommand(int cmd, const StartupRegistry::StartupEntry& entry) {
    HWND hwnd = App::Instance()->GetHWND();
    switch (cmd) {
    case IDM_STARTUP_OPEN_LOC: {
        std::wstring path = entry.command;
        if (!path.empty() && path[0] == L'"') {
            size_t endQuote = path.find(L'"', 1);
            if (endQuote != std::wstring::npos) path = path.substr(1, endQuote - 1);
        }
        else {
            size_t spacePos = path.find(L' ');
            if (spacePos != std::wstring::npos) path = path.substr(0, spacePos);
        }
        OpenPathInExplorer(path);
        break;
    }
    case IDM_STARTUP_DISABLE:
        StartupRegistry::DisableEntry(entry);
        DoRefresh();
        break;
    case IDM_STARTUP_ENABLE:
        StartupRegistry::EnableEntry(entry);
        DoRefresh();
        break;
    case IDM_STARTUP_REMOVE: {
        if (entry.isCritical && entry.source == StartupRegistry::StartupSource::Winlogon &&
            (StartupRegistry::IsCriticalWinlogonValue(entry.valueName) ||
             _wcsicmp(entry.valueName.c_str(), L"Shell") == 0 ||
             _wcsicmp(entry.valueName.c_str(), L"Userinit") == 0)) {
            if (StartupRegistry::RestoreWinlogonSafeDefaults(entry.root, entry.regPath, entry.view)) {
                MessageBoxW(hwnd, L"Критические значения Winlogon восстановлены до безопасных значений.",
                    L"Автозагрузка", MB_OK | MB_ICONINFORMATION);
                DoRefresh();
            }
            break;
        }
        if (entry.isCritical) {
            MessageBoxW(hwnd, L"Нельзя удалить критическую запись Winlogon.", L"Ошибка", MB_OK | MB_ICONERROR);
            break;
        }
        std::wstring msg = L"Удалить запись автозагрузки?\r\n\r\n" + entry.valueName;
        if (MessageBoxW(hwnd, msg.c_str(), L"Удаление", MB_YESNO | MB_ICONWARNING) == IDYES) {
            bool ok = StartupRegistry::RemoveEntry(entry);
            if (ok) {
                TryDeleteStartupFile(entry.command);
            }
            DoRefresh();
        }
        break;
    }
    case IDM_STARTUP_CLEAR: {
        if (StartupRegistry::ClearEntryCommand(entry)) {
            DoRefresh();
        }
        else {
            MessageBoxW(hwnd, L"Не удалось очистить параметр.", L"Ошибка", MB_OK | MB_ICONERROR);
        }
        break;
    }
    case IDM_STARTUP_REMOVE_ALL: {
        std::wstring msg = L"Удалить все записи этого раздела автозагрузки?\r\n\r\n" +
            StartupRegistry::SourceToString(entry.source);
        if (MessageBoxW(hwnd, msg.c_str(), L"Удаление всех записей", MB_YESNO | MB_ICONWARNING) != IDYES)
            break;

        int removed = 0;
        for (const auto& item : g_startupEntries) {
            if (item.source != entry.source) continue;
            if (item.root != entry.root) continue;
            if (item.regPath != entry.regPath) continue;
            if (item.isCritical && item.source == StartupRegistry::StartupSource::Winlogon &&
                (StartupRegistry::IsCriticalWinlogonValue(item.valueName) ||
                 _wcsicmp(item.valueName.c_str(), L"Shell") == 0 ||
                 _wcsicmp(item.valueName.c_str(), L"Userinit") == 0)) {
                if (StartupRegistry::RestoreWinlogonSafeDefaults(item.root, item.regPath, item.view)) ++removed;
                continue;
            }
            if (!item.isCritical && StartupRegistry::RemoveEntry(item)) {
                ++removed;
                TryDeleteStartupFile(item.command);
            }
        }

        if (removed > 0) {
            RefreshStartupList();
            DoRefresh();
        }
        else {
            MessageBoxW(hwnd, L"Нечего удалять в этом разделе.", L"Автозагрузка", MB_OK | MB_ICONINFORMATION);
        }
        break;
    }
    case IDM_STARTUP_EDIT:
        EditStartupEntry(entry);
        break;
    }
}

static void ExecuteDriverCommand(int cmd, const DriverDisplayInfo& drv) {
    HWND hwnd = App::Instance()->GetHWND();

    if (g_driversOffline) {
        if (g_offlineMounts.systemMount.empty()) { DoRefresh(); return; }
        switch (cmd) {
        case IDM_DRV_BOOT:
            OfflineServiceManager::SetStartType(
                g_offlineMounts.systemMount, drv.serviceName, SERVICE_BOOT_START);
            break;
        case IDM_DRV_SYSTEM:
            OfflineServiceManager::SetStartType(
                g_offlineMounts.systemMount, drv.serviceName, SERVICE_SYSTEM_START);
            break;
        case IDM_DRV_AUTO:
            OfflineServiceManager::SetStartType(
                g_offlineMounts.systemMount, drv.serviceName, SERVICE_AUTO_START);
            break;
        case IDM_DRV_MANUAL:
            OfflineServiceManager::SetStartType(
                g_offlineMounts.systemMount, drv.serviceName, SERVICE_DEMAND_START);
            break;
        case IDM_DRV_DISABLED:
            OfflineServiceManager::SetStartType(
                g_offlineMounts.systemMount, drv.serviceName, SERVICE_DISABLED);
            break;
        case IDM_DRV_OPEN_LOC:
            if (!drv.imagePath.empty())
                OpenPathInExplorer(drv.imagePath);
            break;
        case IDM_DRV_COPY_PATH:
            if (!drv.imagePath.empty()) CopyToClipboard(drv.imagePath);
            break;
        case IDM_DRV_VERIFY_SIG: {
            if (drv.imagePath.empty()) break;
            std::wstring signer;
            bool isMs = false, isTrusted = false;
            auto st = WinCtrl::Scanner::VerifySignature(
                drv.imagePath, &signer, nullptr, &isMs, &isTrusted, true);
            std::wstring status;
            switch (st) {
            case SigStatus::Valid:    status = L"✓ Подписан"; break;
            case SigStatus::Unsigned: status = L"— Не подписан"; break;
            case SigStatus::Invalid:  status = L"✗ Невалидна"; break;
            case SigStatus::Error:    status = L"? Ошибка"; break;
            }
            std::wstring msg = L"Файл: " + drv.imagePath + L"\r\n\r\n";
            msg += L"Статус: " + status + L"\r\n";
            if (!signer.empty()) msg += L"Подписант: " + signer + L"\r\n";
            if (isMs) msg += L"Microsoft: да\r\n";
            MessageBoxW(hwnd, msg.c_str(), L"Подпись драйвера",
                MB_OK | MB_ICONINFORMATION);
            break;
        }
        default: break;
        }
        DoRefresh();
        return;
    }
    switch (cmd) {
    case IDM_DRV_OPEN_LOC: {
        if (drv.imagePath.empty()) {
            MessageBoxW(hwnd, L"Путь к файлу неизвестен.", L"Инфо", MB_OK);
            break;
        }
        ShellExecuteW(nullptr, L"open", L"explorer.exe",
            (L"/select,\"" + drv.imagePath + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        break;
    }
    case IDM_DRV_COPY_PATH:
        if (!drv.imagePath.empty()) CopyToClipboard(drv.imagePath);
        break;

    case IDM_DRV_VERIFY_SIG: {
        if (drv.imagePath.empty()) {
            MessageBoxW(hwnd, L"Путь к файлу неизвестен.", L"Инфо", MB_OK);
            break;
        }
        std::wstring signer;
        bool isMs = false, isTrusted = false;
        auto st = WinCtrl::Scanner::VerifySignature(
            drv.imagePath, &signer, nullptr, &isMs, &isTrusted, true);

        std::wstring status;
        switch (st) {
        case SigStatus::Valid:    status = L"✓ Подписан"; break;
        case SigStatus::Unsigned: status = L"— Не подписан"; break;
        case SigStatus::Invalid:  status = L"✗ Невалидна"; break;
        case SigStatus::Error:    status = L"? Ошибка"; break;
        }

        std::wstring msg = L"Файл: " + drv.imagePath + L"\r\n\r\n";
        msg += L"Статус: " + status + L"\r\n";
        if (!signer.empty()) msg += L"Подписант: " + signer + L"\r\n";
        if (isMs) msg += L"Microsoft: да\r\n";
        MessageBoxW(hwnd, msg.c_str(), L"Подпись драйвера", MB_OK | MB_ICONINFORMATION);
        break;
    }

    case IDM_DRV_EDIT:
        if (ServiceEditDialog::ShowEdit(hwnd, drv.serviceName, true))
            DoRefresh();
        break;

    case IDM_DRV_START:
        ServiceManager::StartService(drv.serviceName);
        DoRefresh();
        break;
    case IDM_DRV_STOP:
        ServiceManager::StopService(drv.serviceName);
        DoRefresh();
        break;
    }
}

static void InitColumnWidths(const RectF& listArea) {
    if (g_colWidthsInitialized && fabsf(g_lastColumnAreaWidth - listArea.Width) < 1.0f) return;
    float w = listArea.Width;
    g_procColWidths[0] = w * 0.18f;
    g_procColWidths[1] = w * 0.07f;
    g_procColWidths[2] = w * 0.16f;
    g_procColWidths[3] = w * 0.11f;
    g_procColWidths[4] = w * 0.11f;
    g_procColWidths[5] = w - (g_procColWidths[0] + g_procColWidths[1] +
        g_procColWidths[2] + g_procColWidths[3] + g_procColWidths[4]);

    g_svcColWidths[0] = w * 0.40f;
    g_svcColWidths[1] = 120.0f;
    g_svcColWidths[2] = 120.0f;

    g_startupColWidths[0] = w * 0.22f;
    g_startupColWidths[1] = w * 0.48f;
    g_startupColWidths[2] = w * 0.18f;
    g_startupColWidths[3] = w * 0.12f;

    g_driverColWidths[0] = 150.0f;
    g_driverColWidths[1] = 180.0f;
    g_driverColWidths[2] = 80.0f;
    g_driverColWidths[3] = 100.0f;
    g_driverColWidths[4] = w * 0.35f;
    g_driverColWidths[5] = 40.0f;

    g_colWidthsInitialized = true;
    g_lastColumnAreaWidth = w;
}

static int GetColumnResizeHit(float fx, float fy, const RectF& contentArea) {
    TaskmgrLayout L = GetLayout(contentArea, false);
    int tab = g_activeSubTab;

    if (fy < L.listArea.Y || fy > L.listArea.Y + 24.0f + 6.0f) return -1;
    if (fx < L.listArea.X || fx > L.listArea.X + L.listArea.Width) return -1;

    float colX = L.listArea.X + 6.0f;
    int colCount = 0;
    float* widths = nullptr;

    if (tab == SUBTAB_PROCESSES) { widths = g_procColWidths; colCount = 6; }
    else if (tab == SUBTAB_STARTUP || tab == SUBTAB_SUSPICIOUS) { widths = g_startupColWidths; colCount = 4; }
    else if (tab == SUBTAB_SERVICES) { widths = g_svcColWidths; colCount = 3; }
    else if (tab == SUBTAB_DRIVERS) { widths = g_driverColWidths; colCount = 6; }
    else return -1;

    for (int i = 0; i < colCount; ++i) {
        float rightEdge = colX + widths[i];
        if (fx >= rightEdge - RESIZE_HIT_ZONE && fx <= rightEdge + RESIZE_HIT_ZONE) {
            return i;
        }
        colX += widths[i];
    }
    return -1;
}

static int GetProcessRowAt(float fx, float fy, const RectF& contentArea) {
    TaskmgrLayout L = GetLayout(contentArea, false);
    if (fx < L.listArea.X || fx > L.listArea.X + L.listArea.Width ||
        fy < L.listDataTop || fy > L.listArea.Y + L.listArea.Height) return -1;
    auto filtered = GetFilteredProcessIndices();
    int scrollOffset = g_scrollOffset[1];
    int startRow = scrollOffset / (int)ROW_HEIGHT;
    float relativeY = fy - L.listDataTop + (scrollOffset % (int)ROW_HEIGHT);
    if (relativeY < 0) return -1;
    int row = startRow + (int)(relativeY / ROW_HEIGHT);
    if (row >= 0 && row < (int)filtered.size()) return filtered[row];
    return -1;
}

static void UpdateProcessPathTooltip(int x, int y, const RectF& contentArea) {
    TaskmgrLayout layout = GetLayout(contentArea, false);
    float pathX = layout.listArea.X + 6.0f;
    for (int i = 0; i < 5; ++i) pathX += g_procColWidths[i];
    float pathEnd = pathX + g_procColWidths[5];

    bool overPath = g_activeSubTab == SUBTAB_PROCESSES &&
        x >= pathX && x <= pathEnd &&
        y >= layout.listDataTop && y <= layout.listArea.Y + layout.listArea.Height;
    int row = overPath ? GetProcessRowAt((float)x, (float)y, contentArea) : -1;
    if (row < 0 || row >= (int)g_processes.size() || g_processes[row].fullPath.empty()) {
        if (g_pathTooltip) {
            TOOLINFO ti{};
            ti.cbSize = sizeof(ti);
            ti.hwnd = App::Instance()->GetHWND();
            ti.uId = 1;
            SendMessageW(g_pathTooltip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
        }
        return;
    }

    if (!g_pathTooltip) {
        g_pathTooltip = CreateWindowExW(
            0, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            App::Instance()->GetHWND(), nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!g_pathTooltip) return;

        SendMessageW(g_pathTooltip, TTM_SETMAXTIPWIDTH, 0, 900);
        SendMessageW(g_pathTooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 150);
    }

    g_pathTooltipText = g_processes[row].fullPath;
    TOOLINFO ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = App::Instance()->GetHWND();
    ti.uId = 1;
    ti.lpszText = const_cast<LPWSTR>(g_pathTooltipText.c_str());
    ti.rect = { 0, 0, 1, 1 };
    SendMessageW(g_pathTooltip, TTM_DELTOOL, 0, (LPARAM)&ti);
    SendMessageW(g_pathTooltip, TTM_ADDTOOL, 0, (LPARAM)&ti);

    POINT screenPoint{ x + 12, y + 18 };
    ClientToScreen(App::Instance()->GetHWND(), &screenPoint);
    SendMessageW(g_pathTooltip, TTM_TRACKPOSITION, 0,
        MAKELONG(screenPoint.x, screenPoint.y));
    SendMessageW(g_pathTooltip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
}

static int GetServiceRowAt(float fx, float fy, const RectF& contentArea) {
    TaskmgrLayout L = GetLayout(contentArea, false);
    if (fx < L.listArea.X || fx > L.listArea.X + L.listArea.Width ||
        fy < L.listDataTop || fy > L.listArea.Y + L.listArea.Height) return -1;
    auto filtered = GetFilteredServiceIndices();
    int scrollOffset = g_scrollOffset[1];
    int startRow = scrollOffset / (int)ROW_HEIGHT;
    float relativeY = fy - L.listDataTop + (scrollOffset % (int)ROW_HEIGHT);
    if (relativeY < 0) return -1;
    int row = startRow + (int)(relativeY / ROW_HEIGHT);
    if (row >= 0 && row < (int)filtered.size()) return filtered[row];
    return -1;
}

static int GetStartupRowAt(float fx, float fy, const RectF& contentArea) {
    TaskmgrLayout L = GetLayout(contentArea, false);
    if (fx < L.listArea.X || fx > L.listArea.X + L.listArea.Width ||
        fy < L.listDataTop || fy > L.listArea.Y + L.listArea.Height) return -1;
    bool suspiciousOnly = (g_activeSubTab == SUBTAB_SUSPICIOUS);
    auto filtered = GetFilteredStartupIndices(suspiciousOnly);
    int scrollOffset = g_scrollOffset[1];
    int startRow = scrollOffset / (int)ROW_HEIGHT;
    float relativeY = fy - L.listDataTop + (scrollOffset % (int)ROW_HEIGHT);
    if (relativeY < 0) return -1;
    int row = startRow + (int)(relativeY / ROW_HEIGHT);
    if (row >= 0 && row < (int)filtered.size()) return filtered[row];
    return -1;
}

static int GetDriverRowAt(float fx, float fy, const RectF& contentArea) {
    TaskmgrLayout L = GetLayout(contentArea, false);
    if (fx < L.listArea.X || fx > L.listArea.X + L.listArea.Width ||
        fy < L.listDataTop || fy > L.listArea.Y + L.listArea.Height) return -1;
    auto filtered = GetFilteredDriverIndices();
    int scrollOffset = g_scrollOffset[1];
    int startRow = scrollOffset / (int)ROW_HEIGHT;
    float relativeY = fy - L.listDataTop + (scrollOffset % (int)ROW_HEIGHT);
    if (relativeY < 0) return -1;
    int row = startRow + (int)(relativeY / ROW_HEIGHT);
    if (row >= 0 && row < (int)filtered.size()) return filtered[row];
    return -1;
}

static SortColumn GetColumnAtHeader(float fx, float fy, const RectF& contentArea) {
    TaskmgrLayout L = GetLayout(contentArea, false);
    if (fy < L.listArea.Y || fy > L.listArea.Y + 24.0f) return SortColumn::None;
    if (fx < L.listArea.X || fx > L.listArea.X + L.listArea.Width) return SortColumn::None;
    if (g_activeSubTab != SUBTAB_PROCESSES) return SortColumn::None;

    float colX = L.listArea.X + 6.0f;
    SortColumn cols[] = { SortColumn::Name, SortColumn::Pid, SortColumn::User,
        SortColumn::None, SortColumn::None, SortColumn::None };
    for (int i = 0; i < 6; ++i) {
        if (fx >= colX && fx <= colX + g_procColWidths[i]) return cols[i];
        colX += g_procColWidths[i];
    }
    return SortColumn::None;
}

static void DrawButton(Graphics& g, const RectF& r, const std::wstring& text,
    Font& font, SolidBrush& bg, SolidBrush& txt, Pen& pen, bool checked = false) {
    SolidBrush* useBg = &bg;
    SolidBrush checkedBg(COLOR_TAB_ACTIVE);
    if (checked) useBg = &checkedBg;
    g.FillRectangle(useBg, r);
    g.DrawRectangle(&pen, r);
    StringFormat f; f.SetAlignment(StringAlignmentCenter); f.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text.c_str(), -1, &font, r, &f, &txt);
}

static void DrawScrollableRows(
    Graphics& g, const RectF& listArea, float listDataTop,
    int totalRows, int& scrollOffset,
    std::function<void(int, float, bool)> drawRow
) {
    float visibleHeight = listArea.Height - 24.0f - 12.0f;
    if (visibleHeight < 0.0f) visibleHeight = 0.0f;
    int visibleRows = (int)(visibleHeight / ROW_HEIGHT);
    int maxScroll = (totalRows > visibleRows) ? (totalRows - visibleRows) * (int)ROW_HEIGHT : 0;
    g_maxScroll[1] = maxScroll;
    if (scrollOffset > maxScroll) scrollOffset = maxScroll;
    if (scrollOffset < 0) scrollOffset = 0;
    g_scrollOffset[1] = scrollOffset;

    int startRow = scrollOffset / (int)ROW_HEIGHT;
    int endRow = (std::min)(startRow + visibleRows + 1, totalRows);
    float dataY = listDataTop;

    for (int vi = startRow; vi < endRow; ++vi) {
        float rowY = dataY + (vi - startRow) * ROW_HEIGHT - (scrollOffset % (int)ROW_HEIGHT);
        if (rowY + ROW_HEIGHT > listArea.Y + listArea.Height) break;
        bool alternate = (vi % 2 == 1);
        drawRow(vi, rowY, alternate);
    }
}

static void DrawProcesses(Graphics& g, const TaskmgrLayout& L,
    Font& headFont, Font& itemFont, Font& smallFont,
    SolidBrush& textBrush, SolidBrush& mutedBrush, SolidBrush& selBrush) {

    auto filtered = GetFilteredProcessIndices();

    float colX = L.listArea.X + 6.0f;
    float headerY = L.listArea.Y + 4.0f;
    StringFormat headerFormat;
    headerFormat.SetAlignment(StringAlignmentNear);
    headerFormat.SetLineAlignment(StringAlignmentCenter);
    headerFormat.SetTrimming(StringTrimmingEllipsisCharacter);

    struct ColDef { SortColumn col; const wchar_t* name; };
    ColDef cols[] = {
        {SortColumn::Name, IsLikelyRecoveryEnvironment() ? L"Процесс WinRE" : L"Процесс"},
        {SortColumn::Pid, L"PID"},
        {SortColumn::User, L"Пользователь"}, {SortColumn::None, L"Риск"},
        {SortColumn::None, L"Статус"}, {SortColumn::None, L"Полный путь"},
    };

    for (int i = 0; i < 6; ++i) {
        RectF hr(colX, headerY, g_procColWidths[i], 24.0f);
        std::wstring header = cols[i].name;
        if (cols[i].col != SortColumn::None && g_sortColumn == cols[i].col)
            header += g_sortAscending ? L" ▲" : L" ▼";
        g.DrawString(header.c_str(), -1, &headFont, hr, &headerFormat, &textBrush);
        colX += g_procColWidths[i];
    }

    Pen sepPen(COLOR_BORDER, 1.0f);
    g.DrawLine(&sepPen, L.listArea.X, L.listDataTop - 4.0f,
        L.listArea.X + L.listArea.Width, L.listDataTop - 4.0f);

    SolidBrush criticalBrush(Color(255, 245, 75, 75));
    SolidBrush suspiciousBrush(Color(255, 245, 75, 75));
    SolidBrush systemBrush(Color(255, 155, 155, 155));

    int scrollOffset = g_scrollOffset[1];
    DrawScrollableRows(g, L.listArea, L.listDataTop, (int)filtered.size(), scrollOffset,
        [&](int vi, float rowY, bool alternate) {
            int realIdx = filtered[vi];
            const auto& proc = g_processes[realIdx];
            bool selected = (realIdx == g_selectedProcessIndex);

            RectF rowRect(L.listArea.X + 2.0f, rowY, L.listArea.Width - 4.0f, ROW_HEIGHT);
            if (selected) g.FillRectangle(&selBrush, rowRect);
            else if (alternate) {
                SolidBrush altBrush(Color(255, 38, 38, 38));
                g.FillRectangle(&altBrush, rowRect);
            }
            float cx = L.listArea.X + 6.0f;
            StringFormat f; f.SetAlignment(StringAlignmentNear); f.SetLineAlignment(StringAlignmentCenter);
            f.SetTrimming(StringTrimmingEllipsisCharacter);
            f.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsLineLimit);

            RectF c1(cx, rowY, g_procColWidths[0], ROW_HEIGHT);
            std::wstring name = proc.name;
            int depth = 0;
            DWORD parent = proc.parentPid;
            while (parent != 0 && depth < 8) {
                auto it = std::find_if(g_processes.begin(), g_processes.end(),
                    [parent](const ProcessDisplayInfo& p) { return p.pid == parent; });
                if (it == g_processes.end()) break;
                ++depth;
                parent = it->parentPid;
            }
            name = std::wstring(depth * 2, L' ') + (depth ? L"|- " : L"") + name;
            SolidBrush* nameBrush = proc.critical ? &criticalBrush
                : (proc.riskScore >= 5 ? &suspiciousBrush
                    : (proc.fullPath.find(L"\\Windows\\") == std::wstring::npos ? &suspiciousBrush : &systemBrush));

            g.DrawString(name.c_str(), -1, &itemFont, c1, &f, nameBrush);
            cx += g_procColWidths[0];

            RectF c2(cx, rowY, g_procColWidths[1], ROW_HEIGHT);
            g.DrawString(std::to_wstring(proc.pid).c_str(), -1, &smallFont, c2, &f, &mutedBrush);
            cx += g_procColWidths[1];

            RectF c3(cx, rowY, g_procColWidths[2], ROW_HEIGHT);
            g.DrawString(proc.userName.empty() ? L"?" : proc.userName.c_str(), -1, &smallFont, c3, &f, &mutedBrush);
            cx += g_procColWidths[2];

            RectF c4(cx, rowY, g_procColWidths[3], ROW_HEIGHT);
            const wchar_t* risk = proc.critical ? L"КРИТИЧНЫЙ" :
                (proc.riskScore >= 5 ? L"ПОДОЗРИТЕЛЬНЫЙ" : L"НИЗКИЙ");
            g.DrawString(risk, -1, &smallFont, c4, &f,
                proc.critical || proc.riskScore >= 5 ? &criticalBrush : &systemBrush);
            cx += g_procColWidths[3];

            RectF c5(cx, rowY, g_procColWidths[4], ROW_HEIGHT);
            const wchar_t* status = proc.critical ? L"Критичный" : (proc.suspended ? L"Приостановлен" : L"Работает");
            g.DrawString(status, -1, &smallFont, c5, &f, proc.critical ? &criticalBrush : &textBrush);
            cx += g_procColWidths[4];

            RectF c6(cx, rowY, g_procColWidths[5], ROW_HEIGHT);
            GraphicsState state = g.Save();
            g.SetClip(c6);
            g.DrawString(proc.fullPath.empty() ? L"[доступ запрещен]" : proc.fullPath.c_str(),
                -1, &itemFont, RectF(c6.X - (REAL)g_pathScrollX, c6.Y,
                    c6.Width + (REAL)g_pathScrollX, c6.Height), &f,
                proc.fullPath.empty() ? &suspiciousBrush : &textBrush);
            g.Restore(state);
        });
    g_scrollOffset[1] = scrollOffset;
}

static void DrawStartup(Graphics& g, const TaskmgrLayout& L,
    Font& headFont, Font& itemFont, Font& smallFont,
    SolidBrush& textBrush, SolidBrush& mutedBrush, SolidBrush& selBrush,
    bool suspiciousOnly = false) {

    auto filtered = GetFilteredStartupIndices(suspiciousOnly);

    float colX = L.listArea.X + 6.0f;
    float headerY = L.listArea.Y + 4.0f;
    StringFormat hf; hf.SetAlignment(StringAlignmentNear); hf.SetLineAlignment(StringAlignmentCenter);
    hf.SetTrimming(StringTrimmingEllipsisCharacter);

    const wchar_t* headers[] = { L"Имя", L"Команда", L"Источник", L"Статус" };
    for (int i = 0; i < 4; ++i) {
        RectF hr(colX, headerY, g_startupColWidths[i], 24.0f);
        g.DrawString(headers[i], -1, &headFont, hr, &hf, &textBrush);
        colX += g_startupColWidths[i];
    }

    Pen sepPen(COLOR_BORDER, 1.0f);
    g.DrawLine(&sepPen, L.listArea.X, L.listDataTop - 4.0f,
        L.listArea.X + L.listArea.Width, L.listDataTop - 4.0f);

    SolidBrush dangerBrush(Color(255, 240, 80, 80));
    SolidBrush dangerMuted(Color(255, 160, 80, 80));

    int scrollOffset = g_scrollOffset[1];
    DrawScrollableRows(g, L.listArea, L.listDataTop, (int)filtered.size(), scrollOffset,
        [&](int vi, float rowY, bool alternate) {
            int realIdx = filtered[vi];
            const auto& entry = g_startupEntries[realIdx];
            bool selected = (realIdx == g_selectedStartupIndex);
            bool suspicious = IsStartupEntrySuspicious(entry);
            RectF rowRect(L.listArea.X + 2.0f, rowY, L.listArea.Width - 4.0f, ROW_HEIGHT);
            if (selected) g.FillRectangle(&selBrush, rowRect);
            else if (alternate) {
                SolidBrush altBrush(Color(255, 38, 38, 38));
                g.FillRectangle(&altBrush, rowRect);
            }
            if (suspicious) {
                SolidBrush dangerRow(Color(255, 48, 20, 20));
                g.FillRectangle(&dangerRow, rowRect);
            }
            float cx = L.listArea.X + 6.0f;
            StringFormat f; f.SetAlignment(StringAlignmentNear); f.SetLineAlignment(StringAlignmentCenter);
            f.SetTrimming(StringTrimmingEllipsisCharacter);

            RectF c1(cx, rowY, g_startupColWidths[0], ROW_HEIGHT);
            g.DrawString(entry.valueName.c_str(), -1, &itemFont, c1, &f, suspicious ? &dangerBrush : &textBrush);
            cx += g_startupColWidths[0];

            RectF c2(cx, rowY, g_startupColWidths[1], ROW_HEIGHT);
            GraphicsState clipState = g.Save();
            g.SetClip(c2);
            g.DrawString(entry.command.c_str(), -1, &smallFont, c2, &f, suspicious ? &dangerMuted : &mutedBrush);
            g.Restore(clipState);
            cx += g_startupColWidths[1];

            RectF c3(cx, rowY, g_startupColWidths[2], ROW_HEIGHT);
            std::wstring sourceStr = FormatStartupSourceDisplay(entry);
            g.DrawString(sourceStr.c_str(), -1, &smallFont, c3, &f, suspicious ? &dangerMuted : &mutedBrush);
            cx += g_startupColWidths[2];

            RectF c4(cx, rowY, g_startupColWidths[3], ROW_HEIGHT);
            SolidBrush enabledBrush(Color(255, 100, 220, 100));
            SolidBrush disabledBrush(Color(255, 200, 100, 100));
            SolidBrush* sBrush = entry.enabled ? &enabledBrush : &disabledBrush;
            std::wstring statusStr = entry.enabled ? L"Включено" : L"Отключено";
            if (entry.isCritical) statusStr += L" [CRIT]";
            else if (suspicious) statusStr = L"Подозрительно";
            g.DrawString(statusStr.c_str(), -1, &smallFont, c4, &f, suspicious ? &dangerBrush : sBrush);
        });
    g_scrollOffset[1] = scrollOffset;
}

static void DrawServices(Graphics& g, const TaskmgrLayout& L,
    Font& headFont, Font& itemFont, Font& smallFont,
    SolidBrush& textBrush, SolidBrush& mutedBrush, SolidBrush& selBrush) {

    auto filtered = GetFilteredServiceIndices();

    float colX = L.listArea.X + 6.0f;
    float headerY = L.listArea.Y + 4.0f;
    StringFormat hf; hf.SetAlignment(StringAlignmentNear); hf.SetLineAlignment(StringAlignmentCenter);
    hf.SetTrimming(StringTrimmingEllipsisCharacter);

    const wchar_t* headers[] = { L"Служба", L"Статус", L"Тип запуска" };
    for (int i = 0; i < 3; ++i) {
        RectF hr(colX, headerY, g_svcColWidths[i], 24.0f);
        g.DrawString(headers[i], -1, &headFont, hr, &hf, &textBrush);
        colX += g_svcColWidths[i];
    }

    Pen sepPen(COLOR_BORDER, 1.0f);
    g.DrawLine(&sepPen, L.listArea.X, L.listDataTop - 4.0f,
        L.listArea.X + L.listArea.Width, L.listDataTop - 4.0f);

    int scrollOffset = g_scrollOffset[1];
    DrawScrollableRows(g, L.listArea, L.listDataTop, (int)filtered.size(), scrollOffset,
        [&](int vi, float rowY, bool alternate) {
            int realIdx = filtered[vi];
            const auto& svc = g_services[realIdx];
            bool selected = (realIdx == g_selectedServiceIndex);
            RectF rowRect(L.listArea.X + 2.0f, rowY, L.listArea.Width - 4.0f, ROW_HEIGHT);
            if (selected) g.FillRectangle(&selBrush, rowRect);
            else if (alternate) {
                SolidBrush altBrush(Color(255, 38, 38, 38));
                g.FillRectangle(&altBrush, rowRect);
            }
            float cx = L.listArea.X + 6.0f;
            StringFormat f; f.SetAlignment(StringAlignmentNear); f.SetLineAlignment(StringAlignmentCenter);
            f.SetTrimming(StringTrimmingEllipsisCharacter);

            RectF c1(cx, rowY, g_svcColWidths[0], ROW_HEIGHT);
            g.DrawString(svc.displayName.c_str(), -1, &itemFont, c1, &f, &textBrush);
            cx += g_svcColWidths[0];

            RectF c2(cx, rowY, g_svcColWidths[1], ROW_HEIGHT);
            std::wstring status = g_servicesOffline
                ? std::wstring(L"—")
                : ServiceStatusToString(svc.state);
            SolidBrush runBrush(Color(255, 100, 220, 100));
            SolidBrush stopBrush(Color(255, 200, 100, 100));
            SolidBrush* statusBrush = &mutedBrush;
            if (svc.state == ServiceManager::ServiceState::Running) statusBrush = &runBrush;
            else if (svc.state == ServiceManager::ServiceState::Stopped) statusBrush = &stopBrush;
            g.DrawString(status.c_str(), -1, &smallFont, c2, &f, statusBrush);
            cx += g_svcColWidths[1];

            RectF c3(cx, rowY, g_svcColWidths[2], ROW_HEIGHT);
            g.DrawString(ServiceStartTypeToString(svc.startType).c_str(), -1, &smallFont, c3, &f, &mutedBrush);
        });
    g_scrollOffset[1] = scrollOffset;
}

static void DrawDrivers(Graphics& g, const TaskmgrLayout& L,
    Font& headFont, Font& itemFont, Font& smallFont,
    SolidBrush& textBrush, SolidBrush& mutedBrush, SolidBrush& selBrush) {

    auto filtered = GetFilteredDriverIndices();

    float colX = L.listArea.X + 6.0f;
    float headerY = L.listArea.Y + 4.0f;
    StringFormat hf; hf.SetAlignment(StringAlignmentNear); hf.SetLineAlignment(StringAlignmentCenter);
    hf.SetTrimming(StringTrimmingEllipsisCharacter);

    const wchar_t* headers[] = { L"Драйвер", L"Отображаемое имя", L"Старт", L"Состояние", L"Путь", L"Sig" };
    for (int i = 0; i < 6; ++i) {
        RectF hr(colX, headerY, g_driverColWidths[i], 24.0f);
        g.DrawString(headers[i], -1, &headFont, hr, &hf, &textBrush);
        colX += g_driverColWidths[i];
    }

    Pen sepPen(COLOR_BORDER, 1.0f);
    g.DrawLine(&sepPen, L.listArea.X, L.listDataTop - 4.0f,
        L.listArea.X + L.listArea.Width, L.listDataTop - 4.0f);

    SolidBrush sigValid(Color(255, 100, 220, 100));
    SolidBrush sigUnsigned(Color(255, 160, 160, 160));
    SolidBrush sigInvalid(Color(255, 240, 80, 80));

    int scrollOffset = g_scrollOffset[1];
    DrawScrollableRows(g, L.listArea, L.listDataTop, (int)filtered.size(), scrollOffset,
        [&](int vi, float rowY, bool alternate) {
            int realIdx = filtered[vi];
            const auto& drv = g_drivers[realIdx];
            bool selected = (realIdx == g_selectedDriverIndex);

            // Подсветка строки по вердикту подписи
            Color rowBg = Color(0, 0, 0, 0);
            bool hasBg = false;

            if (drv.sigChecked) {
                if (drv.sigInvalid || (!drv.sigValid && drv.sigMicrosoft)) {
                    // битая подпись Microsoft
                    rowBg = Color(60, 240, 80, 80);
                    hasBg = true;
                }
                else if (!drv.sigValid && !drv.sigMicrosoft) {
                    // не подписан
                    rowBg = Color(40, 255, 200, 80);
                    hasBg = true;
                }
            }

            RectF rowRect(L.listArea.X + 2.0f, rowY, L.listArea.Width - 4.0f, ROW_HEIGHT);
            if (selected) g.FillRectangle(&selBrush, rowRect);
            else if (hasBg) {
                SolidBrush bgBrush(rowBg);
                g.FillRectangle(&bgBrush, rowRect);
            }
            else if (alternate) {
                SolidBrush altBrush(Color(255, 38, 38, 38));
                g.FillRectangle(&altBrush, rowRect);
            }

            float cx = L.listArea.X + 6.0f;
            StringFormat f; f.SetAlignment(StringAlignmentNear); f.SetLineAlignment(StringAlignmentCenter);
            f.SetTrimming(StringTrimmingEllipsisCharacter);

            RectF c1(cx, rowY, g_driverColWidths[0], ROW_HEIGHT);
            g.DrawString(drv.serviceName.c_str(), -1, &itemFont, c1, &f, &textBrush);
            cx += g_driverColWidths[0];

            RectF c2(cx, rowY, g_driverColWidths[1], ROW_HEIGHT);
            g.DrawString(drv.displayName.c_str(), -1, &smallFont, c2, &f, &mutedBrush);
            cx += g_driverColWidths[1];

            RectF c3(cx, rowY, g_driverColWidths[2], ROW_HEIGHT);
            g.DrawString(DriverStartTypeToString(drv.startType).c_str(), -1, &smallFont, c3, &f, &mutedBrush);
            cx += g_driverColWidths[2];

            RectF c4(cx, rowY, g_driverColWidths[3], ROW_HEIGHT);
            SolidBrush runBrush(Color(255, 100, 220, 100));
            SolidBrush* stateBrush = (drv.state == SERVICE_RUNNING) ? &runBrush : &mutedBrush;
            std::wstring stateStr = g_driversOffline
                ? std::wstring(L"—")
                : DriverStateToString(drv.state);
            g.DrawString(stateStr.c_str(), -1, &smallFont, c4, &f, stateBrush);
            cx += g_driverColWidths[3];

            RectF c5(cx, rowY, g_driverColWidths[4], ROW_HEIGHT);
            g.DrawString(drv.imagePath.c_str(), -1, &smallFont, c5, &f, &mutedBrush);
            cx += g_driverColWidths[4];

            RectF c6(cx, rowY, g_driverColWidths[5], ROW_HEIGHT);
            if (!drv.sigChecked) {
                SolidBrush dot(Color(255, 90, 90, 90));
                g.FillEllipse(&dot, RectF(cx + 14.0f, rowY + 10.0f, 5.0f, 5.0f));
            }
            else {
                SolidBrush* b = &sigUnsigned;
                const wchar_t* sym = L"—";
                if (drv.sigValid) { b = &sigValid;    sym = L"✓"; }
                else if (drv.sigInvalid) { b = &sigInvalid; sym = L"✗"; }

                StringFormat cf;
                cf.SetAlignment(StringAlignmentCenter);
                cf.SetLineAlignment(StringAlignmentCenter);
                g.DrawString(sym, -1, &itemFont, c6, &cf, b);
            }
        });
    g_scrollOffset[1] = scrollOffset;
}

void DrawTaskManagerContent(Graphics& g, const RectF& contentArea, Font& contentFont) {
    (void)contentFont;
    SetTimer(App::Instance()->GetHWND(), 2002, TASKMGR_AUTO_REFRESH_INTERVAL_MS, nullptr);

    static int lastMainTab = -1;
    if (g_activeMainTab == 1 && lastMainTab != 1) {
        g_forceRefresh = true;
        DoRefresh();
        lastMainTab = 1;
    }
    else if (g_activeMainTab != 1) {
        lastMainTab = g_activeMainTab;
    }

    static bool initialized = false;
    if (!initialized || g_forceRefresh) {
        DoRefresh();
        initialized = true;
    }

    FontFamily ff(L"Segoe UI");
    Font headFont(&ff, 14.5f, FontStyleBold, UnitPixel);
    Font itemFont(&ff, 13.5f, FontStyleRegular, UnitPixel);
    Font toolbarLabelFont(&ff, 11.5f, FontStyleRegular, UnitPixel);
    Font smallFont(&ff, 12.5f, FontStyleRegular, UnitPixel);
    SolidBrush textBrush(COLOR_TEXT);
    SolidBrush mutedBrush(COLOR_TEXT_MUTED);
    SolidBrush bgBrush(COLOR_TAB_BG);
    SolidBrush selBrush(COLOR_TAB_ACTIVE);
    Pen borderPen(COLOR_BORDER, 1.0f);

    TaskmgrLayout L = GetLayout(contentArea, false);
    InitColumnWidths(L.listArea);
    int tab = g_activeSubTab;

    float x = L.toolbar.X + 6.0f;
    float y = L.toolbar.Y + 2.0f;
    float btnH = 24.0f;

    StringFormat checkboxFormat;
    checkboxFormat.SetAlignment(StringAlignmentNear);
    checkboxFormat.SetLineAlignment(StringAlignmentCenter);

    if (tab == SUBTAB_STARTUP || tab == SUBTAB_SUSPICIOUS) {
        g_startupFilterRects.clear();
        g_startupFilterVisibleIndices.clear();

        const float actionX = L.toolbar.X + 4.0f;
        const float actionY = L.toolbar.Y + 1.0f;
        const float actionW = 260.0f;
        const float actionH = 24.0f;
        g_btnDeleteRecentStartup = RectF(actionX, actionY, actionW, actionH);
        SolidBrush deleteBg(COLOR_TAB_BG);
        g.FillRectangle(&deleteBg, g_btnDeleteRecentStartup);
        g.DrawRectangle(&borderPen, g_btnDeleteRecentStartup);
        g.DrawString(L"Удалить подозрительные", -1, &itemFont,
            RectF(actionX + 8.0f, actionY + 1.0f, actionW - 16.0f, actionH), &checkboxFormat, &textBrush);

        const wchar_t* filters[] = {
            L"Run",
            L"RunOnce",
            L"Winlogon",
            L"Boot",
            L"AppInit"
        };
        const float filterY = L.toolbar.Y + 28.0f;
        const float filterH = 22.0f;
        const float filterGap = 6.0f;
        const float filterW = 90.0f;
        const float stripWidth = 5.0f * filterW + 4.0f * filterGap;
        const float viewportStart = L.toolbar.X + 4.0f;
        const float viewportEnd = L.toolbar.X + L.toolbar.Width - 4.0f;
        const float maxScroll = (std::max)(0.0f, stripWidth - (viewportEnd - viewportStart));
        g_startupFilterScrollX = (std::min)(g_startupFilterScrollX, maxScroll);

        float filterX = viewportStart - g_startupFilterScrollX;
        for (int i = 0; i < 5; ++i) {
            RectF r(filterX, filterY, filterW, filterH);
            if (r.X + r.Width > viewportStart && r.X < viewportEnd) {
                g_startupFilterRects.push_back(r);
                g_startupFilterVisibleIndices.push_back(i);
                Color bg = (g_startupFilter == i + 1) ? COLOR_TAB_ACTIVE : COLOR_TAB_BG;
                SolidBrush filterBg(bg);
                g.FillRectangle(&filterBg, r);
                g.DrawRectangle(&borderPen, r);
                StringFormat filterFmt;
                filterFmt.SetAlignment(StringAlignmentCenter);
                filterFmt.SetLineAlignment(StringAlignmentCenter);
                SolidBrush filterText(COLOR_TEXT);
                g.DrawString(filters[i], -1, &toolbarLabelFont, r, &filterFmt, &filterText);
            }
            filterX += filterW + filterGap;
        }
        x = (std::max)(x, viewportEnd + 8.0f);
    }

    g_btnEndProcess = RectF(0, 0, 0, 0);
    g_btnRefresh = RectF(0, 0, 0, 0);
    if (tab != SUBTAB_STARTUP && tab != SUBTAB_SUSPICIOUS) {
        StringFormat checkboxFormat;
        checkboxFormat.SetAlignment(StringAlignmentNear);
        checkboxFormat.SetLineAlignment(StringAlignmentCenter);

        const float boxSize = 14.0f;
        const float itemGap = 8.0f;
        const float labelGap = 6.0f;

        auto measureTextWidth = [&](const wchar_t* text) {
            RectF textBounds;
            g.MeasureString(text, -1, &toolbarLabelFont,
                RectF(0.0f, 0.0f, 1000.0f, 1000.0f),
                &checkboxFormat, &textBounds);
            return textBounds.Width + 4.0f;
        };

        const float hideTextW = measureTextWidth(L"Скрыть дубли");
        const float clearTextW = measureTextWidth(L"Авто-снятие критичности");
        const float blockTextW = measureTextWidth(L"Блокировать запуск");

        if (tab == SUBTAB_PROCESSES) {
            const float autoRefreshTextW = measureTextWidth(L"Автообновление");
            const float autoRefreshW = boxSize + labelGap + autoRefreshTextW;
            g_btnAutoRefresh = RectF(x, y, autoRefreshW, btnH);
            g_autoRefreshBox = RectF(x, y + 3.0f, boxSize, boxSize);
            SolidBrush autoRefreshBg(g_autoRefresh.load() ? COLOR_TAB_ACTIVE : COLOR_TAB_BG);
            g.FillRectangle(&autoRefreshBg, g_autoRefreshBox);
            g.DrawRectangle(&borderPen, g_autoRefreshBox);
            if (g_autoRefresh.load()) {
                Pen checkPen(COLOR_TEXT, 2.0f);
                g.DrawLine(&checkPen, x + 2.0f, y + 9.0f, x + 5.0f, y + 12.0f);
                g.DrawLine(&checkPen, x + 5.0f, y + 12.0f, x + 11.0f, y + 5.0f);
            }
            g.DrawString(L"Автообновление", -1, &toolbarLabelFont,
                RectF(x + boxSize + labelGap, y, autoRefreshTextW, btnH), &checkboxFormat, &textBrush);
            x += autoRefreshW + itemGap;
        }
        else {
            g_btnAutoRefresh = RectF(0, 0, 0, 0);
            g_autoRefreshBox = RectF(0, 0, 0, 0);
        }

        const float hideW = boxSize + labelGap + hideTextW;
        const float clearW = boxSize + labelGap + clearTextW;
        const float blockW = boxSize + labelGap + blockTextW;

        g_btnHideDuplicates = RectF(x, y, hideW, btnH);
        g_hideDuplicatesBox = RectF(x, y + 3.0f, boxSize, boxSize);
        SolidBrush checkboxBg(g_hideDuplicates ? COLOR_TAB_ACTIVE : COLOR_TAB_BG);
        g.FillRectangle(&checkboxBg, g_hideDuplicatesBox);
        g.DrawRectangle(&borderPen, g_hideDuplicatesBox);
        if (g_hideDuplicates) {
            Pen checkPen(COLOR_TEXT, 2.0f);
            g.DrawLine(&checkPen, x + 2.0f, y + 9.0f, x + 5.0f, y + 12.0f);
            g.DrawLine(&checkPen, x + 5.0f, y + 12.0f, x + 11.0f, y + 5.0f);
        }
        g.DrawString(L"Скрыть дубли", -1, &toolbarLabelFont,
            RectF(x + boxSize + labelGap, y, hideTextW, btnH), &checkboxFormat, &textBrush);
        x += hideW + itemGap;

        g_btnAutoClearCritical = RectF(x, y, clearW, btnH);
        g_autoClearCriticalBox = RectF(x, y + 3.0f, boxSize, boxSize);
        SolidBrush autoClearBg(g_autoClearCriticalOnTerminate ? COLOR_TAB_ACTIVE : COLOR_TAB_BG);
        g.FillRectangle(&autoClearBg, g_autoClearCriticalBox);
        g.DrawRectangle(&borderPen, g_autoClearCriticalBox);
        if (g_autoClearCriticalOnTerminate) {
            Pen checkPen(COLOR_TEXT, 2.0f);
            g.DrawLine(&checkPen, x + 2.0f, y + 9.0f, x + 5.0f, y + 12.0f);
            g.DrawLine(&checkPen, x + 5.0f, y + 12.0f, x + 11.0f, y + 5.0f);
        }
        g.DrawString(L"Авто-снятие критичности", -1, &toolbarLabelFont,
            RectF(x + boxSize + labelGap, y, clearTextW, btnH), &checkboxFormat, &textBrush);
        x += clearW + itemGap;

        g_btnBlockProcessLaunches = RectF(x, y, blockW, btnH);
        g_blockProcessLaunchesBox = RectF(x, y + 3.0f, boxSize, boxSize);
        SolidBrush blockLaunchBg(g_blockProcessLaunches ? COLOR_TAB_ACTIVE : COLOR_TAB_BG);
        g.FillRectangle(&blockLaunchBg, g_blockProcessLaunchesBox);
        g.DrawRectangle(&borderPen, g_blockProcessLaunchesBox);
        if (g_blockProcessLaunches) {
            Pen checkPen(COLOR_TEXT, 2.0f);
            g.DrawLine(&checkPen, x + 2.0f, y + 9.0f, x + 5.0f, y + 12.0f);
            g.DrawLine(&checkPen, x + 5.0f, y + 12.0f, x + 11.0f, y + 5.0f);
        }
        g.DrawString(L"Блокировать запуск", -1, &toolbarLabelFont,
            RectF(x + boxSize + labelGap, y, blockTextW, btnH), &checkboxFormat, &textBrush);
        x += blockW;
    }
    else {
        g_btnHideDuplicates = RectF(0, 0, 0, 0);
        g_hideDuplicatesBox = RectF(0, 0, 0, 0);
        g_btnAutoClearCritical = RectF(0, 0, 0, 0);
        g_autoClearCriticalBox = RectF(0, 0, 0, 0);
        g_btnAutoRefresh = RectF(0, 0, 0, 0);
        g_autoRefreshBox = RectF(0, 0, 0, 0);
        g_btnBlockProcessLaunches = RectF(0, 0, 0, 0);
        g_blockProcessLaunchesBox = RectF(0, 0, 0, 0);
    }

    if (tab != SUBTAB_STARTUP && tab != SUBTAB_SUSPICIOUS) {
        g_btnDeleteRecentStartup = RectF(0, 0, 0, 0);
    }

    g_btnCreate = RectF(0, 0, 0, 0);

    StringFormat leftF; leftF.SetAlignment(StringAlignmentNear); leftF.SetLineAlignment(StringAlignmentCenter);

    g.FillRectangle(&bgBrush, L.searchBox);
    g.DrawRectangle(&borderPen, L.searchBox);
    RectF searchTextRect(L.searchBox.X + 8.0f, L.searchBox.Y, L.searchBox.Width - 16.0f, L.searchBox.Height);
    StringFormat searchF; searchF.SetAlignment(StringAlignmentNear); searchF.SetLineAlignment(StringAlignmentCenter);
    if (g_searchText.empty()) {
        g.DrawString(L"Поиск...", -1, &itemFont, searchTextRect, &searchF, &mutedBrush);
    }
    else {
        g.DrawString(g_searchText.c_str(), -1, &itemFont, searchTextRect, &searchF, &textBrush);
    }
    g_searchBoxRect = L.searchBox;

    g.FillRectangle(&bgBrush, L.listArea);
    g.DrawRectangle(&borderPen, L.listArea);

    switch (tab) {
    case SUBTAB_PROCESSES:
        DrawProcesses(g, L, headFont, itemFont, smallFont, textBrush, mutedBrush, selBrush);
        break;
    case SUBTAB_STARTUP:
        DrawStartup(g, L, headFont, itemFont, smallFont, textBrush, mutedBrush, selBrush, false);
        break;
    case SUBTAB_SUSPICIOUS:
        DrawStartup(g, L, headFont, itemFont, smallFont, textBrush, mutedBrush, selBrush, true);
        break;
    case SUBTAB_SERVICES:
        DrawServices(g, L, headFont, itemFont, smallFont, textBrush, mutedBrush, selBrush);
        break;
    case SUBTAB_DRIVERS:
        DrawDrivers(g, L, headFont, itemFont, smallFont, textBrush, mutedBrush, selBrush);
        break;
    }
}

bool OnTaskManagerClick(int x, int y, const RectF& contentArea) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    int tab = g_activeSubTab;
    bool inWinRE = IsLikelyRecoveryEnvironment();

    if ((tab == SUBTAB_STARTUP || tab == SUBTAB_SUSPICIOUS) && !g_startupFilterRects.empty()) {
        for (int i = 0; i < (int)g_startupFilterRects.size(); ++i) {
            if (HitTestRect(g_startupFilterRects[i], fx, fy)) {
                g_startupFilter = g_startupFilterVisibleIndices[i] + 1;
                DoRefresh();
                InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
                return true;
            }
        }
    }

    int resizeCol = GetColumnResizeHit(fx, fy, contentArea);
    if (resizeCol >= 0) {
        g_resizingColIndex = resizeCol;
        g_resizingStartX = fx;
        float* widths = nullptr;
        if (tab == SUBTAB_PROCESSES) widths = g_procColWidths;
        else if (tab == SUBTAB_STARTUP) widths = g_startupColWidths;
        else if (tab == SUBTAB_SERVICES) widths = g_svcColWidths;
        else if (tab == SUBTAB_DRIVERS) widths = g_driverColWidths;
        if (widths) g_resizingStartWidth = widths[resizeCol];
        SetCapture(App::Instance()->GetHWND());
        return true;
    }

    if (fx >= g_btnHideDuplicates.X && fx <= g_btnHideDuplicates.X + g_btnHideDuplicates.Width &&
        fy >= g_btnHideDuplicates.Y && fy <= g_btnHideDuplicates.Y + g_btnHideDuplicates.Height) {
        g_hideDuplicates = !g_hideDuplicates;
        g_selectedProcessIndex = -1;
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }

    if (g_btnAutoRefresh.Width > 0.0f &&
        fx >= g_btnAutoRefresh.X && fx <= g_btnAutoRefresh.X + g_btnAutoRefresh.Width &&
        fy >= g_btnAutoRefresh.Y && fy <= g_btnAutoRefresh.Y + g_btnAutoRefresh.Height) {
        bool enabled = !g_autoRefresh.load();
        g_autoRefresh.store(enabled);
        if (enabled) {
            g_procRefreshRequested.store(true);
            if (!g_procWorkerRunning.load())
                StartProcWorker(App::Instance()->GetHWND());
        }
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }

    if (fx >= g_btnAutoClearCritical.X && fx <= g_btnAutoClearCritical.X + g_btnAutoClearCritical.Width &&
        fy >= g_btnAutoClearCritical.Y && fy <= g_btnAutoClearCritical.Y + g_btnAutoClearCritical.Height) {
        g_autoClearCriticalOnTerminate = !g_autoClearCriticalOnTerminate;
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }

    if (fx >= g_btnBlockProcessLaunches.X && fx <= g_btnBlockProcessLaunches.X + g_btnBlockProcessLaunches.Width &&
        fy >= g_btnBlockProcessLaunches.Y && fy <= g_btnBlockProcessLaunches.Y + g_btnBlockProcessLaunches.Height) {
        g_blockProcessLaunches = !g_blockProcessLaunches;
        if (g_blockProcessLaunches) {
            g_launchGuardKnownPids.clear();
            g_launchGuardSuspendedPids.clear();
            g_launchGuardInitialized = false;
        }
        else if (!g_launchGuardSuspendedPids.empty())
            ResumeLaunchGuardProcesses();
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }

    if (g_btnDeleteRecentStartup.Width > 0.0f &&
        fx >= g_btnDeleteRecentStartup.X && fx <= g_btnDeleteRecentStartup.X + g_btnDeleteRecentStartup.Width &&
        fy >= g_btnDeleteRecentStartup.Y && fy <= g_btnDeleteRecentStartup.Y + g_btnDeleteRecentStartup.Height) {
        if (tab == SUBTAB_STARTUP || tab == SUBTAB_SUSPICIOUS) {
            TaskManagerDeleteRecentStartupEntries();
            DoRefresh();
            InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        }
        return true;
    }

    if (g_btnCreate.Width > 0.0f &&
        fx >= g_btnCreate.X && fx <= g_btnCreate.X + g_btnCreate.Width &&
        fy >= g_btnCreate.Y && fy <= g_btnCreate.Y + g_btnCreate.Height) {
        if (tab == SUBTAB_STARTUP) {
            bool created = false;
            if (inWinRE) {
                auto locs = BuildOfflineStartupLocations();
                if (!locs.empty())
                    created = StartupEditDialog::ShowCreate(App::Instance()->GetHWND(), locs);
                else
                    MessageBoxW(App::Instance()->GetHWND(),
                        L"Офлайн-кусты реестра не смонтированы.",
                        L"Автозагрузки", MB_OK | MB_ICONINFORMATION);
            }
            else {
                created = StartupEditDialog::ShowCreate(App::Instance()->GetHWND());
            }
            if (created) DoRefresh();
        }
        else if (tab == SUBTAB_SERVICES) {
            if (ServiceEditDialog::ShowCreate(App::Instance()->GetHWND()))
                DoRefresh();
        }
        return true;
    }

    if (fx >= g_searchBoxRect.X && fx <= g_searchBoxRect.X + g_searchBoxRect.Width &&
        fy >= g_searchBoxRect.Y && fy <= g_searchBoxRect.Y + g_searchBoxRect.Height) {
        g_searchActive = true;
        SetFocus(App::Instance()->GetHWND());
        return true;
    }
    else {
        g_searchActive = false;
    }

    if (tab == SUBTAB_PROCESSES) {
        SortColumn col = GetColumnAtHeader(fx, fy, contentArea);
        if (col != SortColumn::None) {
            if (g_sortColumn == col) g_sortAscending = !g_sortAscending;
            else { g_sortColumn = col; g_sortAscending = true; }
            InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
            return true;
        }
    }

    if (tab == SUBTAB_PROCESSES) {
        int row = GetProcessRowAt(fx, fy, contentArea);
        if (row >= 0) {
            g_selectedProcessIndex = row;
            LoadProcessDetails(row);
            InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
            return true;
        }
    }
    else if (tab == SUBTAB_STARTUP || tab == SUBTAB_SUSPICIOUS) {
        int row = GetStartupRowAt(fx, fy, contentArea);
        if (row >= 0) {
            g_selectedStartupIndex = row;
            InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
            return true;
        }
    }
    else if (tab == SUBTAB_SERVICES) {
        int row = GetServiceRowAt(fx, fy, contentArea);
        if (row >= 0) {
            g_selectedServiceIndex = row;
            InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
            return true;
        }
    }
    else if (tab == SUBTAB_DRIVERS) {
        int row = GetDriverRowAt(fx, fy, contentArea);
        if (row >= 0) {
            g_selectedDriverIndex = row;
            LoadDriverSignature(row);
            InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
            return true;
        }
    }
    return false;
}

bool OnTaskManagerMouseMove(int x, int y, const RectF& contentArea) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    int tab = g_activeSubTab;

    UpdateProcessPathTooltip(x, y, contentArea);

    if (g_resizingColIndex >= 0) {
        float* widths = nullptr;
        if (tab == SUBTAB_PROCESSES) widths = g_procColWidths;
        else if (tab == SUBTAB_STARTUP) widths = g_startupColWidths;
        else if (tab == SUBTAB_SERVICES) widths = g_svcColWidths;
        else if (tab == SUBTAB_DRIVERS) widths = g_driverColWidths;

        if (widths) {
            float delta = fx - g_resizingStartX;
            float newWidth = g_resizingStartWidth + delta;
            if (newWidth < COL_MIN_WIDTH) newWidth = COL_MIN_WIDTH;
            if (newWidth > COL_MAX_WIDTH) newWidth = COL_MAX_WIDTH;
            widths[g_resizingColIndex] = newWidth;
            InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        }
        return true;
    }

    int resizeCol = GetColumnResizeHit(fx, fy, contentArea);
    if (resizeCol >= 0) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return true;
    }
    return false;
}

bool OnTaskManagerLButtonUp() {
    if (g_resizingColIndex >= 0) {
        g_resizingColIndex = -1;
        g_resizingStartX = 0.0f;
        g_resizingStartWidth = 0.0f;
        ReleaseCapture();
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }
    return false;
}

bool OnTaskManagerWheel(int x, int y, int delta, const RectF& contentArea) {
    TaskmgrLayout layout = GetLayout(contentArea, false);

    if ((g_activeSubTab == SUBTAB_STARTUP || g_activeSubTab == SUBTAB_SUSPICIOUS) &&
        x >= layout.toolbar.X && x <= layout.toolbar.X + layout.toolbar.Width &&
        y >= layout.toolbar.Y && y <= layout.toolbar.Y + layout.toolbar.Height + 28.0f) {
        const float filterW = 92.0f;
        const float filterGap = 6.0f;
        const float viewportWidth = (std::max)(240.0f, layout.toolbar.Width - 8.0f);
        const float stripWidth = 11.0f * filterW + 10.0f * filterGap;
        const float maxScroll = (std::max)(0.0f, stripWidth - viewportWidth);
        int notches = delta / WHEEL_DELTA;
        if (notches == 0) notches = delta > 0 ? 1 : -1;
        g_startupFilterScrollX = (std::max)(0.0f, (std::min)(maxScroll, g_startupFilterScrollX + notches * 40.0f));
        InvalidateRect(App::Instance()->GetHWND(), nullptr, FALSE);
        return true;
    }

    if (x < layout.listArea.X || x > layout.listArea.X + layout.listArea.Width ||
        y < layout.listArea.Y || y > layout.listArea.Y + layout.listArea.Height) return false;

    float pathX = layout.listArea.X + 6.0f;
    for (int i = 0; i < 5; ++i) pathX += g_procColWidths[i];
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0 && x >= pathX) {
        auto processes = GetFilteredProcessIndices();
        int maxPathScroll = 0;
        for (int index : processes) {
            const auto& path = g_processes[index].fullPath;
            maxPathScroll = (std::max)(maxPathScroll,
                (int)path.size() * 8 - (int)g_procColWidths[5] + 16);
        }
        maxPathScroll = (std::max)(0, maxPathScroll);
        int notches = delta / WHEEL_DELTA;
        if (notches == 0) notches = delta > 0 ? 1 : -1;
        g_pathScrollX = (std::max)(0, (std::min)(maxPathScroll,
            g_pathScrollX - notches * 80));
        InvalidateRect(App::Instance()->GetHWND(), nullptr, FALSE);
        return true;
    }

    auto filtered = GetFilteredProcessIndices();
    float visibleHeight = layout.listArea.Height - 36.0f;
    int visibleRows = (std::max)(1, (int)(visibleHeight / ROW_HEIGHT));
    int maxScroll = ((int)filtered.size() > visibleRows)
        ? ((int)filtered.size() - visibleRows) * (int)ROW_HEIGHT : 0;
    int notches = delta / WHEEL_DELTA;
    if (notches == 0) notches = delta > 0 ? 1 : -1;
    int next = g_scrollOffset[1] - notches * (int)ROW_HEIGHT * 3;
    next = (std::max)(0, (std::min)(maxScroll, next));
    if (next != g_scrollOffset[1]) {
        g_scrollOffset[1] = next;
        InvalidateRect(App::Instance()->GetHWND(), nullptr, FALSE);
    }
    return true;
}

bool OnTaskManagerRightClick(int x, int y, const RectF& contentArea) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    HWND hwnd = App::Instance()->GetHWND();
    int tab = g_activeSubTab;
    bool inWinRE = IsLikelyRecoveryEnvironment();

    if (tab == SUBTAB_PROCESSES && !inWinRE) {
        int row = GetProcessRowAt(fx, fy, contentArea);
        if (row < 0) return false;
        g_selectedProcessIndex = row;
        LoadProcessDetails(row);
        InvalidateRect(hwnd, nullptr, TRUE);
        if (row >= (int)g_processes.size()) return false;
        const ProcessDisplayInfo proc = g_processes[row];

        HMENU menu = CreatePopupMenu();
        UINT flags = (proc.pid == 0 || proc.pid == 4) ? MF_GRAYED : MF_STRING;
        AppendMenuW(menu, flags, IDM_PROC_TERMINATE, L"Завершить процесс");
        AppendMenuW(menu, flags, IDM_PROC_TERMINATE_TREE, L"Завершить дерево процессов");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, flags, IDM_PROC_SUSPEND, L"Приостановить");
        AppendMenuW(menu, flags, IDM_PROC_RESUME, L"Возобновить");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        HMENU prioMenu = CreatePopupMenu();
        AppendMenuW(prioMenu, MF_STRING, IDM_PROC_PRIORITY_REALTIME, L"Реального времени");
        AppendMenuW(prioMenu, MF_STRING, IDM_PROC_PRIORITY_HIGH, L"Высокий");
        AppendMenuW(prioMenu, MF_STRING, IDM_PROC_PRIORITY_ABOVE_NORMAL, L"Выше обычного");
        AppendMenuW(prioMenu, MF_STRING, IDM_PROC_PRIORITY_NORMAL, L"Обычный");
        AppendMenuW(prioMenu, MF_STRING, IDM_PROC_PRIORITY_BELOW_NORMAL, L"Ниже обычного");
        AppendMenuW(prioMenu, MF_STRING, IDM_PROC_PRIORITY_LOW, L"Низкий");
        DWORD currentPriority = WinCtrl::Process::GetProcessPriority(proc.pid);
        UINT checkedId = 0;
        switch (currentPriority) {
        case REALTIME_PRIORITY_CLASS:      checkedId = IDM_PROC_PRIORITY_REALTIME; break;
        case HIGH_PRIORITY_CLASS:          checkedId = IDM_PROC_PRIORITY_HIGH; break;
        case ABOVE_NORMAL_PRIORITY_CLASS:  checkedId = IDM_PROC_PRIORITY_ABOVE_NORMAL; break;
        case NORMAL_PRIORITY_CLASS:        checkedId = IDM_PROC_PRIORITY_NORMAL; break;
        case BELOW_NORMAL_PRIORITY_CLASS:  checkedId = IDM_PROC_PRIORITY_BELOW_NORMAL; break;
        case IDLE_PRIORITY_CLASS:          checkedId = IDM_PROC_PRIORITY_LOW; break;
        }
        if (checkedId) CheckMenuRadioItem(prioMenu, IDM_PROC_PRIORITY_REALTIME, IDM_PROC_PRIORITY_LOW, checkedId, MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)prioMenu, L"Приоритет");

        std::wstring path = proc.fullPath.empty() ? WinCtrl::Process::GetImagePath(proc.pid) : proc.fullPath;
        AppendMenuW(menu, path.empty() ? MF_GRAYED : MF_STRING, IDM_PROC_OPEN_LOCATION, L"Открыть расположение файла");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        bool injectEnabled = proc.pid != 0 && proc.pid != 4 && TaskMgr_IsInjectionAllowedByBitness(proc.pid);
        AppendMenuW(menu, injectEnabled ? MF_STRING : MF_GRAYED, IDM_PROC_INJECT_DLL,
            injectEnabled ? L"Инжект DLL" : L"Инжект DLL (недоступно)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        HMENU copyMenu = CreatePopupMenu();
        AppendMenuW(copyMenu, MF_STRING, IDM_PROC_COPY_PID, L"Копировать PID");
        AppendMenuW(copyMenu, MF_STRING, IDM_PROC_COPY_NAME, L"Копировать имя");
        AppendMenuW(copyMenu, MF_STRING, IDM_PROC_COPY_PATH, L"Копировать путь");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)copyMenu, L"Копировать");

        POINT pt{ x, y }; ClientToScreen(hwnd, &pt);
        SetForegroundWindow(hwnd);
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (cmd) ExecuteProcessCommand(cmd, proc);
        return true;
    }
    else if (tab == SUBTAB_STARTUP || tab == SUBTAB_SUSPICIOUS) {
        int row = GetStartupRowAt(fx, fy, contentArea);
        if (row < 0) return false;
        g_selectedStartupIndex = row;
        InvalidateRect(hwnd, nullptr, TRUE);
        if (row >= (int)g_startupEntries.size()) return false;
        const auto& entry = g_startupEntries[row];

        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDM_STARTUP_OPEN_LOC, L"Открыть расположение");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, entry.enabled ? IDM_STARTUP_DISABLE : IDM_STARTUP_ENABLE,
            entry.enabled ? L"Отключить" : L"Включить");
        AppendMenuW(menu, MF_STRING, IDM_STARTUP_CLEAR, L"Очистить параметр");
        AppendMenuW(menu, entry.isCritical ? MF_GRAYED : MF_STRING, IDM_STARTUP_REMOVE, L"Удалить параметр");
        AppendMenuW(menu, MF_STRING, IDM_STARTUP_REMOVE_ALL, L"Удалить все в этом разделе");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_STARTUP_EDIT, L"Изменить параметр...");

        POINT pt{ x, y }; ClientToScreen(hwnd, &pt);
        SetForegroundWindow(hwnd);
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (cmd) ExecuteStartupCommand(cmd, entry);
        return true;
    }
    else if (tab == SUBTAB_SERVICES) {
        int row = GetServiceRowAt(fx, fy, contentArea);
        if (row < 0) return false;
        g_selectedServiceIndex = row;
        InvalidateRect(hwnd, nullptr, TRUE);
        if (row >= (int)g_services.size()) return false;
        const auto& svc = g_services[row];

        HMENU menu = CreatePopupMenu();

        UINT startFlag = svc.state == ServiceManager::ServiceState::Stopped ? MF_STRING : MF_GRAYED;
        UINT stopFlag = svc.state == ServiceManager::ServiceState::Running ? MF_STRING : MF_GRAYED;
        if (g_servicesOffline) startFlag = stopFlag = MF_GRAYED;

        AppendMenuW(menu, startFlag, IDM_SVC_START, L"Запустить");
        AppendMenuW(menu, stopFlag, IDM_SVC_STOP, L"Остановить");
        AppendMenuW(menu, stopFlag, IDM_SVC_RESTART, L"Перезапустить");

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        HMENU stMenu = CreatePopupMenu();
        AppendMenuW(stMenu, MF_STRING, IDM_SVC_AUTO, L"Автоматически");
        AppendMenuW(stMenu, MF_STRING, IDM_SVC_MANUAL, L"Вручную");
        AppendMenuW(stMenu, MF_STRING, IDM_SVC_DISABLED, L"Отключена");

        UINT checkedId = 0;
        switch (svc.startType) {
        case ServiceManager::ServiceStartType::Auto:     checkedId = IDM_SVC_AUTO;     break;
        case ServiceManager::ServiceStartType::Demand:   checkedId = IDM_SVC_MANUAL;   break;
        case ServiceManager::ServiceStartType::Disabled: checkedId = IDM_SVC_DISABLED; break;
        default: break;
        }
        if (checkedId)
            CheckMenuRadioItem(stMenu, IDM_SVC_AUTO, IDM_SVC_DISABLED, checkedId, MF_BYCOMMAND);

        AppendMenuW(menu, MF_POPUP, (UINT_PTR)stMenu, L"Тип запуска");

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_SVC_EDIT, L"Изменить...");

        POINT pt{ x, y }; ClientToScreen(hwnd, &pt);
        SetForegroundWindow(hwnd);
        int cmd = TrackPopupMenu(menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (cmd) ExecuteServiceCommand(cmd, svc);
        return true;
    }
    else if (tab == SUBTAB_DRIVERS) {
        int row = GetDriverRowAt(fx, fy, contentArea);
        if (row < 0) return false;
        g_selectedDriverIndex = row;
        LoadDriverSignature(row);
        InvalidateRect(hwnd, nullptr, TRUE);
        if (row >= (int)g_drivers.size()) return false;
        const auto& drv = g_drivers[row];

        HMENU menu = CreatePopupMenu();
        bool hasPath = !drv.imagePath.empty();

        AppendMenuW(menu, hasPath ? MF_STRING : MF_GRAYED,
            IDM_DRV_OPEN_LOC, L"Открыть расположение");
        AppendMenuW(menu, hasPath ? MF_STRING : MF_GRAYED,
            IDM_DRV_COPY_PATH, L"Копировать путь");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, hasPath ? MF_STRING : MF_GRAYED,
            IDM_DRV_VERIFY_SIG, L"Проверить подпись");

        if (g_driversOffline) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            HMENU stMenu = CreatePopupMenu();
            AppendMenuW(stMenu, MF_STRING, IDM_DRV_BOOT, L"Boot");
            AppendMenuW(stMenu, MF_STRING, IDM_DRV_SYSTEM, L"System");
            AppendMenuW(stMenu, MF_STRING, IDM_DRV_AUTO, L"Авто");
            AppendMenuW(stMenu, MF_STRING, IDM_DRV_MANUAL, L"Вручную");
            AppendMenuW(stMenu, MF_STRING, IDM_DRV_DISABLED, L"Отключена");

            UINT checkedId = 0;
            switch (drv.startType) {
            case SERVICE_BOOT_START:   checkedId = IDM_DRV_BOOT;     break;
            case SERVICE_SYSTEM_START: checkedId = IDM_DRV_SYSTEM;   break;
            case SERVICE_AUTO_START:   checkedId = IDM_DRV_AUTO;     break;
            case SERVICE_DEMAND_START: checkedId = IDM_DRV_MANUAL;   break;
            case SERVICE_DISABLED:     checkedId = IDM_DRV_DISABLED; break;
            }
            if (checkedId)
                CheckMenuRadioItem(stMenu, IDM_DRV_BOOT, IDM_DRV_DISABLED,
                    checkedId, MF_BYCOMMAND);

            AppendMenuW(menu, MF_POPUP, (UINT_PTR)stMenu, L"Тип запуска");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_GRAYED, IDM_DRV_EDIT, L"Изменить...");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_GRAYED, IDM_DRV_START, L"Запустить службу");
            AppendMenuW(menu, MF_GRAYED, IDM_DRV_STOP, L"Остановить службу");
        }
        else {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_DRV_EDIT, L"Изменить...");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, drv.state == SERVICE_STOPPED ? MF_STRING : MF_GRAYED,
                IDM_DRV_START, L"Запустить службу");
            AppendMenuW(menu, drv.state == SERVICE_RUNNING ? MF_STRING : MF_GRAYED,
                IDM_DRV_STOP, L"Остановить службу");
        }

        POINT pt{ x, y }; ClientToScreen(hwnd, &pt);
        SetForegroundWindow(hwnd);
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (cmd) ExecuteDriverCommand(cmd, drv);
        return true;
    }
    return false;
}

bool OnTaskManagerDblClick(int x, int y, const RectF& contentArea) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    int tab = g_activeSubTab;
    HWND hwnd = App::Instance()->GetHWND();
    bool inWinRE = IsLikelyRecoveryEnvironment();

    if (tab == SUBTAB_PROCESSES && inWinRE) return false;

    if (tab == SUBTAB_PROCESSES) {
        int row = GetProcessRowAt(fx, fy, contentArea);
        if (row >= 0 && row < (int)g_processes.size()) {
            g_selectedProcessIndex = row;
            LoadProcessDetails(row);
            std::wstring path = g_processes[row].fullPath.empty()
                ? WinCtrl::Process::GetImagePath(g_processes[row].pid)
                : g_processes[row].fullPath;
            OpenPathInExplorer(path);
            return true;
        }
    }

    if (tab == SUBTAB_STARTUP || tab == SUBTAB_SUSPICIOUS) {
        int row = GetStartupRowAt(fx, fy, contentArea);
        if (row >= 0 && row < (int)g_startupEntries.size()) {
            g_selectedStartupIndex = row;
            EditStartupEntry(g_startupEntries[row]);
            return true;
        }
    }
    else if (tab == SUBTAB_SERVICES) {
        int row = GetServiceRowAt(fx, fy, contentArea);
        if (row >= 0 && row < (int)g_services.size()) {
            g_selectedServiceIndex = row;
            if (ServiceEditDialog::ShowEdit(hwnd, g_services[row].name))
                DoRefresh();
            return true;
        }
    }
    else if (tab == SUBTAB_DRIVERS) {
        int row = GetDriverRowAt(fx, fy, contentArea);
        if (row >= 0 && row < (int)g_drivers.size()) {
            g_selectedDriverIndex = row;
            LoadDriverSignature(row);
            if (ServiceEditDialog::ShowEdit(hwnd, g_drivers[row].serviceName, true))
                DoRefresh();
            return true;
        }
    }
    return false;
}

bool OnTaskManagerKey(UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    if (msg == WM_CHAR) {
        wchar_t ch = (wchar_t)wParam;
        if (ch == L'\x1b') {
            g_searchText.clear();
            g_searchActive = false;
            DoRefresh();
            InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
            return true;
        }
        if (ch == L'\b' || ch == L'\x7f') {
            if (!g_searchActive) g_searchActive = true;
            if (!g_searchText.empty()) g_searchText.pop_back();
            DoRefresh();
            InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
            return true;
        }
        if (ch >= L' ') {
            if (!g_searchActive) g_searchActive = true;
            g_searchText.push_back(ch);
            DoRefresh();
            InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
            return true;
        }
    }
    if (msg == WM_KEYDOWN && wParam == VK_BACK) {
        if (!g_searchActive) g_searchActive = true;
        if (!g_searchText.empty()) g_searchText.pop_back();
        DoRefresh();
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        g_searchText.clear();
        g_searchActive = false;
        DoRefresh();
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }
    return false;
}