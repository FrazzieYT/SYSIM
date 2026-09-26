#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace OfflineServiceManager {

    struct OfflineServiceInfo {
        std::wstring name;
        std::wstring displayName;
        std::wstring imagePath;   // резолвлен в путь системы
        std::wstring description;
        DWORD start = 4;          // SERVICE_*
        DWORD type = 0;          // SERVICE_*
        bool isDriver = false;
        bool isService = false;
    };

    // mountName — подраздел HKLM, куда смонтирован SYSTEM (напр. "OfflineSystem")
    // targetWindowsPath — "D:\Windows" (для резолва \SystemRoot\...)
    // driversOnly=true — только kernel/fs-драйверы, false — только Win32-сервисы
    std::vector<OfflineServiceInfo> Enumerate(
        const std::wstring& mountName,
        const std::wstring& targetWindowsPath,
        bool driversOnly);

    // Записать Start в реестр. startType = 0..4
    bool SetStartType(
        const std::wstring& mountName,
        const std::wstring& serviceName,
        DWORD startType);
}