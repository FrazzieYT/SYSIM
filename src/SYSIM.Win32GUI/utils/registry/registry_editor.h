#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace RegistryEditor {

    // Types and structures
    enum class RegValueType : DWORD {
        None = REG_NONE,
        String = REG_SZ,
        ExpandString = REG_EXPAND_SZ,
        Binary = REG_BINARY,
        DWord = REG_DWORD,
        DWordBigEndian = REG_DWORD_BIG_ENDIAN,
        Link = REG_LINK,
        MultiString = REG_MULTI_SZ,
        ResourceList = REG_RESOURCE_LIST,
        FullResourceDescriptor = REG_FULL_RESOURCE_DESCRIPTOR,
        ResourceRequirementsList = REG_RESOURCE_REQUIREMENTS_LIST,
        QWord = REG_QWORD
    };

    struct RegValue {
        std::wstring name;
        RegValueType type = RegValueType::None;
        std::vector<BYTE> data;
    };

    struct RegKeyInfo {
        std::wstring fullPath;
        FILETIME lastWriteTime{};
        DWORD subKeyCount = 0;
        DWORD valueCount = 0;
    };

    struct SearchResult {
        std::wstring keyPath;
        std::wstring valueName;
        RegValueType type = RegValueType::None;
        std::wstring data;
    };

    enum class RegistryClipboardType {
        None,
        Key,
        Value
    };

    // Basic operations
    HANDLE OpenKey(HKEY root, const std::wstring& subKey, REGSAM access = KEY_READ);
    void CloseKey(HANDLE hKey);

    std::vector<std::wstring> EnumSubKeys(HANDLE hKey);
    std::vector<RegValue> EnumValues(HANDLE hKey);

    RegValue ReadValue(HANDLE hKey, const std::wstring& valueName);
    bool WriteValue(HANDLE hKey, const RegValue& value);
    bool DeleteValue(HANDLE hKey, const std::wstring& valueName);

    bool DeleteKeyRecursive(HKEY root, const std::wstring& subKey);
    bool CreateKey(HKEY root, const std::wstring& subKey, HANDLE& outKey);

    RegKeyInfo GetKeyInfo(HANDLE hKey);

    // Copy / move / rename
    bool CopyKey(HKEY rootSrc, const std::wstring& srcPath, HKEY rootDst, const std::wstring& dstPath);
    bool MoveKey(HKEY rootSrc, const std::wstring& srcPath, HKEY rootDst, const std::wstring& dstPath);
    bool RenameKey(HKEY root, const std::wstring& oldPath, const std::wstring& newName);

    bool CopyValue(HANDLE hKey, const std::wstring& valueName, const std::wstring& destKeyFullPath);
    bool MoveValue(HANDLE hKey, const std::wstring& valueName, const std::wstring& destKeyFullPath);
    bool RenameValue(HANDLE hKey, const std::wstring& oldName, const std::wstring& newName);

    // Search
    std::vector<SearchResult> SearchRegistry(
        HKEY root,
        const std::wstring& searchText,
        bool searchNames = true,
        bool searchValues = true,
        bool searchData = true,
        DWORD maxResults = 100
    );

    std::vector<SearchResult> SearchRegistryAtPath(
        HKEY root,
        const std::wstring& startPath,
        const std::wstring& searchText,
        bool searchNames = true,
        bool searchValues = true,
        bool searchData = true,
        DWORD maxResults = 100
    );

    // Favorites
    std::vector<std::wstring> GetFavorites();
    void AddFavorite(const std::wstring& keyPath);
    void RemoveFavorite(const std::wstring& keyPath);

    // Import / Export
    bool ExportKeyToRegFile(HKEY root, const std::wstring& keyPath, const std::wstring& filePath);
    bool ExportKeyToHiveFile(HKEY root, const std::wstring& keyPath, const std::wstring& filePath);
    bool ImportRegFile(const std::wstring& filePath);

    // Permissions
    bool GetKeySecurity(HKEY hKey, PSECURITY_DESCRIPTOR& pSD);
    bool SetKeySecurity(HKEY hKey, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR pSD);

    // Branch comparison
    bool CompareKeys(
        HKEY root1,
        const std::wstring& path1,
        HKEY root2,
        const std::wstring& path2,
        std::vector<std::wstring>& differences
    );

    // Clipboard
    void SetRegistryClipboard(
        RegistryClipboardType type,
        HKEY root,
        const std::wstring& path,
        const std::wstring& valueName = L"",
        bool cut = false
    );

    bool HasRegistryClipboard();

    bool GetRegistryClipboard(
        RegistryClipboardType& type,
        HKEY& root,
        std::wstring& path,
        std::wstring& valueName,
        bool& cut
    );

    void ClearRegistryClipboard();

    // WinRE (Recovery environment) / Offline hive
    bool IsLikelyRecoveryEnvironment();
    std::vector<std::wstring> FindWindowsInstallations();

    bool LoadHive(HKEY root, const std::wstring& mountName, const std::wstring& hiveFile);
    bool UnloadHive(HKEY root, const std::wstring& mountName);

    // Inline editing helpers
    std::wstring ValueDataToDisplay(const RegValue& value);

    bool SetValueDataFromString(
        HKEY root,
        const std::wstring& subKey,
        const std::wstring& valueName,
        const std::wstring& text
    );

    bool ValueExists(HANDLE hKey, const std::wstring& valueName);
    bool SubKeyExists(HKEY root, const std::wstring& subKey);

    bool RenameValueSafe(
        HKEY root,
        const std::wstring& subKey,
        const std::wstring& oldName,
        const std::wstring& newName
    );

    bool RenameKeySafe(
        HKEY root,
        const std::wstring& oldPath,
        const std::wstring& newName
    );
}