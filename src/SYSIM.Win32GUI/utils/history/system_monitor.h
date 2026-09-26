#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace ActivityMonitor {
    enum class EventType {
        FileCreated, FileDeleted, FileRenamed,
        FolderCreated, FolderDeleted,
        RegistryChanged,
        ProcessStarted, ProcessStopped,
        ServiceStarted, ServiceStopped,
        NetConnect,
    };

    struct Event {
        SYSTEMTIME time{};
        EventType type = EventType::FileCreated;
        std::wstring path;
        std::wstring extra;
        bool suspicious = false;
        DWORD pid = 0;        // PID процесса
        DWORD parentPid = 0;  // PID родителя
    };

    const wchar_t* TypeToString(EventType t);
    std::wstring EventToString(const Event& e);

    bool IsRunning();
    void Start(bool files, bool registry, bool processes, bool services, bool network, bool useSysmon);
    void Stop();
    std::vector<Event> GetEvents();
    void Clear();
    bool SaveReport(const std::wstring& path);

    struct Stats {
        int files = 0, registry = 0, processes = 0, services = 0, network = 0;
    };
    Stats GetStats();

    // API для слежки за процессом
    void StartTrackingProcess(DWORD rootPid, const std::wstring& rootName);
    void StopTrackingProcess();
    bool IsTrackingProcess();
    DWORD GetTrackedRootPid();
    std::wstring GetTrackedRootName();
    std::vector<DWORD> GetTrackedPids();
    std::vector<std::pair<DWORD, DWORD>> GetProcessTree();

    // Статус Sysmon
    bool IsSysmonAvailable();
    void SetSysmonXPathFilter(const std::wstring& xpath);
    std::wstring GetSysmonXPathFilter();
}