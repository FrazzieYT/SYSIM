#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace ServiceManager {

    // Service states (Win32 compatible)
    enum class ServiceState {
        Stopped = SERVICE_STOPPED,
        StartPending = SERVICE_START_PENDING,
        StopPending = SERVICE_STOP_PENDING,
        Running = SERVICE_RUNNING,
        ContinuePending = SERVICE_CONTINUE_PENDING,
        PausePending = SERVICE_PAUSE_PENDING,
        Paused = SERVICE_PAUSED,
        Unknown
    };

    // Service startup type
    enum class ServiceStartType {
        Boot = SERVICE_BOOT_START,
        System = SERVICE_SYSTEM_START,
        Auto = SERVICE_AUTO_START,
        Demand = SERVICE_DEMAND_START,
        Disabled = SERVICE_DISABLED,
        Unknown
    };

    struct ServiceInfo {
        std::wstring name;
        std::wstring displayName;
        std::wstring description;
        ServiceState state = ServiceState::Unknown;
        ServiceStartType startType = ServiceStartType::Unknown;
        DWORD pid = 0;
    };

    // Get services with optional state filter
    std::vector<ServiceInfo> GetServices(ServiceState filter = ServiceState::Unknown);
    bool StartService(const std::wstring& serviceName);
    bool StopService(const std::wstring& serviceName);
    bool PauseService(const std::wstring& serviceName);
    bool ResumeService(const std::wstring& serviceName);
    bool SetStartType(const std::wstring& serviceName, ServiceStartType startType);
    bool GetServiceDetails(const std::wstring& serviceName, ServiceInfo& outInfo);

}