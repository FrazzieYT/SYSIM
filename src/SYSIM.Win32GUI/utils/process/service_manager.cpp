#include "service_manager.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <string>
#include <memory>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace ServiceManager {

    static ServiceState ToServiceState(DWORD state) {
        switch (state) {
        case SERVICE_STOPPED: return ServiceState::Stopped;
        case SERVICE_START_PENDING: return ServiceState::StartPending;
        case SERVICE_STOP_PENDING: return ServiceState::StopPending;
        case SERVICE_RUNNING: return ServiceState::Running;
        case SERVICE_CONTINUE_PENDING: return ServiceState::ContinuePending;
        case SERVICE_PAUSE_PENDING: return ServiceState::PausePending;
        case SERVICE_PAUSED: return ServiceState::Paused;
        default: return ServiceState::Unknown;
        }
    }

    static ServiceStartType ToServiceStartType(DWORD startType) {
        switch (startType) {
        case SERVICE_BOOT_START: return ServiceStartType::Boot;
        case SERVICE_SYSTEM_START: return ServiceStartType::System;
        case SERVICE_AUTO_START: return ServiceStartType::Auto;
        case SERVICE_DEMAND_START: return ServiceStartType::Demand;
        case SERVICE_DISABLED: return ServiceStartType::Disabled;
        default: return ServiceStartType::Unknown;
        }
    }

    std::vector<ServiceInfo> GetServices(ServiceState filter) {
        std::vector<ServiceInfo> result;
        SC_HANDLE scManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
        if (!scManager) return result;

        DWORD bytesNeeded = 0, servicesReturned = 0, resumeHandle = 0;
        EnumServicesStatusExW(scManager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            nullptr, 0, &bytesNeeded, &servicesReturned, &resumeHandle, nullptr);
        if (bytesNeeded == 0) {
            CloseServiceHandle(scManager);
            return result;
        }

        std::vector<BYTE> buffer(bytesNeeded);
        if (!EnumServicesStatusExW(scManager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            buffer.data(), bytesNeeded, &bytesNeeded, &servicesReturned, &resumeHandle, nullptr)) {
            CloseServiceHandle(scManager);
            return result;
        }

        ENUM_SERVICE_STATUS_PROCESSW* services = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
        for (DWORD i = 0; i < servicesReturned; ++i) {
            ServiceInfo info;
            info.name = services[i].lpServiceName;
            info.displayName = services[i].lpDisplayName;
            info.state = ToServiceState(services[i].ServiceStatusProcess.dwCurrentState);
            info.pid = services[i].ServiceStatusProcess.dwProcessId;
            info.startType = ServiceStartType::Unknown;

            if (filter != ServiceState::Unknown && info.state != filter) {
                continue;
            }

            SC_HANDLE hService = OpenServiceW(scManager, info.name.c_str(), SERVICE_QUERY_CONFIG);
            if (hService) {
                DWORD configSize = 0;
                QueryServiceConfigW(hService, nullptr, 0, &configSize);
                if (configSize > 0) {
                    std::vector<BYTE> configBuf(configSize);
                    if (QueryServiceConfigW(hService, reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(configBuf.data()), configSize, &configSize)) {
                        LPQUERY_SERVICE_CONFIGW cfg = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(configBuf.data());
                        info.displayName = cfg->lpDisplayName ? cfg->lpDisplayName : info.displayName;
                        info.startType = ToServiceStartType(cfg->dwStartType);
                    }
                }
                DWORD descSize = 0;
                QueryServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, nullptr, 0, &descSize);
                if (descSize > 0) {
                    std::vector<BYTE> descBuf(descSize);
                    if (QueryServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, descBuf.data(), descSize, &descSize)) {
                        SERVICE_DESCRIPTIONW* desc = reinterpret_cast<SERVICE_DESCRIPTIONW*>(descBuf.data());
                        if (desc->lpDescription) {
                            info.description = desc->lpDescription;
                        }
                    }
                }
                CloseServiceHandle(hService);
            }
            result.push_back(info);
        }

        CloseServiceHandle(scManager);
        return result;
    }

    bool StartService(const std::wstring& serviceName) {
        SC_HANDLE scManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scManager) return false;
        SC_HANDLE hService = OpenServiceW(scManager, serviceName.c_str(), SERVICE_START);
        if (!hService) {
            CloseServiceHandle(scManager);
            return false;
        }
        bool success = ::StartServiceW(hService, 0, nullptr) != 0;
        CloseServiceHandle(hService);
        CloseServiceHandle(scManager);
        return success;
    }

    bool StopService(const std::wstring& serviceName) {
        SC_HANDLE scManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scManager) return false;
        SC_HANDLE hService = OpenServiceW(scManager, serviceName.c_str(), SERVICE_STOP);
        if (!hService) {
            CloseServiceHandle(scManager);
            return false;
        }
        SERVICE_STATUS status;
        bool success = ControlService(hService, SERVICE_CONTROL_STOP, &status) != 0;
        CloseServiceHandle(hService);
        CloseServiceHandle(scManager);
        return success;
    }

    bool PauseService(const std::wstring& serviceName) {
        SC_HANDLE scManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scManager) return false;
        SC_HANDLE hService = OpenServiceW(scManager, serviceName.c_str(), SERVICE_PAUSE_CONTINUE);
        if (!hService) {
            CloseServiceHandle(scManager);
            return false;
        }
        SERVICE_STATUS status;
        bool success = ControlService(hService, SERVICE_CONTROL_PAUSE, &status) != 0;
        CloseServiceHandle(hService);
        CloseServiceHandle(scManager);
        return success;
    }

    bool ResumeService(const std::wstring& serviceName) {
        SC_HANDLE scManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scManager) return false;
        SC_HANDLE hService = OpenServiceW(scManager, serviceName.c_str(), SERVICE_PAUSE_CONTINUE);
        if (!hService) {
            CloseServiceHandle(scManager);
            return false;
        }
        SERVICE_STATUS status;
        bool success = ControlService(hService, SERVICE_CONTROL_CONTINUE, &status) != 0;
        CloseServiceHandle(hService);
        CloseServiceHandle(scManager);
        return success;
    }

    bool SetStartType(const std::wstring& serviceName, ServiceStartType startType) {
        DWORD dwStartType = 0;
        switch (startType) {
        case ServiceStartType::Boot: dwStartType = SERVICE_BOOT_START; break;
        case ServiceStartType::System: dwStartType = SERVICE_SYSTEM_START; break;
        case ServiceStartType::Auto: dwStartType = SERVICE_AUTO_START; break;
        case ServiceStartType::Demand: dwStartType = SERVICE_DEMAND_START; break;
        case ServiceStartType::Disabled: dwStartType = SERVICE_DISABLED; break;
        default: return false;
        }
        SC_HANDLE scManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scManager) return false;
        SC_HANDLE hService = OpenServiceW(scManager, serviceName.c_str(), SERVICE_CHANGE_CONFIG);
        if (!hService) {
            CloseServiceHandle(scManager);
            return false;
        }
        bool success = ChangeServiceConfigW(hService, SERVICE_NO_CHANGE, dwStartType, SERVICE_NO_CHANGE,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) != 0;
        CloseServiceHandle(hService);
        CloseServiceHandle(scManager);
        return success;
    }

    bool GetServiceDetails(const std::wstring& serviceName, ServiceInfo& outInfo) {
        SC_HANDLE scManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scManager) return false;
        SC_HANDLE hService = OpenServiceW(scManager, serviceName.c_str(), SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
        if (!hService) {
            CloseServiceHandle(scManager);
            return false;
        }

        SERVICE_STATUS_PROCESS status;
        DWORD bytesNeeded = 0;
        if (!QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &bytesNeeded)) {
            CloseServiceHandle(hService);
            CloseServiceHandle(scManager);
            return false;
        }

        outInfo.name = serviceName;
        outInfo.state = ToServiceState(status.dwCurrentState);
        outInfo.pid = status.dwProcessId;

        DWORD configSize = 0;
        QueryServiceConfigW(hService, nullptr, 0, &configSize);
        if (configSize > 0) {
            std::vector<BYTE> configBuf(configSize);
            if (QueryServiceConfigW(hService, reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(configBuf.data()), configSize, &configSize)) {
                LPQUERY_SERVICE_CONFIGW cfg = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(configBuf.data());
                outInfo.displayName = cfg->lpDisplayName ? cfg->lpDisplayName : L"";
                outInfo.startType = ToServiceStartType(cfg->dwStartType);
            }
        }

        DWORD descSize = 0;
        QueryServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, nullptr, 0, &descSize);
        if (descSize > 0) {
            std::vector<BYTE> descBuf(descSize);
            if (QueryServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, descBuf.data(), descSize, &descSize)) {
                SERVICE_DESCRIPTIONW* desc = reinterpret_cast<SERVICE_DESCRIPTIONW*>(descBuf.data());
                outInfo.description = desc->lpDescription ? desc->lpDescription : L"";
            }
        }

        CloseServiceHandle(hService);
        CloseServiceHandle(scManager);
        return true;
    }

}