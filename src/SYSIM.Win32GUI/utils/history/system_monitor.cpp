#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <winsvc.h>
#include <winevt.h>
#include "system_monitor.h"
#include <tlhelp32.h>
#include <winternl.h>
#include <map>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <mutex>
#include <thread>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "wevtapi.lib")

#ifndef MIB_TCP_STATE_ESTABLISHED
#define MIB_TCP_STATE_ESTABLISHED 5
#endif

namespace ActivityMonitor {

    // ==================== ГЛОБАЛЬНОЕ СОСТОЯНИЕ ====================
    static CRITICAL_SECTION g_cs;
    static bool g_csInit = false;
    static std::vector<Event> g_events;
    static const size_t MAX_EVENTS = 50000;
    static bool g_running = false;
    static HANDLE g_stopEvent = nullptr;
    static std::vector<HANDLE> g_threads;

    static bool g_useSysmon = false;
    static bool g_sysmonAvailable = false;
    static std::wstring g_sysmonXPath = L"*[System[(EventID=1 or EventID=3 or EventID=5 or EventID=11 or EventID=12 or EventID=13 or EventID=14)]]";
    static HANDLE g_sysmonSubscription = nullptr;

    static std::set<DWORD> g_trackedPids;
    static DWORD g_rootTrackedPid = 0;
    static std::wstring g_rootTrackedName;
    static CRITICAL_SECTION g_trackCs;
    static bool g_trackCsInit = false;

    // ==================== СИНХРОНИЗАЦИЯ ====================
    static void Lock() { if (g_csInit) EnterCriticalSection(&g_cs); }
    static void Unlock() { if (g_csInit) LeaveCriticalSection(&g_cs); }
    static void EnsureTrackCs() {
        if (!g_trackCsInit) {
            InitializeCriticalSection(&g_trackCs);
            g_trackCsInit = true;
        }
    }

    static void LockTrack() {
        EnsureTrackCs();
        EnterCriticalSection(&g_trackCs);
    }

    static void UnlockTrack() {
        if (g_trackCsInit) LeaveCriticalSection(&g_trackCs);
    }
    static bool WantStop() { return WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0; }

    // ==================== PEB СТРУКТУРЫ (ДО использования) ====================
    typedef NTSTATUS(NTAPI* pfnNtQueryInformationProcess)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

    typedef struct _RTL_UP_CUSTOM {
        BYTE Reserved1[16];
        PVOID Reserved2[10];
        UNICODE_STRING ImagePathName;
        UNICODE_STRING CommandLine;
    } RTL_UP_CUSTOM;

    typedef struct _PEB_CUSTOM {
        BYTE Reserved1[2];
        BYTE BeingDebugged;
        BYTE Reserved2[1];
        PVOID Reserved3[2];
        PVOID Ldr;
        RTL_UP_CUSTOM* ProcessParameters;
    } PEB_CUSTOM;

    // ==================== СЕТЕВЫЕ КЭШИ (ДО использования) ====================
    static std::mutex g_dnsMtx;
    static std::unordered_map<std::wstring, std::wstring> g_dnsCache;
    static std::unordered_map<DWORD, std::wstring> g_procCache;

    static const wchar_t* PortToService(USHORT port) {
        switch (port) {
        case 20: return L"FTP-DATA"; case 21: return L"FTP";
        case 22: return L"SSH";      case 23: return L"TELNET";
        case 25: return L"SMTP";     case 53: return L"DNS";
        case 80: return L"HTTP";     case 110: return L"POP3";
        case 143: return L"IMAP";    case 443: return L"HTTPS";
        case 445: return L"SMB";     case 993: return L"IMAPS";
        case 995: return L"POP3S";   case 3389: return L"RDP";
        case 5900: return L"VNC";    case 8080: return L"HTTP-ALT";
        case 8443: return L"HTTPS-ALT";
        default: return L"";
        }
    }

