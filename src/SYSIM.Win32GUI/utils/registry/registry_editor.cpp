#include "registry_editor.h"
#include <fstream>
#include <functional>
#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#pragma comment(lib, "advapi32.lib")

namespace RegistryEditor {

    // Helper functions
    static bool EnablePrivilegeInternal(const wchar_t* privilegeName) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
            &token
        )) {
            return false;
        }

        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        if (!LookupPrivilegeValueW(
            nullptr,
            privilegeName,
            &tp.Privileges[0].Luid
        )) {
            CloseHandle(token);
            return false;
        }

        BOOL ok = AdjustTokenPrivileges(
            token,
            FALSE,
            &tp,
            sizeof(tp),
            nullptr,
            nullptr
        );

        DWORD error = GetLastError();
        CloseHandle(token);
        return ok && error == ERROR_SUCCESS;
    }

    static bool EnableBackupRestorePrivilegesInternal() {
        bool backup = EnablePrivilegeInternal(L"SeBackupPrivilege");
        bool restore = EnablePrivilegeInternal(L"SeRestorePrivilege");
        return backup || restore;
    }

    static std::wstring RootName(HKEY root) {
        if (root == HKEY_CLASSES_ROOT)   return L"HKEY_CLASSES_ROOT";
        if (root == HKEY_CURRENT_USER)   return L"HKEY_CURRENT_USER";
        if (root == HKEY_LOCAL_MACHINE)  return L"HKEY_LOCAL_MACHINE";
        if (root == HKEY_USERS)          return L"HKEY_USERS";
        if (root == HKEY_CURRENT_CONFIG) return L"HKEY_CURRENT_CONFIG";
        return L"";
    }

    static bool ParseFullRegistryPath(
        const std::wstring& fullPath,
        HKEY& root,
        std::wstring& subKey
    ) {
        struct RootInfo {
            const wchar_t* prefix;
            HKEY root;
        };

        static const RootInfo roots[] = {
            { L"HKEY_CLASSES_ROOT",   HKEY_CLASSES_ROOT },
            { L"HKEY_CURRENT_USER",   HKEY_CURRENT_USER },
            { L"HKEY_LOCAL_MACHINE",  HKEY_LOCAL_MACHINE },
            { L"HKEY_USERS",          HKEY_USERS },
            { L"HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG },
        };

        for (const auto& r : roots) {
            size_t len = wcslen(r.prefix);
            if (fullPath.compare(0, len, r.prefix) == 0) {
                root = r.root;
                if (fullPath.size() > len && fullPath[len] == L'\\') {
                    subKey = fullPath.substr(len + 1);
                }
                else {
                    subKey = fullPath.substr(len);
                }
                return true;
            }
        }
        return false;
    }

    static bool ReadValueRaw(
        HANDLE hKey,
        const std::wstring& valueName,
        RegValue& out
    ) {
        DWORD type = 0;
        DWORD size = 0;

        if (RegQueryValueExW(
            reinterpret_cast<HKEY>(hKey),
            valueName.c_str(),
            nullptr,
            &type,
            nullptr,
            &size
        ) != ERROR_SUCCESS) {
            return false;
        }

        out.name = valueName;
        out.type = static_cast<RegValueType>(type);
        out.data.resize(size);

        if (size > 0) {
            if (RegQueryValueExW(
                reinterpret_cast<HKEY>(hKey),
                valueName.c_str(),
                nullptr,
                &type,
                out.data.data(),
                &size
            ) != ERROR_SUCCESS) {
                out.data.clear();
                return false;
            }
        }
        return true;
    }

    // Basic operations
    HANDLE OpenKey(HKEY root, const std::wstring& subKey, REGSAM access) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(root, subKey.c_str(), 0, access, &hKey) == ERROR_SUCCESS) {
            return static_cast<HANDLE>(hKey);
        }
        return nullptr;
    }

    void CloseKey(HANDLE hKey) {
        if (hKey) {
            RegCloseKey(reinterpret_cast<HKEY>(hKey));
        }
    }

    std::vector<std::wstring> EnumSubKeys(HANDLE hKey) {
        std::vector<std::wstring> result;
        DWORD subKeyCount = 0;
        DWORD maxSubKeyLen = 0;

        if (RegQueryInfoKeyW(
            reinterpret_cast<HKEY>(hKey),
            nullptr, nullptr, nullptr,
            &subKeyCount, &maxSubKeyLen, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr
        ) != ERROR_SUCCESS) {
            return result;
        }

        if (subKeyCount == 0) {
            return result;
        }

        std::vector<wchar_t> buffer(maxSubKeyLen + 1);
        for (DWORD i = 0; i < subKeyCount; ++i) {
            DWORD nameLen = maxSubKeyLen + 1;
            if (RegEnumKeyExW(
                reinterpret_cast<HKEY>(hKey),
                i,
                buffer.data(),
                &nameLen,
                nullptr, nullptr, nullptr, nullptr
            ) == ERROR_SUCCESS) {
                result.emplace_back(buffer.data(), nameLen);
            }
        }
        return result;
    }

    std::vector<RegValue> EnumValues(HANDLE hKey) {
        std::vector<RegValue> result;
        DWORD valueCount = 0;
        DWORD maxValueNameLen = 0;
        DWORD maxValueDataLen = 0;

        if (RegQueryInfoKeyW(
            reinterpret_cast<HKEY>(hKey),
            nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr,
            &valueCount, &maxValueNameLen, &maxValueDataLen,
            nullptr, nullptr
        ) != ERROR_SUCCESS) {
            return result;
        }

        if (valueCount == 0) {
            return result;
        }

        std::vector<wchar_t> nameBuffer(maxValueNameLen + 1);
        std::vector<BYTE> dataBuffer(maxValueDataLen ? maxValueDataLen : 1);

        for (DWORD i = 0; i < valueCount; ++i) {
            DWORD nameLen = maxValueNameLen + 1;
            DWORD dataLen = maxValueDataLen;
            DWORD type = 0;

            if (RegEnumValueW(
                reinterpret_cast<HKEY>(hKey),
                i,
                nameBuffer.data(),
                &nameLen,
                nullptr,
                &type,
                dataBuffer.data(),
                &dataLen
            ) == ERROR_SUCCESS) {
                RegValue val;
                val.name.assign(nameBuffer.data(), nameLen);
                val.type = static_cast<RegValueType>(type);
                val.data.assign(dataBuffer.begin(), dataBuffer.begin() + dataLen);
                result.push_back(std::move(val));
            }
        }
        return result;
    }

    RegValue ReadValue(HANDLE hKey, const std::wstring& valueName) {
        RegValue result;
        result.name = valueName;
        DWORD type = 0;
        DWORD size = 0;

        if (RegQueryValueExW(
            reinterpret_cast<HKEY>(hKey),
            valueName.c_str(),
            nullptr,
            &type,
            nullptr,
            &size
        ) != ERROR_SUCCESS) {
            result.type = RegValueType::None;
            return result;
        }

        result.type = static_cast<RegValueType>(type);
        result.data.resize(size);

        if (size > 0) {
            if (RegQueryValueExW(
                reinterpret_cast<HKEY>(hKey),
                valueName.c_str(),
                nullptr,
                &type,
                result.data.data(),
                &size
            ) != ERROR_SUCCESS) {
                result.data.clear();
            }
        }
        return result;
    }

    bool WriteValue(HANDLE hKey, const RegValue& value) {
        DWORD type = static_cast<DWORD>(value.type);
        DWORD size = static_cast<DWORD>(value.data.size());

        return RegSetValueExW(
            reinterpret_cast<HKEY>(hKey),
            value.name.c_str(),
            0,
            type,
            value.data.data(),
            size
        ) == ERROR_SUCCESS;
    }

    bool DeleteValue(HANDLE hKey, const std::wstring& valueName) {
        return RegDeleteValueW(
            reinterpret_cast<HKEY>(hKey),
            valueName.c_str()
        ) == ERROR_SUCCESS;
    }

    bool DeleteKeyRecursive(HKEY root, const std::wstring& subKey) {
        return RegDeleteTreeW(
            root,
            subKey.empty() ? nullptr : subKey.c_str()
        ) == ERROR_SUCCESS;
    }

    bool CreateKey(HKEY root, const std::wstring& subKey, HANDLE& outKey) {
        HKEY hKey = nullptr;
        DWORD disposition = 0;

        if (RegCreateKeyExW(
            root,
            subKey.c_str(),
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_ALL_ACCESS,
            nullptr,
            &hKey,
            &disposition
        ) == ERROR_SUCCESS) {
            outKey = static_cast<HANDLE>(hKey);
            return true;
        }

        outKey = nullptr;
        return false;
    }

    RegKeyInfo GetKeyInfo(HANDLE hKey) {
        RegKeyInfo info;
        DWORD subKeys = 0;
        DWORD values = 0;

        if (RegQueryInfoKeyW(
            reinterpret_cast<HKEY>(hKey),
            nullptr, nullptr, nullptr,
            &subKeys, nullptr, nullptr,
            &values, nullptr, nullptr, nullptr,
            &info.lastWriteTime
        ) == ERROR_SUCCESS) {
            info.subKeyCount = subKeys;
            info.valueCount = values;
        }
        return info;
    }

    // Copy / move / rename
    static bool CopyKeyRecursive(
        HKEY srcKey,
        const std::wstring& srcSubKey,
        HKEY dstKey,
        const std::wstring& dstSubKey
    ) {
        HKEY hSrc = reinterpret_cast<HKEY>(
            OpenKey(srcKey, srcSubKey, KEY_READ)
            );
        if (!hSrc) {
            return false;
        }

        HKEY hDst = nullptr;
        if (!CreateKey(dstKey, dstSubKey, reinterpret_cast<HANDLE&>(hDst))) {
            CloseKey(static_cast<HANDLE>(hSrc));
            return false;
        }

        auto values = EnumValues(static_cast<HANDLE>(hSrc));
        for (const auto& val : values) {
            WriteValue(static_cast<HANDLE>(hDst), val);
        }

        auto subKeys = EnumSubKeys(static_cast<HANDLE>(hSrc));
        for (const auto& sub : subKeys) {
            if (!CopyKeyRecursive(hSrc, sub, hDst, sub)) {
                CloseKey(static_cast<HANDLE>(hSrc));
                CloseKey(static_cast<HANDLE>(hDst));
                return false;
            }
        }

        CloseKey(static_cast<HANDLE>(hSrc));
        CloseKey(static_cast<HANDLE>(hDst));
        return true;
    }

    bool CopyKey(
        HKEY rootSrc,
        const std::wstring& srcPath,
        HKEY rootDst,
        const std::wstring& dstPath
    ) {
        return CopyKeyRecursive(rootSrc, srcPath, rootDst, dstPath);
    }

    bool MoveKey(
        HKEY rootSrc,
        const std::wstring& srcPath,
        HKEY rootDst,
        const std::wstring& dstPath
    ) {
        if (!CopyKey(rootSrc, srcPath, rootDst, dstPath)) {
            return false;
        }
        return DeleteKeyRecursive(rootSrc, srcPath);
    }

    bool RenameKey(
        HKEY root,
        const std::wstring& oldPath,
        const std::wstring& newName
    ) {
        std::wstring parent = oldPath;
        size_t pos = parent.find_last_of(L'\\');

        if (pos != std::wstring::npos) {
            parent = parent.substr(0, pos);
        }
        else {
            parent.clear();
        }

        std::wstring newPath =
            parent.empty()
            ? newName
            : parent + L"\\" + newName;

        if (!CopyKey(root, oldPath, root, newPath)) {
            return false;
        }
        return DeleteKeyRecursive(root, oldPath);
    }

    bool CopyValue(
        HANDLE hKey,
        const std::wstring& valueName,
        const std::wstring& destKeyFullPath
    ) {
        HKEY root = nullptr;
        std::wstring subKey;

        if (!ParseFullRegistryPath(destKeyFullPath, root, subKey)) {
            return false;
        }

        HANDLE hDest = OpenKey(root, subKey, KEY_SET_VALUE);
        if (!hDest) {
            return false;
        }

        RegValue val;
        bool ok = false;

        if (ReadValueRaw(hKey, valueName, val)) {
            ok = WriteValue(hDest, val);
        }

        CloseKey(hDest);
        return ok;
    }

    bool MoveValue(
        HANDLE hKey,
        const std::wstring& valueName,
        const std::wstring& destKeyFullPath
    ) {
        if (!CopyValue(hKey, valueName, destKeyFullPath)) {
            return false;
        }
        return DeleteValue(hKey, valueName);
    }

    bool RenameValue(
        HANDLE hKey,
        const std::wstring& oldName,
        const std::wstring& newName
    ) {
        RegValue val;
        if (!ReadValueRaw(hKey, oldName, val)) {
            return false;
        }

        val.name = newName;

        if (!WriteValue(hKey, val)) {
            return false;
        }

        return DeleteValue(hKey, oldName);
    }

    // Search
    std::vector<SearchResult> SearchRegistry(
        HKEY root,
        const std::wstring& searchText,
        bool searchNames,
        bool searchValues,
        bool searchData,
        DWORD maxResults
    ) {
        return SearchRegistryAtPath(
            root,
            L"",
            searchText,
            searchNames,
            searchValues,
            searchData,
            maxResults
        );
    }

    std::vector<SearchResult> SearchRegistryAtPath(
        HKEY root,
        const std::wstring& startPath,
        const std::wstring& searchText,
        bool searchNames,
        bool searchValues,
        bool searchData,
        DWORD maxResults
    ) {
        std::vector<SearchResult> results;

        if (searchText.empty()) {
            return results;
        }

        HKEY hStart = root;
        bool closeStart = false;

        if (!startPath.empty()) {
            hStart = reinterpret_cast<HKEY>(
                OpenKey(root, startPath, KEY_READ)
                );
            if (!hStart) {
                return results;
            }
            closeStart = true;
        }

        std::function<void(HKEY, const std::wstring&)> searchRecursive =
            [&](HKEY hKey, const std::wstring& currentPath) {
            if (results.size() >= maxResults) {
                return;
            }

            if (searchNames && !currentPath.empty()) {
                size_t pos = currentPath.find_last_of(L'\\');
                std::wstring keyName =
                    (pos != std::wstring::npos)
                    ? currentPath.substr(pos + 1)
                    : currentPath;

                if (keyName.find(searchText) != std::wstring::npos) {
                    SearchResult sr;
                    sr.keyPath = currentPath;
                    sr.valueName.clear();
                    results.push_back(sr);
                }
            }

            auto values = EnumValues(static_cast<HANDLE>(hKey));
            for (const auto& val : values) {
                if (results.size() >= maxResults) {
                    break;
                }

                if (searchNames && val.name.find(searchText) != std::wstring::npos) {
                    SearchResult sr;
                    sr.keyPath = currentPath;
                    sr.valueName = val.name;
                    sr.type = val.type;

                    if (val.type == RegValueType::String ||
                        val.type == RegValueType::ExpandString) {
                        sr.data.assign(
                            reinterpret_cast<const wchar_t*>(val.data.data()),
                            val.data.size() / sizeof(wchar_t)
                        );
                    }
                    results.push_back(sr);
                }
                else if (searchData) {
                    if (val.type == RegValueType::String ||
                        val.type == RegValueType::ExpandString) {
                        std::wstring str;
                        str.assign(
                            reinterpret_cast<const wchar_t*>(val.data.data()),
                            val.data.size() / sizeof(wchar_t)
                        );

                        if (str.find(searchText) != std::wstring::npos) {
                            SearchResult sr;
                            sr.keyPath = currentPath;
                            sr.valueName = val.name;
                            sr.type = val.type;
                            sr.data = str;
                            results.push_back(sr);
                        }
                    }
                }
            }

            auto subKeys = EnumSubKeys(static_cast<HANDLE>(hKey));
            for (const auto& sub : subKeys) {
                if (results.size() >= maxResults) {
                    break;
                }

                HKEY hSub = reinterpret_cast<HKEY>(
                    OpenKey(hKey, sub, KEY_READ)
                    );

                if (hSub) {
                    std::wstring subPath;
                    if (currentPath.empty()) {
                        subPath = sub;
                    }
                    else {
                        subPath = currentPath + L"\\" + sub;
                    }

                    searchRecursive(hSub, subPath);
                    CloseKey(static_cast<HANDLE>(hSub));
                }
            }
            };

        searchRecursive(hStart, startPath);

        if (closeStart) {
            CloseKey(static_cast<HANDLE>(hStart));
        }

        return results;
    }

    // Favorites
    static std::wstring GetFavoritesFilePath() {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
            return L"";
        }

        std::wstring folder = exePath;
        size_t pos = folder.find_last_of(L'\\');

        if (pos != std::wstring::npos) {
            folder = folder.substr(0, pos + 1);
        }
        else {
            folder.clear();
        }

        if (folder.empty()) {
            return L"";
        }

        return folder + L"favorites.txt";
    }

    std::vector<std::wstring> GetFavorites() {
        std::vector<std::wstring> favorites;
        std::wstring file = GetFavoritesFilePath();

        if (file.empty()) {
            return favorites;
        }

        std::wifstream f(file);
        if (!f.is_open()) {
            return favorites;
        }

        std::wstring line;
        while (std::getline(f, line)) {
            if (!line.empty()) {
                favorites.push_back(line);
            }
        }

        f.close();
        return favorites;
    }

    void AddFavorite(const std::wstring& keyPath) {
        auto favs = GetFavorites();

        for (const auto& f : favs) {
            if (f == keyPath) {
                return;
            }
        }

        favs.push_back(keyPath);
        std::wstring file = GetFavoritesFilePath();

        if (file.empty()) {
            return;
        }

        std::wofstream f(file, std::ios::out | std::ios::trunc);
        if (!f.is_open()) {
            return;
        }

        for (const auto& fav : favs) {
            f << fav << L"\r\n";
        }

        f.close();
    }

    void RemoveFavorite(const std::wstring& keyPath) {
        auto favs = GetFavorites();

        favs.erase(
            std::remove(favs.begin(), favs.end(), keyPath),
            favs.end()
        );

        std::wstring file = GetFavoritesFilePath();

        if (file.empty()) {
            return;
        }

        std::wofstream f(file, std::ios::out | std::ios::trunc);
        if (!f.is_open()) {
            return;
        }

        for (const auto& fav : favs) {
            f << fav << L"\r\n";
        }

        f.close();
    }

    // Import / Export
    bool ExportKeyToRegFile(
        HKEY root,
        const std::wstring& keyPath,
        const std::wstring& filePath
    ) {
        std::wstring fullPath = RootName(root);
        if (!keyPath.empty()) {
            fullPath += L"\\" + keyPath;
        }

        std::wstring cmd =
            L"reg export \"" +
            fullPath +
            L"\" \"" +
            filePath +
            L"\" /y";

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring cmdLine = cmd;

        if (!CreateProcessW(
            nullptr,
            cmdLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi
        )) {
            return false;
        }

        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        return exitCode == 0;
    }

    bool ExportKeyToHiveFile(
        HKEY root,
        const std::wstring& keyPath,
        const std::wstring& filePath
    ) {
        HKEY hKey = reinterpret_cast<HKEY>(
            OpenKey(root, keyPath, KEY_READ)
            );

        if (!hKey) {
            return false;
        }

        bool ok = RegSaveKeyW(hKey, filePath.c_str(), nullptr) == ERROR_SUCCESS;
        CloseKey(static_cast<HANDLE>(hKey));
        return ok;
    }

    bool ImportRegFile(const std::wstring& filePath) {
        DWORD attrs = GetFileAttributesW(filePath.c_str());

        if (attrs == INVALID_FILE_ATTRIBUTES) {
            return false;
        }

        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            return false;
        }

        std::wstring cmd = L"reg import \"" + filePath + L"\"";

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring cmdLine = cmd;

        if (!CreateProcessW(
            nullptr,
            cmdLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi
        )) {
            return false;
        }

        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        return exitCode == 0;
    }

    // Permissions
    bool GetKeySecurity(HKEY hKey, PSECURITY_DESCRIPTOR& pSD) {
        DWORD size = 0;

        if (RegGetKeySecurity(
            hKey,
            DACL_SECURITY_INFORMATION,
            nullptr,
            &size
        ) == ERROR_INSUFFICIENT_BUFFER) {
            pSD = static_cast<PSECURITY_DESCRIPTOR>(malloc(size));

            if (!pSD) {
                return false;
            }

            if (RegGetKeySecurity(
                hKey,
                DACL_SECURITY_INFORMATION,
                pSD,
                &size
            ) == ERROR_SUCCESS) {
                return true;
            }

            free(pSD);
        }

        pSD = nullptr;
        return false;
    }

    bool SetKeySecurity(
        HKEY hKey,
        SECURITY_INFORMATION si,
        PSECURITY_DESCRIPTOR pSD
    ) {
        return RegSetKeySecurity(hKey, si, pSD) == ERROR_SUCCESS;
    }

    // Branch comparison
    bool CompareKeys(
        HKEY root1,
        const std::wstring& path1,
        HKEY root2,
        const std::wstring& path2,
        std::vector<std::wstring>& differences
    ) {
        HKEY hKey1 = reinterpret_cast<HKEY>(
            OpenKey(root1, path1, KEY_READ)
            );
        HKEY hKey2 = reinterpret_cast<HKEY>(
            OpenKey(root2, path2, KEY_READ)
            );

        if (!hKey1 || !hKey2) {
            if (hKey1) CloseKey(static_cast<HANDLE>(hKey1));
            if (hKey2) CloseKey(static_cast<HANDLE>(hKey2));
            return false;
        }

        auto sub1 = EnumSubKeys(static_cast<HANDLE>(hKey1));
        auto sub2 = EnumSubKeys(static_cast<HANDLE>(hKey2));

        for (const auto& s : sub1) {
            if (std::find(sub2.begin(), sub2.end(), s) == sub2.end()) {
                differences.push_back(L"Missing in second: " + path2 + L"\\" + s);
            }
        }

        for (const auto& s : sub2) {
            if (std::find(sub1.begin(), sub1.end(), s) == sub1.end()) {
                differences.push_back(L"Extra in second: " + path2 + L"\\" + s);
            }
        }

        auto val1 = EnumValues(static_cast<HANDLE>(hKey1));
        auto val2 = EnumValues(static_cast<HANDLE>(hKey2));

        for (const auto& v : val1) {
            bool found = false;

            for (const auto& v2 : val2) {
                if (v.name == v2.name &&
                    v.type == v2.type &&
                    v.data == v2.data) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                differences.push_back(L"Value different/missing: " + path1 + L"\\" + v.name);
            }
        }

        for (const auto& s : sub1) {
            if (std::find(sub2.begin(), sub2.end(), s) != sub2.end()) {
                CompareKeys(
                    root1,
                    path1 + L"\\" + s,
                    root2,
                    path2 + L"\\" + s,
                    differences
                );
            }
        }

        CloseKey(static_cast<HANDLE>(hKey1));
        CloseKey(static_cast<HANDLE>(hKey2));
        return true;
    }

    // Clipboard
    static RegistryClipboardType g_regClipType = RegistryClipboardType::None;
    static HKEY g_regClipRoot = HKEY_CURRENT_USER;
    static std::wstring g_regClipPath;
    static std::wstring g_regClipValueName;
    static bool g_regClipCut = false;

    void SetRegistryClipboard(
        RegistryClipboardType type,
        HKEY root,
        const std::wstring& path,
        const std::wstring& valueName,
        bool cut
    ) {
        g_regClipType = type;
        g_regClipRoot = root;
        g_regClipPath = path;
        g_regClipValueName = valueName;
        g_regClipCut = cut;
    }

    bool HasRegistryClipboard() {
        return g_regClipType != RegistryClipboardType::None;
    }

    bool GetRegistryClipboard(
        RegistryClipboardType& type,
        HKEY& root,
        std::wstring& path,
        std::wstring& valueName,
        bool& cut
    ) {
        if (g_regClipType == RegistryClipboardType::None) {
            return false;
        }

        type = g_regClipType;
        root = g_regClipRoot;
        path = g_regClipPath;
        valueName = g_regClipValueName;
        cut = g_regClipCut;
        return true;
    }

    void ClearRegistryClipboard() {
        g_regClipType = RegistryClipboardType::None;
        g_regClipPath.clear();
        g_regClipValueName.clear();
        g_regClipCut = false;
    }

    // WinRE (Recovery environment) / Offline hive
    bool IsLikelyRecoveryEnvironment() {
        static int cached = -1;
        if (cached != -1) {
            return cached == 1;
        }

        bool result = false;

        wchar_t systemDrive[16] = {};
        DWORD driveLen = GetEnvironmentVariableW(
            L"SystemDrive",
            systemDrive,
            sizeof(systemDrive) / sizeof(systemDrive[0])
        );

        if (driveLen > 0) {
            if (lstrcmpiW(systemDrive, L"X:") == 0) {
                result = true;
            }
        }

        if (!result) {
            wchar_t windowsDir[MAX_PATH] = {};
            if (GetWindowsDirectoryW(
                windowsDir,
                sizeof(windowsDir) / sizeof(windowsDir[0])
            ) != 0) {
                if (CompareStringOrdinal(windowsDir, 2, L"X:", 2, TRUE) == CSTR_EQUAL) {
                    result = true;
                }
            }
        }

        if (!result) {
            wchar_t systemDir[MAX_PATH] = {};
            if (GetSystemDirectoryW(
                systemDir,
                sizeof(systemDir) / sizeof(systemDir[0])
            ) != 0) {
                if (CompareStringOrdinal(systemDir, 2, L"X:", 2, TRUE) == CSTR_EQUAL) {
                    result = true;
                }
            }
        }

        if (!result) {
            HKEY hKey = nullptr;
            LONG status = RegOpenKeyExW(
                HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\MiniNT",
                0,
                KEY_READ,
                &hKey
            );

            if (status == ERROR_SUCCESS) {
                result = true;
                RegCloseKey(hKey);
            }
        }

        cached = result ? 1 : 0;
        return result;
    }

    std::vector<std::wstring> FindWindowsInstallations() {
        std::vector<std::wstring> result;
        DWORD drives = GetLogicalDrives();

        for (int i = 2; i < 26; ++i) {
            if ((drives & (1 << i)) == 0) {
                continue;
            }

            wchar_t driveLetter = static_cast<wchar_t>(L'A' + i);
            std::wstring driveRoot;
            driveRoot.push_back(driveLetter);
            driveRoot += L":\\";

            UINT type = GetDriveTypeW(driveRoot.c_str());
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) {
                continue;
            }

            std::wstring windowsPath = driveRoot + L"Windows";
            std::wstring softwareHive =
                windowsPath + L"\\System32\\config\\SOFTWARE";

            DWORD attrs = GetFileAttributesW(softwareHive.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                continue;
            }

            if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }

            result.push_back(windowsPath);
        }

        return result;
    }

    bool LoadHive(
        HKEY root,
        const std::wstring& mountName,
        const std::wstring& hiveFile
    ) {
        EnableBackupRestorePrivilegesInternal();

        DWORD attrs = GetFileAttributesW(hiveFile.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            return false;
        }

        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            return false;
        }

        LONG status = RegLoadKeyW(
            root,
            mountName.c_str(),
            hiveFile.c_str()
        );

        if (status == ERROR_SUCCESS) {
            return true;
        }

        HKEY hKey = nullptr;
        if (RegOpenKeyExW(
            root,
            mountName.c_str(),
            0,
            KEY_READ,
            &hKey
        ) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return true;
        }

        return false;
    }

    bool UnloadHive(
        HKEY root,
        const std::wstring& mountName
    ) {
        EnableBackupRestorePrivilegesInternal();

        LONG status = RegUnLoadKeyW(
            root,
            mountName.c_str()
        );

        if (status == ERROR_SUCCESS) {
            return true;
        }

        if (status == ERROR_FILE_NOT_FOUND) {
            return true;
        }

        return false;
    }

    // Inline editing helpers
    static std::vector<BYTE> MakeUnicodeStringData(const std::wstring& text) {
        std::vector<BYTE> data((text.size() + 1) * sizeof(wchar_t));
        memcpy(
            data.data(),
            text.c_str(),
            (text.size() + 1) * sizeof(wchar_t)
        );
        return data;
    }

    static std::vector<std::wstring> SplitString(
        const std::wstring& text,
        const std::wstring& delimiter
    ) {
        std::vector<std::wstring> result;

        if (text.empty()) {
            result.push_back(L"");
            return result;
        }

        size_t start = 0;

        while (true) {
            size_t pos = text.find(delimiter, start);

            if (pos == std::wstring::npos) {
                result.push_back(text.substr(start));
                break;
            }

            result.push_back(text.substr(start, pos - start));
            start = pos + delimiter.size();
        }

        return result;
    }

    static std::vector<BYTE> MakeMultiStringDataFromDisplay(
        const std::wstring& text
    ) {
        std::vector<std::wstring> parts;

        if (text.empty()) {
            parts.push_back(L"");
        }
        else {
            parts = SplitString(text, L" | ");

            if (parts.size() == 1) {
                parts = SplitString(text, L"\n");
            }
        }

        std::vector<BYTE> data;

        auto addPart = [&](const std::wstring& part) {
            size_t bytes = (part.size() + 1) * sizeof(wchar_t);
            size_t oldSize = data.size();
            data.resize(oldSize + bytes);
            memcpy(data.data() + oldSize, part.c_str(), bytes);
            };

        for (const auto& part : parts) {
            if (!part.empty()) {
                addPart(part);
            }
        }

        addPart(L"");

        return data;
    }

    static bool ParseDwordText(const std::wstring& text, DWORD& out) {
        if (text.empty()) {
            out = 0;
            return true;
        }

        wchar_t* end = nullptr;
        unsigned long value = wcstoul(text.c_str(), &end, 0);

        if (end == text.c_str()) {
            return false;
        }

        out = static_cast<DWORD>(value);
        return true;
    }

    static bool ParseQwordText(const std::wstring& text, ULONGLONG& out) {
        if (text.empty()) {
            out = 0;
            return true;
        }

        wchar_t* end = nullptr;
        unsigned long long value = wcstoull(text.c_str(), &end, 0);

        if (end == text.c_str()) {
            return false;
        }

        out = static_cast<ULONGLONG>(value);
        return true;
    }

    std::wstring ValueDataToDisplay(const RegValue& value) {
        if (value.data.empty()) {
            return L"";
        }

        switch (value.type) {
        case RegValueType::String:
        case RegValueType::ExpandString: {
            std::wstring text(
                reinterpret_cast<const wchar_t*>(value.data.data()),
                value.data.size() / sizeof(wchar_t)
            );

            size_t nullPos = text.find(L'\0');
            if (nullPos != std::wstring::npos) {
                text = text.substr(0, nullPos);
            }

            return text;
        }

        case RegValueType::MultiString: {
            const wchar_t* p =
                reinterpret_cast<const wchar_t*>(value.data.data());

            size_t chars = value.data.size() / sizeof(wchar_t);
            std::wstring result;
            size_t i = 0;

            while (i < chars && p[i] != L'\0') {
                std::wstring item = p + i;

                if (!result.empty()) {
                    result += L" | ";
                }

                result += item;
                i += item.size() + 1;
            }

            return result;
        }

        case RegValueType::DWord: {
            if (value.data.size() >= sizeof(DWORD)) {
                DWORD v = 0;
                memcpy(&v, value.data.data(), sizeof(DWORD));
                return std::to_wstring(v);
            }
            return L"";
        }

        case RegValueType::QWord: {
            if (value.data.size() >= sizeof(ULONGLONG)) {
                ULONGLONG v = 0;
                memcpy(&v, value.data.data(), sizeof(ULONGLONG));
                return std::to_wstring(v);
            }
            return L"";
        }

        case RegValueType::Binary: {
            std::wstring result;
            size_t count = (std::min)(value.data.size(), static_cast<size_t>(16));
            wchar_t buffer[8];

            for (size_t i = 0; i < count; ++i) {
                swprintf_s(buffer, L"%02X ", value.data[i]);
                result += buffer;
            }

            if (value.data.size() > 16) {
                result += L"...";
            }

            return result;
        }

        default:
            return L"(данные)";
        }
    }

    bool SetValueDataFromString(
        HKEY root,
        const std::wstring& subKey,
        const std::wstring& valueName,
        const std::wstring& text
    ) {
        HANDLE hKey = OpenKey(root, subKey, KEY_READ | KEY_SET_VALUE);

        if (!hKey) {
            return false;
        }

        if (!ValueExists(hKey, valueName)) {
            CloseKey(hKey);
            return false;
        }

        RegValue current;

        if (!ReadValueRaw(hKey, valueName, current)) {
            CloseKey(hKey);
            return false;
        }

        RegValue updated = current;

        switch (current.type) {
        case RegValueType::String:
        case RegValueType::ExpandString: {
            updated.data = MakeUnicodeStringData(text);
            break;
        }

        case RegValueType::MultiString: {
            updated.data = MakeMultiStringDataFromDisplay(text);
            break;
        }

        case RegValueType::DWord: {
            DWORD num = 0;

            if (!ParseDwordText(text, num)) {
                CloseKey(hKey);
                return false;
            }

            updated.data.resize(sizeof(DWORD));
            memcpy(updated.data.data(), &num, sizeof(DWORD));
            break;
        }

        case RegValueType::QWord: {
            ULONGLONG num = 0;

            if (!ParseQwordText(text, num)) {
                CloseKey(hKey);
                return false;
            }

            updated.data.resize(sizeof(ULONGLONG));
            memcpy(updated.data.data(), &num, sizeof(ULONGLONG));
            break;
        }

        default: {
            CloseKey(hKey);
            return false;
        }
        }

        bool ok = WriteValue(hKey, updated);
        CloseKey(hKey);
        return ok;
    }

    bool ValueExists(HANDLE hKey, const std::wstring& valueName) {
        return RegQueryValueExW(
            reinterpret_cast<HKEY>(hKey),
            valueName.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr
        ) == ERROR_SUCCESS;
    }

    bool SubKeyExists(HKEY root, const std::wstring& subKey) {
        HANDLE hKey = OpenKey(root, subKey, KEY_READ);

        if (!hKey) {
            return false;
        }

        CloseKey(hKey);
        return true;
    }

    bool RenameValueSafe(
        HKEY root,
        const std::wstring& subKey,
        const std::wstring& oldName,
        const std::wstring& newName
    ) {
        if (oldName == newName) {
            return true;
        }

        if (newName.empty()) {
            return false;
        }

        HANDLE hKey = OpenKey(root, subKey, KEY_READ | KEY_SET_VALUE);

        if (!hKey) {
            return false;
        }

        if (ValueExists(hKey, newName)) {
            CloseKey(hKey);
            return false;
        }

        bool ok = RenameValue(hKey, oldName, newName);
        CloseKey(hKey);
        return ok;
    }

    bool RenameKeySafe(
        HKEY root,
        const std::wstring& oldPath,
        const std::wstring& newName
    ) {
        if (oldPath.empty()) {
            return false;
        }

        if (newName.empty()) {
            return false;
        }

        if (newName.find_first_of(L"\\/") != std::wstring::npos) {
            return false;
        }

        std::wstring parent = oldPath;
        size_t pos = parent.find_last_of(L'\\');

        if (pos != std::wstring::npos) {
            parent = parent.substr(0, pos);
        }
        else {
            parent.clear();
        }

        std::wstring newPath =
            parent.empty() ? newName : parent + L"\\" + newName;

        if (SubKeyExists(root, newPath)) {
            return false;
        }

        return RenameKey(root, oldPath, newName);
    }

}