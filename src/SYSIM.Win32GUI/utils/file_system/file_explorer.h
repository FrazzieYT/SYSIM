#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace FileExplorer {

    struct FileItem {
        std::wstring name;
        std::wstring fullPath;
        bool isDirectory = false;
        unsigned long long size = 0;
        FILETIME modificationTime{};

        // Sig проверка подписи
        bool sigChecked = false;
        bool sigValid = false;
        bool sigInvalid = false;
        bool sigMicrosoft = false;
        std::wstring signer;
    };
    
    std::vector<FileItem> GetDirectoryContents(const std::wstring& path);
    std::vector<std::wstring> GetDrives();
    bool IsRootPath(const std::wstring& path);
    std::wstring GetParentPath(const std::wstring& path);

    // File and folder operations
    bool CopyFileOrFolder(const std::wstring& source, const std::wstring& dest);
    bool MoveFileOrFolder(const std::wstring& source, const std::wstring& dest);
    bool DeleteFileOrFolder(const std::wstring& path, bool permanent = false);
    bool RenameFileOrFolder(const std::wstring& oldPath, const std::wstring& newName);
    bool CreateFolder(const std::wstring& path);
    bool CreateNewFile(const std::wstring& path, const std::vector<BYTE>& data = {});
    bool OpenFile(const std::wstring& path, bool runAsAdmin = false);

    // Clipboard
    void SetClipboardItem(const std::wstring& path, bool cut = false);
    bool HasClipboardItem();
    std::wstring GetClipboardItem(bool& cut);
    void ClearClipboard();
}