    static std::wstring GetProcNameCached(DWORD pid) {
        {
            std::lock_guard<std::mutex> lk(g_dnsMtx);
            auto it = g_procCache.find(pid);
            if (it != g_procCache.end()) return it->second;
        }
        std::wstring name = L"?";
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            wchar_t buf[MAX_PATH]; DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, buf, &sz)) {
                name = buf;
                auto p = name.find_last_of(L'\\');
                if (p != std::wstring::npos) name = name.substr(p + 1);
            }
            CloseHandle(h);
        }
        std::lock_guard<std::mutex> lk(g_dnsMtx);
        g_procCache[pid] = name;
        return name;
    }

    static DWORD WINAPI DnsResolveThread(LPVOID param) {
        auto* ip = (std::wstring*)param;
        wchar_t host[NI_MAXHOST] = { 0 };
        sockaddr_in sa{}; sa.sin_family = AF_INET;
        InetPtonW(AF_INET, ip->c_str(), &sa.sin_addr);
        if (GetNameInfoW((sockaddr*)&sa, sizeof(sa), host, NI_MAXHOST, nullptr, 0, NI_NAMEREQD) == 0) {
            std::lock_guard<std::mutex> lk(g_dnsMtx);
            g_dnsCache[*ip] = host;
        }
        else {
            std::lock_guard<std::mutex> lk(g_dnsMtx);
            g_dnsCache[*ip] = *ip;
        }
        delete ip;
        return 0;
    }

    static std::wstring ResolveIpAsync(const std::wstring& ip) {
        if (ip.find(L"127.") == 0 || ip.find(L"10.") == 0 ||
            ip.find(L"192.168.") == 0 || ip.find(L"169.254.") == 0 ||
            ip.find(L"0.") == 0) return ip;
        {
            std::lock_guard<std::mutex> lk(g_dnsMtx);
            auto it = g_dnsCache.find(ip);
            if (it != g_dnsCache.end()) return it->second;
        }
        HANDLE h = CreateThread(nullptr, 0, DnsResolveThread, new std::wstring(ip), 0, nullptr);
        if (h) CloseHandle(h);
        return ip;
    }

    struct ConnKey {
        DWORD pid; DWORD ip; USHORT port;
        bool operator==(const ConnKey& o) const { return pid == o.pid && ip == o.ip && port == o.port; }
    };
    struct ConnKeyHash {
        size_t operator()(const ConnKey& k) const { return std::hash<DWORD>()(k.pid) ^ (std::hash<DWORD>()(k.ip) << 1) ^ k.port; }
    };

    // ==================== PUSHEVENT ====================
    static void PushEvent(EventType type, const std::wstring& path, const std::wstring& extra,
        bool suspicious = false, DWORD pid = 0, DWORD parentPid = 0) {
        LockTrack();
        bool trackActive = (g_rootTrackedPid != 0);
        if (trackActive) {
            if (g_trackedPids.find(pid) == g_trackedPids.end() &&
                g_trackedPids.find(parentPid) == g_trackedPids.end()) {
                UnlockTrack();
                return;
            }
        }
        UnlockTrack();

        Event e;
        GetLocalTime(&e.time);
        e.type = type;
        e.path = path;
        e.extra = extra;
        e.suspicious = suspicious;
        e.pid = pid;
        e.parentPid = parentPid;

        Lock();
        if (g_events.size() >= MAX_EVENTS) g_events.erase(g_events.begin());
        g_events.push_back(e);
        Unlock();
    }

    // ==================== УТИЛИТЫ ====================
    const wchar_t* TypeToString(EventType t) {
        switch (t) {
        case EventType::FileCreated:     return L"Файл +";
        case EventType::FileDeleted:     return L"Файл −";
        case EventType::FileRenamed:     return L"Rename";
        case EventType::FolderCreated:   return L"Папка +";
        case EventType::FolderDeleted:   return L"Папка −";
        case EventType::RegistryChanged: return L"Reg";
        case EventType::ProcessStarted:  return L"Proc ▶";
        case EventType::ProcessStopped:  return L"Proc ■";
        case EventType::ServiceStarted:  return L"Svc ▶";
        case EventType::ServiceStopped:  return L"Svc ■";
        case EventType::NetConnect:      return L"Net";
        }
        return L"?";
    }

    std::wstring EventToString(const Event& e) {
        wchar_t buf[64];
        swprintf_s(buf, L"%02d:%02d:%02d", e.time.wHour, e.time.wMinute, e.time.wSecond);
        std::wstring s = std::wstring(buf) + L"  " + TypeToString(e.type) + L"  " + e.path;
        if (!e.extra.empty()) s += L"  " + e.extra;
        return s;
    }

    Stats GetStats() {
        Stats st;
        if (!g_csInit) return st;
        Lock();
        for (const auto& e : g_events) {
            switch (e.type) {
            case EventType::FileCreated: case EventType::FileDeleted:
            case EventType::FileRenamed: case EventType::FolderCreated:
            case EventType::FolderDeleted: st.files++; break;
            case EventType::RegistryChanged: st.registry++; break;
            case EventType::ProcessStarted: case EventType::ProcessStopped: st.processes++; break;
            case EventType::ServiceStarted: case EventType::ServiceStopped: st.services++; break;
            case EventType::NetConnect: st.network++; break;
            }
        }
        Unlock();
        return st;
    }

    // ==================== FILES ====================
    static bool IsNoisyPath(const std::wstring& p) {
        static const wchar_t* noise[] = {
            L"\\$Recycle.Bin", L"\\pagefile.sys", L"\\swapfile.sys", L"\\hiberfil.sys",
            L"\\Windows\\Temp\\", L"\\Windows\\Logs\\", L"\\Windows\\Prefetch\\",
            L"\\Windows\\SoftwareDistribution\\", L"\\INetCache\\", L"\\Cookies\\",
            L"\\AppData\\Local\\Temp\\", L"\\AppData\\Local\\Microsoft\\Windows\\Explorer\\",
            L"\\AppData\\Local\\Microsoft\\Windows\\WebCache\\",
            L"\\System Volume Information\\", L"\\ntuser.dat",
        };
        for (const wchar_t* n : noise)
            if (p.find(n) != std::wstring::npos) return true;
        return false;
    }

    static void ProcessNotifyBuffer(const std::wstring& root, BYTE* buffer, DWORD bytes) {
        BYTE* p = buffer;
        std::wstring oldName;
        for (;;) {
            FILE_NOTIFY_INFORMATION* fni = (FILE_NOTIFY_INFORMATION*)p;
            std::wstring full = root;
            if (!full.empty() && full.back() != L'\\') full += L'\\';
            full.append(fni->FileName, fni->FileNameLength / sizeof(wchar_t));
            if (!IsNoisyPath(full)) {
                switch (fni->Action) {
                case FILE_ACTION_ADDED: {
                    DWORD attrs = GetFileAttributesW(full.c_str());
                    bool dir = (attrs != INVALID_FILE_ATTRIBUTES) && (attrs & FILE_ATTRIBUTE_DIRECTORY);
                    PushEvent(dir ? EventType::FolderCreated : EventType::FileCreated, full, L"");
                    break;
                }
                case FILE_ACTION_REMOVED:
                    PushEvent(EventType::FileDeleted, full, L"");
                    break;
                case FILE_ACTION_RENAMED_OLD_NAME:
                    oldName = full;
                    break;
                case FILE_ACTION_RENAMED_NEW_NAME:
                    PushEvent(EventType::FileRenamed, full, oldName);
                    oldName.clear();
                    break;
                }
            }
            if (!fni->NextEntryOffset) break;
            p += fni->NextEntryOffset;
        }
    }

    struct DirWatchCtx { std::wstring root; };

    static DWORD WINAPI FileWatchThread(LPVOID param) {
        DirWatchCtx* ctx = (DirWatchCtx*)param;
        std::wstring root = ctx->root;
        delete ctx;
        HANDLE hDir = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (hDir == INVALID_HANDLE_VALUE) return 1;
        std::vector<BYTE> buffer(64 * 1024);
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ov.hEvent) { CloseHandle(hDir); return 1; }
        while (!WantStop()) {
            DWORD bytes = 0;
            BOOL ok = ReadDirectoryChangesW(hDir, buffer.data(), (DWORD)buffer.size(), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME, nullptr, &ov, nullptr);
            if (!ok) break;
            if (WaitForSingleObject(ov.hEvent, 500) == WAIT_OBJECT_0) {
                if (GetOverlappedResult(hDir, &ov, &bytes, FALSE) && bytes > 0)
                    ProcessNotifyBuffer(root, buffer.data(), bytes);
            }
        }
        CancelIo(hDir);
        DWORD tmp = 0;
        GetOverlappedResult(hDir, &ov, &tmp, TRUE);
        CloseHandle(ov.hEvent);
        CloseHandle(hDir);
        return 0;
    }

    static void StartFileWatchers() {
        wchar_t buf[1024] = {};
        if (GetLogicalDriveStringsW(1024, buf) == 0) return;
        wchar_t* p = buf;
        while (*p) {
            std::wstring root = p;
            UINT type = GetDriveTypeW(root.c_str());
            if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
                HANDLE t = CreateThread(nullptr, 0, FileWatchThread, new DirWatchCtx{ root }, 0, nullptr);
                if (t) g_threads.push_back(t);
            }
            p += lstrlenW(p) + 1;
        }
    }

    // ==================== REGISTRY ====================
    struct RegTarget { HKEY root; const wchar_t* subKey; bool suspicious; };
    static const RegTarget kRegTargets[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", true },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", true },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", true },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", true },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", true },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options", true },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", true },
        { HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\AppInit_DLLs", true },
        { HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services", false },
        { HKEY_CLASSES_ROOT,  L"exefile\\shell\\open\\command", true },
        { HKEY_CLASSES_ROOT,  L"batfile\\shell\\open\\command", true },
    };
    static const int kRegCount = (int)(sizeof(kRegTargets) / sizeof(kRegTargets[0]));

    static void SnapshotKey(HKEY hKey, const std::wstring& prefix,
        std::unordered_set<std::wstring>& out, int depth, size_t& count) {
        if (depth > 4 || count > 50000) return;
        std::vector<wchar_t> nameBuf(16384);
        for (DWORD i = 0;; ++i) {
            DWORD len = 16384;
            if (RegEnumValueW(hKey, i, nameBuf.data(), &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            out.insert(prefix + L"\\[v]" + std::wstring(nameBuf.data(), len));
            if (++count > 50000) return;
        }
        for (DWORD i = 0;; ++i) {
            DWORD len = 16384;
            if (RegEnumKeyExW(hKey, i, nameBuf.data(), &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::wstring sub(nameBuf.data(), len);
            HKEY hSub = nullptr;
            if (RegOpenKeyExW(hKey, sub.c_str(), 0, KEY_READ, &hSub) == ERROR_SUCCESS) {
                std::wstring full = prefix + L"\\" + sub;
                out.insert(full);
                SnapshotKey(hSub, full, out, depth + 1, count);
                RegCloseKey(hSub);
            }
            if (count > 50000) return;
        }
    }

    static DWORD WINAPI RegistryWatchThread(LPVOID) {
        HKEY hKeys[kRegCount] = {};
        HANDLE evs[kRegCount] = {};
        std::unordered_set<std::wstring> snaps[kRegCount];
        bool pending[kRegCount] = {};
        ULONGLONG lastDiff[kRegCount] = {};
        for (int i = 0; i < kRegCount; ++i) {
            if (RegOpenKeyExW(kRegTargets[i].root, kRegTargets[i].subKey, 0,
                KEY_READ | KEY_NOTIFY, &hKeys[i]) == ERROR_SUCCESS) {
                evs[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                size_t count = 0;
                SnapshotKey(hKeys[i], std::wstring(kRegTargets[i].subKey), snaps[i], 0, count);
                RegNotifyChangeKeyValue(hKeys[i], TRUE,
                    REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, evs[i], TRUE);
            }
        }
        while (!WantStop()) {
            HANDLE waits[1 + kRegCount];
            DWORD n = 0;
            waits[n++] = g_stopEvent;
            for (int i = 0; i < kRegCount; ++i) if (evs[i]) waits[n++] = evs[i];
            DWORD r = WaitForMultipleObjects(n, waits, FALSE, 500);
            if (r > WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + n) {
                int idx = 0, handle = (int)(r - WAIT_OBJECT_0 - 1);
                for (int i = 0; i < kRegCount; ++i) {
                    if (evs[i] && idx == handle) { pending[i] = true; break; }
                    if (evs[i]) ++idx;
                }
            }
            ULONGLONG now = GetTickCount64();
            for (int i = 0; i < kRegCount; ++i) {
                if (!pending[i] || !evs[i]) continue;
                ULONGLONG throttle = kRegTargets[i].suspicious ? 300 : 1500;
                if (now - lastDiff[i] < throttle) continue;
                pending[i] = false;
                lastDiff[i] = now;
                std::unordered_set<std::wstring> nowSet;
                size_t count = 0;
                SnapshotKey(hKeys[i], std::wstring(kRegTargets[i].subKey), nowSet, 0, count);
                int reported = 0;
                for (const auto& s : nowSet) {
                    if (reported > 50) break;
                    if (!snaps[i].count(s)) {
                        bool value = s.find(L"\\[v]") != std::wstring::npos;
                        PushEvent(EventType::RegistryChanged, s,
                            value ? L"(создано значение)" : L"(создан раздел)",
                            kRegTargets[i].suspicious);
                        ++reported;
                    }
                }
                for (const auto& s : snaps[i]) {
                    if (reported > 50) break;
                    if (!nowSet.count(s)) {
                        bool value = s.find(L"\\[v]") != std::wstring::npos;
                        PushEvent(EventType::RegistryChanged, s,
                            value ? L"(удалено значение)" : L"(удалён раздел)",
                            kRegTargets[i].suspicious);
                        ++reported;
                    }
                }
                snaps[i] = std::move(nowSet);
                RegNotifyChangeKeyValue(hKeys[i], TRUE,
                    REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, evs[i], TRUE);
            }
        }
        for (int i = 0; i < kRegCount; ++i) {
            if (evs[i]) CloseHandle(evs[i]);
            if (hKeys[i]) RegCloseKey(hKeys[i]);
        }
        return 0;
    }

    // ==================== PROCESSES ====================
    static std::wstring ReadCommandLineFromPEB(HANDLE hProc) {
        static auto NtQIP = (pfnNtQueryInformationProcess)
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
        if (!NtQIP) return L"";
        PROCESS_BASIC_INFORMATION pbi{};
        ULONG ret = 0;
        if (NtQIP(hProc, ProcessBasicInformation, &pbi, sizeof(pbi), &ret) != 0) return L"";
        if (!pbi.PebBaseAddress) return L"";
        PEB_CUSTOM peb{};
        if (!ReadProcessMemory(hProc, pbi.PebBaseAddress, &peb, sizeof(peb), nullptr)) return L"";
        if (!peb.ProcessParameters) return L"";
        RTL_UP_CUSTOM params{};
        if (!ReadProcessMemory(hProc, peb.ProcessParameters, &params, sizeof(params), nullptr)) return L"";
        if (params.CommandLine.Length == 0 || !params.CommandLine.Buffer) return L"";
        std::vector<wchar_t> buf(params.CommandLine.Length / sizeof(wchar_t) + 1);
        if (!ReadProcessMemory(hProc, params.CommandLine.Buffer, buf.data(),
            params.CommandLine.Length, nullptr)) return L"";
        buf[params.CommandLine.Length / sizeof(wchar_t)] = L'\0';
        return buf.data();
    }

    static DWORD WINAPI ProcessWatchThread(LPVOID) {
        std::map<DWORD, std::wstring> prev;
        bool first = true;
        while (!WantStop()) {
            std::map<DWORD, std::wstring> cur;
            HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (hSnap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
                if (Process32FirstW(hSnap, &pe)) {
                    do { cur[pe.th32ProcessID] = pe.szExeFile; } while (Process32NextW(hSnap, &pe));
                }
                CloseHandle(hSnap);
            }
            if (!first) {
                for (const auto& kv : cur) {
                    if (!prev.count(kv.first)) {
                        std::wstring cmdLine;
                        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, kv.first);
                        if (hProc) {
                            cmdLine = ReadCommandLineFromPEB(hProc);
                            CloseHandle(hProc);
                        }
                        std::wstring extra = L"PID " + std::to_wstring(kv.first);
                        if (!cmdLine.empty()) extra += L"  " + cmdLine;
                        PushEvent(EventType::ProcessStarted, kv.second, extra, false, kv.first, 0);
                    }
                }
                for (const auto& kv : prev) {
                    if (!cur.count(kv.first))
                        PushEvent(EventType::ProcessStopped, kv.second,
                            L"PID " + std::to_wstring(kv.first), false, kv.first, 0);
                }
            }
            prev = cur;
            first = false;
            WaitForSingleObject(g_stopEvent, 1500);
        }
        return 0;
    }

    // ==================== SERVICES ====================
    static DWORD WINAPI ServiceWatchThread(LPVOID) {
        std::map<std::wstring, DWORD> prev;
        bool first = true;
        while (!WantStop()) {
            std::map<std::wstring, DWORD> cur;
            SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
            if (scm) {
                DWORD needed = 0, count = 0, resume = 0;
                (void)EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO,
                    SERVICE_WIN32 | SERVICE_DRIVER, SERVICE_STATE_ALL,
                    nullptr, 0, &needed, &count, &resume, nullptr);
                if (needed > 0) {
                    std::vector<BYTE> buf(needed);
                    if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO,
                        SERVICE_WIN32 | SERVICE_DRIVER, SERVICE_STATE_ALL,
                        buf.data(), needed, &needed, &count, &resume, nullptr)) {
                        auto* es = (LPENUM_SERVICE_STATUS_PROCESSW)buf.data();
                        for (DWORD i = 0; i < count; ++i)
                            cur[es[i].lpServiceName] = es[i].ServiceStatusProcess.dwCurrentState;
                    }
                }
                CloseServiceHandle(scm);
            }
            if (!first) {
                for (const auto& kv : cur) {
                    auto it = prev.find(kv.first);
                    if (it == prev.end() ||
                        (it->second != SERVICE_RUNNING && kv.second == SERVICE_RUNNING)) {
                        PushEvent(EventType::ServiceStarted, kv.first, L"");
                    }
                    else if (it->second == SERVICE_RUNNING && kv.second != SERVICE_RUNNING) {
                        PushEvent(EventType::ServiceStopped, kv.first, L"");
                    }
                }
                for (const auto& kv : prev)
                    if (!cur.count(kv.first))
                        PushEvent(EventType::ServiceStopped, kv.first, L"");
            }
            prev = cur;
            first = false;
            WaitForSingleObject(g_stopEvent, 3000);
        }
        return 0;
    }

    // ==================== NETWORK ====================
    static DWORD WINAPI NetWatchThread(LPVOID) {
        std::unordered_map<ConnKey, ULONGLONG, ConnKeyHash> cache;
        ULONGLONG lastProcCleanup = GetTickCount64();
        while (!WantStop()) {
            std::unordered_map<ConnKey, ULONGLONG, ConnKeyHash> current;
            DWORD size = sizeof(MIB_TCPTABLE);
            GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
            if (size > 0) {
                std::vector<BYTE> buf(size);
                if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET,
                    TCP_TABLE_OWNER_PID_CONNECTIONS, 0) == NO_ERROR) {
                    auto* t = (PMIB_TCPTABLE_OWNER_PID)buf.data();
                    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                        auto& r = t->table[i];
                        if (r.dwState != MIB_TCP_STATE_ESTABLISHED) continue;
                        DWORD ip = r.dwRemoteAddr;
                        if ((ip & 0xFF) == 127 || (ip & 0xF0) == 0xE0 || ip == 0) continue;
                        USHORT port = (USHORT)(((r.dwRemotePort & 0xFF) << 8) | ((r.dwRemotePort >> 8) & 0xFF));
                        ConnKey key{ r.dwOwningPid, ip, port };
                        current[key] = GetTickCount64();
                        if (cache.find(key) == cache.end()) {
                            wchar_t addr[64];
                            swprintf_s(addr, L"%u.%u.%u.%u",
                                (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
                                (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
                            std::wstring ipStr = addr;
                            std::wstring resolved = ResolveIpAsync(ipStr);
                            std::wstring procName = GetProcNameCached(r.dwOwningPid);
                            const wchar_t* svc = PortToService(port);
                            wchar_t display[512];
                            if (svc[0]) swprintf_s(display, L"%s → %s:%u (%s)",
                                procName.c_str(), resolved.c_str(), port, svc);
                            else swprintf_s(display, L"%s → %s:%u",
                                procName.c_str(), resolved.c_str(), port);
                            bool suspicious = false;
                            static const wchar_t* trusted[] = {
                                L"chrome.exe", L"firefox.exe", L"msedge.exe", L"svchost.exe",
                                L"explorer.exe", L"System", L"opera.exe", L"brave.exe"
                            };
                            bool isTrusted = false;
                            for (const auto* t : trusted) {
                                if (_wcsicmp(procName.c_str(), t) == 0) { isTrusted = true; break; }
                            }
                            if (!isTrusted && port != 80 && port != 443 && port != 53) suspicious = true;
                            PushEvent(EventType::NetConnect, display, L"", suspicious, r.dwOwningPid, 0);
                        }
                    }
                }
            }
            ULONGLONG now = GetTickCount64();
            for (auto it = cache.begin(); it != cache.end(); ) {
                if (current.find(it->first) == current.end() || (now - it->second) > 60000)
                    it = cache.erase(it);
                else ++it;
            }
            for (const auto& kv : current) {
                if (cache.find(kv.first) == cache.end()) cache[kv.first] = kv.second;
            }
            if (now - lastProcCleanup > 60000) {
                std::lock_guard<std::mutex> lk(g_dnsMtx);
                g_procCache.clear();
                lastProcCleanup = now;
            }
            WaitForSingleObject(g_stopEvent, 500);
        }
        return 0;
    }

    // ==================== SYSMON ====================
    static std::wstring ExtractXmlValue(const std::wstring& xml, const std::wstring& tagName) {
        std::wstring search = L"<Data Name=\"" + tagName + L"\">";
        size_t start = xml.find(search);
        if (start == std::wstring::npos) return L"";
        start += search.length();
        size_t end = xml.find(L"</Data>", start);
        if (end == std::wstring::npos) return L"";
        return xml.substr(start, end - start);
    }

    static DWORD WINAPI SysmonCallback(EVT_SUBSCRIBE_NOTIFY_ACTION action, PVOID, EVT_HANDLE event) {
        if (action == EvtSubscribeActionDeliver) {
            DWORD bufferSize = 0;
            EvtRender(nullptr, event, EvtRenderEventXml, 0, nullptr, &bufferSize, nullptr);
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                std::vector<wchar_t> buffer(bufferSize / sizeof(wchar_t));
                if (EvtRender(nullptr, event, EvtRenderEventXml, bufferSize, buffer.data(), &bufferSize, nullptr)) {
                    std::wstring xml(buffer.data());
                    std::wstring eidStr = ExtractXmlValue(xml, L"EventID");
                    if (eidStr.empty()) return 0;
                    DWORD eventId = 0;
                    try { eventId = std::stoi(eidStr); }
                    catch (...) { return 0; }
                    std::wstring pidStr = ExtractXmlValue(xml, L"ProcessId");
                    DWORD pid = 0;
                    try { pid = std::stoi(pidStr); }
                    catch (...) {}
                    std::wstring image = ExtractXmlValue(xml, L"Image");
                    std::wstring imageName = image;
                    size_t pos = imageName.find_last_of(L"\\/");
                    if (pos != std::wstring::npos) imageName = imageName.substr(pos + 1);

                    switch (eventId) {
                    case 1: {
                        std::wstring ppidStr = ExtractXmlValue(xml, L"ParentProcessId");
                        DWORD parentPid = 0;
                        try { parentPid = std::stoi(ppidStr); }
                        catch (...) {}
                        std::wstring cmd = ExtractXmlValue(xml, L"CommandLine");
                        LockTrack();
                        if (g_rootTrackedPid != 0 && g_trackedPids.count(parentPid)) {
                            g_trackedPids.insert(pid);
                        }
                        UnlockTrack();
                        PushEvent(EventType::ProcessStarted, imageName,
                            L"PID " + std::to_wstring(pid) + (cmd.empty() ? L"" : L" | " + cmd),
                            false, pid, parentPid);
                        break;
                    }
                    case 5: {
                        PushEvent(EventType::ProcessStopped, imageName,
                            L"PID " + std::to_wstring(pid), false, pid, 0);
                        break;
                    }
                    case 11: {
                        std::wstring target = ExtractXmlValue(xml, L"TargetFilename");
                        PushEvent(EventType::FileCreated, target,
                            L"PID " + std::to_wstring(pid) + L" | " + imageName, false, pid, 0);
                        break;
                    }
                    case 12: case 13: case 14: {
                        std::wstring target = ExtractXmlValue(xml, L"TargetObject");
                        std::wstring details = ExtractXmlValue(xml, L"Details");
                        PushEvent(EventType::RegistryChanged, target,
                            L"PID " + std::to_wstring(pid) + L" | " + imageName +
                            (details.empty() ? L"" : L" | " + details), false, pid, 0);
                        break;
                    }
                    case 3: {
                        std::wstring ip = ExtractXmlValue(xml, L"DestinationIp");
                        std::wstring port = ExtractXmlValue(xml, L"DestinationPort");
                        PushEvent(EventType::NetConnect,
                            imageName + L" → " + ip + L":" + port,
                            L"PID " + std::to_wstring(pid), false, pid, 0);
                        break;
                    }
                    }
                }
            }
        }
        return 0;
    }

    static void StartSysmon() {
        if (!g_useSysmon) return;
        g_sysmonSubscription = EvtSubscribe(nullptr, nullptr,
            L"Microsoft-Windows-Sysmon/Operational",
            g_sysmonXPath.c_str(), nullptr, nullptr,
            (EVT_SUBSCRIBE_CALLBACK)SysmonCallback, EvtSubscribeToFutureEvents);
        g_sysmonAvailable = (g_sysmonSubscription != nullptr);
    }

    // ==================== TRACKING API ====================
    void StartTrackingProcess(DWORD rootPid, const std::wstring& rootName) {
        LockTrack();
        g_rootTrackedPid = rootPid;
        g_rootTrackedName = rootName;
        g_trackedPids.clear();
        g_trackedPids.insert(rootPid);
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
            if (Process32FirstW(hSnap, &pe)) {
                do {
                    if (g_trackedPids.count(pe.th32ParentProcessID)) {
                        g_trackedPids.insert(pe.th32ProcessID);
                    }
                } while (Process32NextW(hSnap, &pe));
            }
            CloseHandle(hSnap);
        }
        UnlockTrack();
    }

    void StopTrackingProcess() {
        LockTrack();
        g_rootTrackedPid = 0;
        g_rootTrackedName.clear();
        g_trackedPids.clear();
        UnlockTrack();
    }

    bool IsTrackingProcess() {
        LockTrack();
        bool res = (g_rootTrackedPid != 0);
        UnlockTrack();
        return res;
    }

    DWORD GetTrackedRootPid() {
        LockTrack();
        DWORD res = g_rootTrackedPid;
        UnlockTrack();
        return res;
    }

    std::wstring GetTrackedRootName() {
        LockTrack();
        std::wstring res = g_rootTrackedName;
        UnlockTrack();
        return res;
    }

    std::vector<DWORD> GetTrackedPids() {
        LockTrack();
        std::vector<DWORD> res(g_trackedPids.begin(), g_trackedPids.end());
        UnlockTrack();
        return res;
    }

    std::vector<std::pair<DWORD, DWORD>> GetProcessTree() {
        std::vector<std::pair<DWORD, DWORD>> tree;
        LockTrack();
        if (g_rootTrackedPid == 0) { UnlockTrack(); return tree; }
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
            if (Process32FirstW(hSnap, &pe)) {
                do {
                    if (g_trackedPids.count(pe.th32ProcessID)) {
                        tree.push_back({ pe.th32ParentProcessID, pe.th32ProcessID });
                    }
                } while (Process32NextW(hSnap, &pe));
            }
            CloseHandle(hSnap);
        }
        UnlockTrack();
        return tree;
    }

    bool IsSysmonAvailable() { return g_sysmonAvailable; }
    void SetSysmonXPathFilter(const std::wstring& xpath) { g_sysmonXPath = xpath; }
    std::wstring GetSysmonXPathFilter() { return g_sysmonXPath; }

    // ==================== CONTROLS ====================
    bool IsRunning() { return g_running; }

    void Start(bool files, bool registry, bool processes, bool services, bool network, bool useSysmon) {
        if (g_running) return;
        if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
        EnsureTrackCs();
        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_running = true;
        g_useSysmon = useSysmon;
        if (g_useSysmon) StartSysmon();
        if (!g_sysmonAvailable) {
            if (files)     StartFileWatchers();
            if (registry) { HANDLE t = CreateThread(nullptr, 0, RegistryWatchThread, nullptr, 0, nullptr); if (t) g_threads.push_back(t); }
            if (processes) { HANDLE t = CreateThread(nullptr, 0, ProcessWatchThread, nullptr, 0, nullptr); if (t) g_threads.push_back(t); }
            if (services) { HANDLE t = CreateThread(nullptr, 0, ServiceWatchThread, nullptr, 0, nullptr); if (t) g_threads.push_back(t); }
            if (network) { HANDLE t = CreateThread(nullptr, 0, NetWatchThread, nullptr, 0, nullptr); if (t) g_threads.push_back(t); }
        }
    }

    void Stop() {
        if (!g_running) return;
        SetEvent(g_stopEvent);
        if (g_sysmonSubscription) {
            EvtClose(g_sysmonSubscription);
            g_sysmonSubscription = nullptr;
        }
        if (!g_threads.empty())
            WaitForMultipleObjects((DWORD)g_threads.size(), g_threads.data(), TRUE, 5000);
        for (HANDLE h : g_threads) CloseHandle(h);
        g_threads.clear();
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        g_running = false;
        StopTrackingProcess();
        std::lock_guard<std::mutex> lk(g_dnsMtx);
        g_dnsCache.clear();
        g_procCache.clear();
    }

    std::vector<Event> GetEvents() {
        if (!g_csInit) return {};
        Lock();
        std::vector<Event> copy = g_events;
        Unlock();
        return copy;
    }

    void Clear() {
        if (!g_csInit) return;
        Lock(); g_events.clear(); Unlock();
    }

    bool SaveReport(const std::wstring& path) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        const char bom[3] = { (char)0xEF, (char)0xBB, (char)0xBF };
        DWORD written = 0;
        WriteFile(h, bom, 3, &written, nullptr);
        std::string acc;
        for (const auto& e : GetEvents()) {
            std::wstring line = EventToString(e) + L"\r\n";
            int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (n > 0) {
                std::vector<char> buf(n);
                WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, buf.data(), n, nullptr, nullptr);
                acc.append(buf.data(), (size_t)n - 1);
            }
            if (acc.size() > (1 << 20)) {
                WriteFile(h, acc.data(), (DWORD)acc.size(), &written, nullptr);
                acc.clear();
            }
        }
        if (!acc.empty()) WriteFile(h, acc.data(), (DWORD)acc.size(), &written, nullptr);
        CloseHandle(h);
        return true;
    }

} // namespace ActivityMonitor