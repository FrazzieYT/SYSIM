#include "offline_service_manager.h"
#include <algorithm>
#pragma comment(lib, "advapi32.lib")

namespace OfflineServiceManager {

    static DWORD GetCurrentControlSet(const std::wstring& mount) {
        // 1. Пробуем Select\Current
        std::wstring selectPath = mount + L"\\Select";
        HKEY hSelect = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, selectPath.c_str(), 0,
            KEY_READ, &hSelect) == ERROR_SUCCESS)
        {
            DWORD current = 0, size = sizeof(current), type = 0;
            if (RegQueryValueExW(hSelect, L"Current", nullptr, &type,
                reinterpret_cast<LPBYTE>(&current), &size) == ERROR_SUCCESS
                && type == REG_DWORD
                && current >= 1 && current <= 99)
            {
                // Проверим, что ControlSetNNN\Services существует
                wchar_t buf[64];
                swprintf_s(buf, L"\\ControlSet%03u\\Services", current);
                std::wstring full = mount + buf;
                HKEY hTest = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full.c_str(), 0,
                    KEY_READ, &hTest) == ERROR_SUCCESS)
                {
                    RegCloseKey(hTest);
                    RegCloseKey(hSelect);
                    return current;
                }
            }
            RegCloseKey(hSelect);
        }

        // 2. Fallback: перебираем ControlSet001..ControlSet099
        for (DWORD i = 1; i <= 99; ++i) {
            wchar_t buf[64];
            swprintf_s(buf, L"\\ControlSet%03u\\Services", i);
            std::wstring full = mount + buf;
            HKEY hTest = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full.c_str(), 0,
                KEY_READ, &hTest) == ERROR_SUCCESS)
            {
                RegCloseKey(hTest);
                return i;
            }
        }
        return 1;   // ничего не нашли пусть будет 1, Enumerate вернёт пусто
    }

    static std::wstring BuildServicesPath(const std::wstring& mount) {
        DWORD ccs = GetCurrentControlSet(mount);
        wchar_t buf[64];
        swprintf_s(buf, L"\\ControlSet%03u\\Services", ccs);
        return mount + buf;
    }

    static std::wstring ResolvePath(const std::wstring& imagePath,
        const std::wstring& winPath) {
        if (imagePath.empty()) return imagePath;
        std::wstring p = imagePath;

        // 1. %SystemRoot%\... -> winPath\...
        const std::wstring varRoot = L"%SystemRoot%";
        size_t varPos = p.find(varRoot);
        if (varPos != std::wstring::npos && !winPath.empty()) {
            p.replace(varPos, varRoot.size(), winPath);
        }

        // 2. \SystemRoot\... -> winPath\...
        const std::wstring sysRoot = L"\\SystemRoot\\";
        if (!winPath.empty() &&
            p.size() >= sysRoot.size() &&
            _wcsnicmp(p.c_str(), sysRoot.c_str(), sysRoot.size()) == 0)
        {
            p = winPath + p.substr(sysRoot.size() - 1);
        }
        // 3. \??\C:\... -> C:\...
        else if (p.size() >= 4 &&
            p[0] == L'\\' && p[1] == L'?' &&
            p[2] == L'?' && p[3] == L'\\')
        {
            p = p.substr(4);
        }

        // 4. Снять кавычки
        if (!p.empty() && p[0] == L'"') {
            size_t e = p.find(L'"', 1);
            if (e != std::wstring::npos) p = p.substr(1, e - 1);
        }

        // 5. Драйвер: до .sys включительно
        size_t sysEnd = p.find(L".sys");
        if (sysEnd != std::wstring::npos) {
            return p.substr(0, sysEnd + 4);
        }

        // 6. Win32-сервис: до .exe, отрезать аргументы
        size_t exeEnd = p.find(L".exe");
        if (exeEnd != std::wstring::npos) {
            size_t after = exeEnd + 4;
            if (after < p.size() && (p[after] == L' ' || p[after] == L'"'))
                return p.substr(0, after);
            return p;
        }

        return p;
    }

    std::vector<OfflineServiceInfo> Enumerate(
        const std::wstring& mountName,
        const std::wstring& targetWindowsPath,
        bool driversOnly)
    {
        std::vector<OfflineServiceInfo> result;
        if (mountName.empty()) return result;

        std::wstring servicesPath = BuildServicesPath(mountName);

        HKEY hServices = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, servicesPath.c_str(), 0,
            KEY_ENUMERATE_SUB_KEYS | KEY_READ, &hServices) != ERROR_SUCCESS)
            return result;

        wchar_t subName[512];
        DWORD idx = 0;
        while (true) {
            DWORD len = 512;
            if (RegEnumKeyExW(hServices, idx++, subName, &len,
                nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;

            HKEY hSvc = nullptr;
            if (RegOpenKeyExW(hServices, subName, 0, KEY_READ, &hSvc)
                != ERROR_SUCCESS)
                continue;

            DWORD type = 0, ts = sizeof(type);
            RegQueryValueExW(hSvc, L"Type", nullptr, nullptr,
                reinterpret_cast<LPBYTE>(&type), &ts);

            bool isDriver = (type & SERVICE_DRIVER) != 0;   // KERNEL|FILE_SYSTEM|RECOGNIZER
            bool isService = (type & SERVICE_WIN32) != 0;   // OWN|SHARE

            if (driversOnly && !isDriver) { RegCloseKey(hSvc); continue; }
            if (!driversOnly && !isService) { RegCloseKey(hSvc); continue; }

            OfflineServiceInfo info;
            info.name = subName;
            info.type = type;
            info.isDriver = isDriver;
            info.isService = isService;

            wchar_t buf[4096];
            DWORD size;

            size = sizeof(buf);
            if (RegQueryValueExW(hSvc, L"ImagePath", nullptr, nullptr,
                reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
                info.imagePath = ResolvePath(buf, targetWindowsPath);
            }

            size = sizeof(buf);
            if (RegQueryValueExW(hSvc, L"DisplayName", nullptr, nullptr,
                reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
                info.displayName = buf;
            }
            else {
                info.displayName = subName;
            }

            size = sizeof(buf);
            if (RegQueryValueExW(hSvc, L"Description", nullptr, nullptr,
                reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
                info.description = buf;
            }

            DWORD start = 4, ss = sizeof(start);
            RegQueryValueExW(hSvc, L"Start", nullptr, nullptr,
                reinterpret_cast<LPBYTE>(&start), &ss);
            info.start = start;

            RegCloseKey(hSvc);
            result.push_back(std::move(info));
        }

        RegCloseKey(hServices);

        std::sort(result.begin(), result.end(),
            [](const OfflineServiceInfo& a, const OfflineServiceInfo& b) {
                return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
            });
        return result;
    }

    bool SetStartType(const std::wstring& mountName,
        const std::wstring& serviceName,
        DWORD startType)
    {
        if (mountName.empty() || serviceName.empty()) return false;
        if (startType > 4) return false;

        std::wstring full = BuildServicesPath(mountName) + L"\\" + serviceName;
        HKEY hSvc = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full.c_str(), 0,
            KEY_SET_VALUE, &hSvc) != ERROR_SUCCESS)
            return false;

        LONG r = RegSetValueExW(hSvc, L"Start", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&startType), sizeof(startType));
        RegCloseKey(hSvc);
        return r == ERROR_SUCCESS;
    }
}