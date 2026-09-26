#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>

namespace WinCtrl::Process {
struct Entry { DWORD th32ProcessID = 0; DWORD th32ParentProcessID = 0; wchar_t szExeFile[MAX_PATH]{}; };
struct Info { std::wstring userName; bool isCritical = false; bool isSuspended = false; int handleCount = 0; DWORD priorityClass = 0; };
inline std::vector<Entry> GetList() {
    std::vector<Entry> result; HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) do { Entry e{ pe.th32ProcessID, pe.th32ParentProcessID }; wcscpy_s(e.szExeFile, pe.szExeFile); result.push_back(e); } while (Process32NextW(snap, &pe));
    CloseHandle(snap); return result;
}
inline std::wstring GetImagePath(DWORD pid) { wchar_t path[MAX_PATH * 4]{}; DWORD size = ARRAYSIZE(path); HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); if (!h) return {}; std::wstring result = QueryFullProcessImageNameW(h, 0, path, &size) ? std::wstring(path, size) : L""; CloseHandle(h); return result; }
inline Info GetProcessInfo(DWORD pid) {
    Info i{};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return i;
    i.priorityClass = GetPriorityClass(h);
    using NtQueryInformationProcessFn = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    auto query = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    ULONG critical = 0;
    if (query && query(h, 29, &critical, sizeof(critical), nullptr) == 0)
        i.isCritical = critical != 0;
    CloseHandle(h);
    return i;
}
inline HANDLE OpenForTermination(DWORD pid, DWORD* errorCode) {
    HANDLE token = nullptr;
    TOKEN_PRIVILEGES previous{};
    bool restorePrivilege = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        TOKEN_PRIVILEGES requested{};
        requested.PrivilegeCount = 1;
        requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &requested.Privileges[0].Luid)) {
            DWORD previousSize = 0;
            SetLastError(ERROR_SUCCESS);
            restorePrivilege = AdjustTokenPrivileges(token, FALSE, &requested,
                sizeof(previous), &previous, &previousSize) != FALSE &&
                GetLastError() == ERROR_SUCCESS;
        }
    }

    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    DWORD openError = process ? ERROR_SUCCESS : GetLastError();
    if (restorePrivilege)
        AdjustTokenPrivileges(token, FALSE, &previous, 0, nullptr, nullptr);
    if (token) CloseHandle(token);
    if (!process && errorCode) *errorCode = openError;
    return process;
}

inline bool Terminate(DWORD pid, DWORD* errorCode = nullptr) {
    if (errorCode) *errorCode = ERROR_SUCCESS;
    HANDLE process = OpenForTermination(pid, errorCode);
    if (!process) return false;
    BOOL terminated = TerminateProcess(process, 1);
    DWORD terminateError = terminated ? ERROR_SUCCESS : GetLastError();
    CloseHandle(process);
    if (errorCode) *errorCode = terminateError;
    return terminated != FALSE;
}

inline bool KillProcessTree(DWORD pid, DWORD* errorCode = nullptr) {
    if (errorCode) *errorCode = ERROR_SUCCESS;
    bool childrenTerminated = true;
    for (const auto& process : GetList()) {
        if (process.th32ParentProcessID != pid) continue;
        DWORD childError = ERROR_SUCCESS;
        if (!KillProcessTree(process.th32ProcessID, &childError)) {
            childrenTerminated = false;
            if (errorCode && *errorCode == ERROR_SUCCESS) *errorCode = childError;
        }
    }
    DWORD terminateError = ERROR_SUCCESS;
    bool terminated = Terminate(pid, &terminateError);
    if (!terminated && errorCode && *errorCode == ERROR_SUCCESS)
        *errorCode = terminateError;
    return terminated && childrenTerminated;
}
inline bool SuspendProcess(DWORD pid) { HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid); if (!h) return false; using Fn = LONG (NTAPI*)(HANDLE); auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSuspendProcess")); bool ok = fn && fn(h) == 0; CloseHandle(h); return ok; }
inline bool ResumeProcess(DWORD pid) { HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid); if (!h) return false; using Fn = LONG (NTAPI*)(HANDLE); auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtResumeProcess")); bool ok = fn && fn(h) == 0; CloseHandle(h); return ok; }
inline bool SetProcessCritical(DWORD pid, bool critical) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_INFORMATION, FALSE, pid);
    if (!h) return false;
    using NtSetInformationProcessFn = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
    auto setInfo = reinterpret_cast<NtSetInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationProcess"));
    ULONG value = critical ? 1UL : 0UL;
    LONG status = setInfo ? setInfo(h, 29, &value, sizeof(value)) : static_cast<LONG>(0xC0000022L);
    CloseHandle(h);
    return status == 0;
}
inline bool SetProcessPriority(DWORD pid, DWORD priority) { HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid); if (!h) return false; BOOL ok = SetPriorityClass(h, priority); CloseHandle(h); return ok != FALSE; }
inline DWORD GetProcessPriority(DWORD pid) { HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); if (!h) return 0; DWORD p = GetPriorityClass(h); CloseHandle(h); return p; }
inline bool InjectDLL(DWORD pid, const std::wstring& path) { HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, pid); if (!h) return false; SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t); void* remote = VirtualAllocEx(h, nullptr, bytes, MEM_COMMIT, PAGE_READWRITE); bool ok = remote && WriteProcessMemory(h, remote, path.c_str(), bytes, nullptr); HMODULE kernel = GetModuleHandleW(L"kernel32.dll"); auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel, "LoadLibraryW")); HANDLE thread = ok && load ? CreateRemoteThread(h, nullptr, 0, load, remote, 0, nullptr) : nullptr; if (thread) { WaitForSingleObject(thread, 5000); CloseHandle(thread); } if (remote) VirtualFreeEx(h, remote, 0, MEM_RELEASE); CloseHandle(h); return thread != nullptr; }
}
