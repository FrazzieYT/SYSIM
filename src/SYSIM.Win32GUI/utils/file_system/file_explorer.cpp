#include "file_explorer.h"
#include <shlobj.h>
#include <shellapi.h>
#include <string>

namespace FileExplorer {

    std::vector<FileItem> GetDirectoryContents(const std::wstring& path) {
        std::vector<FileItem> items;
        std::wstring searchPath = path + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return items;
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            FileItem item;
            item.name = fd.cFileName;
            item.fullPath = path + L"\\" + fd.cFileName;
            item.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            item.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            item.modificationTime = fd.ftLastWriteTime;
            items.push_back(item);
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
        return items;
    }

    std::vector<std::wstring> GetDrives() {
        std::vector<std::wstring> drives;
        wchar_t buffer[256];
        DWORD len = GetLogicalDriveStringsW(sizeof(buffer) / sizeof(wchar_t), buffer);
        if (len == 0) return drives;
        for (const wchar_t* p = buffer; *p; p += wcslen(p) + 1) {
            drives.push_back(p);
        }
        return drives;
    }

    bool IsRootPath(const std::wstring& path) {
        if (path.length() == 3 && path[1] == L':' && path[2] == L'\\') return true;
        return false;
    }

    std::wstring GetParentPath(const std::wstring& path) {
        size_t pos = path.find_last_of(L'\\');
        if (pos == std::wstring::npos) return path;
        if (pos == 0) return path.substr(0, 1);
        return path.substr(0, pos);
    }

    bool CopyFileOrFolder(const std::wstring& source, const std::wstring& dest) {
        DWORD attrs = GetFileAttributesW(source.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) return false;
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            SHFILEOPSTRUCTW op = {};
            op.wFunc = FO_COPY;
            std::wstring src = source + L"\0";
            std::wstring dst = dest + L"\0";
            op.pFrom = src.c_str();
            op.pTo = dst.c_str();
            op.fFlags = FOF_NOCONFIRMATION | FOF_NO_UI | FOF_SILENT;
            return SHFileOperationW(&op) == 0;
        }
        else {
            return CopyFileW(source.c_str(), dest.c_str(), FALSE) != 0;
        }
    }

    bool MoveFileOrFolder(const std::wstring& source, const std::wstring& dest) {
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_MOVE;
        std::wstring src = source + L"\0";
        std::wstring dst = dest + L"\0";
        op.pFrom = src.c_str();
        op.pTo = dst.c_str();
        op.fFlags = FOF_NOCONFIRMATION | FOF_NO_UI | FOF_SILENT;
        return SHFileOperationW(&op) == 0;
    }

    bool DeleteFileOrFolder(const std::wstring& path, bool permanent) {
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_DELETE;

        std::wstring src = path + L"\0";
        op.pFrom = src.c_str();

        op.fFlags = FOF_NOCONFIRMATION | FOF_NO_UI | FOF_SILENT;

        if (!permanent) {
            op.fFlags |= FOF_ALLOWUNDO;
        }

        return SHFileOperationW(&op) == 0;
    }

    bool RenameFileOrFolder(const std::wstring& oldPath, const std::wstring& newName) {
        std::wstring parent = GetParentPath(oldPath);
        std::wstring newPath = parent + L"\\" + newName;
        return MoveFileExW(oldPath.c_str(), newPath.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }

    bool CreateFolder(const std::wstring& path) {
        return SHCreateDirectoryExW(nullptr, path.c_str(), nullptr) == ERROR_SUCCESS;
    }

    bool CreateNewFile(const std::wstring& path, const std::vector<BYTE>& data) {
        HANDLE hFile = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        if (!data.empty()) {
            WriteFile(hFile, data.data(), (DWORD)data.size(), &written, nullptr);
        }
        CloseHandle(hFile);
        return true;
    }

    bool OpenFile(const std::wstring& path, bool runAsAdmin) {
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = runAsAdmin ? L"runas" : L"open";
        sei.lpFile = path.c_str();
        sei.nShow = SW_SHOWNORMAL;
        return ShellExecuteExW(&sei) != 0;
    }

    // Clipboard
    static std::wstring g_clipboardPath;
    static bool g_clipboardCut = false;

    void SetClipboardItem(const std::wstring& path, bool cut) {
        g_clipboardPath = path;
        g_clipboardCut = cut;
    }

    bool HasClipboardItem() {
        return !g_clipboardPath.empty();
    }

    std::wstring GetClipboardItem(bool& cut) {
        cut = g_clipboardCut;
        return g_clipboardPath;
    }

    void ClearClipboard() {
        g_clipboardPath.clear();
        g_clipboardCut = false;
    }

}