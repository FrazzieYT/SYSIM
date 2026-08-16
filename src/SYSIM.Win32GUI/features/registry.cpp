#include "registry.h"
#include "core/globals.h"
#include "core/app.h"
#include "ui/widgets.h"
#include "utils/registry/registry_editor.h"
#include <string>
#include <vector>
#include <algorithm>
#include <functional>
#include <commdlg.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comdlg32.lib")
using namespace Gdiplus;

// === Context menu commands ===
enum : int {
    IDM_REG_OPEN = 3001,
    IDM_REG_NEW_KEY,
    IDM_REG_NEW_VALUE_SZ,
    IDM_REG_NEW_VALUE_DWORD,
    IDM_REG_MODIFY,
    IDM_REG_COPY_KEY,
    IDM_REG_CUT_KEY,
    IDM_REG_COPY_VALUE,
    IDM_REG_CUT_VALUE,
    IDM_REG_PASTE,
    IDM_REG_DELETE_KEY,
    IDM_REG_DELETE_VALUE,
    IDM_REG_RENAME_KEY,
    IDM_REG_RENAME_VALUE,
    IDM_REG_EXPORT,
    IDM_REG_IMPORT,
    IDM_REG_SEARCH,
    IDM_REG_PERMISSIONS,
    IDM_REG_REFRESH,
    IDM_REG_GOTO,
    IDM_REG_COPY_PATH,
    IDM_REG_OFFLINE
};

// Toolbar commands
enum : int {
    TOOL_UP = 4001,
    TOOL_REFRESH,
    TOOL_OFFLINE,
    TOOL_SEARCH,
    TOOL_EXPORT,
    TOOL_IMPORT,
    TOOL_NEW_KEY,
    TOOL_NEW_VALUE
};

// === Offline mount names ===
static const wchar_t* OFFLINE_SOFTWARE_MOUNT = L"SYSIM_OFFLINE_SOFTWARE";
static const wchar_t* OFFLINE_SYSTEM_MOUNT = L"SYSIM_OFFLINE_SYSTEM";
static const wchar_t* OFFLINE_USER_PREFIX = L"SYSIM_OFFLINE_USER_";

// === Tab state ===
enum class RootAction {
    Normal,
    OfflineHive,
    MountWindows,
    UnmountWindows
};

struct RootItem {
    std::wstring display;
    RootAction action = RootAction::Normal;
    HKEY root = nullptr;
    std::wstring path;
    bool offline = false;
    std::wstring windowsPath;
};

struct OfflineUserMount {
    std::wstring userName;
    std::wstring mountName;
};

static HKEY g_regRoot = nullptr;
static std::wstring g_regPath;
static std::wstring g_rootBasePath;
static std::wstring g_rootDisplayName;
static bool g_currentRootOffline = false;
static std::vector<std::wstring> g_subKeys;
static std::vector<RegistryEditor::RegValue> g_values;
static int g_selectedSubKey = -1;
static int g_selectedValue = -1;
static bool g_searchMode = false;
static std::vector<RegistryEditor::SearchResult> g_searchResults;
static int g_selectedSearch = -1;
static HKEY g_searchRoot = nullptr;
static std::wstring g_searchRootBasePath;
static std::wstring g_searchRootDisplayName;
static bool g_searchRootOffline = false;
static std::vector<RootItem> g_rootItems;
static bool g_offlineMounted = false;
static std::wstring g_offlineWindowsPath;
static bool g_offlineSoftwareMounted = false;
static bool g_offlineSystemMounted = false;
static std::vector<OfflineUserMount> g_offlineUsers;
static std::vector<std::wstring> g_foundWindows;
static bool g_foundWindowsScanned = false;
static bool g_recoveryChecked = false;
static bool g_isRecovery = false;

// == Toolbar scrolling ==
struct ToolbarButton {
    int id = 0;
    RectF rect;
};
static std::vector<ToolbarButton> g_toolbarButtons;
static RectF g_toolbarLeftArrow;
static RectF g_toolbarRightArrow;
static bool g_toolbarArrowsVisible = false;
static float g_toolbarOffset = 0.0f;

// === Inline editing ===
enum class RegistryEditTarget {
    None,
    KeyName,
    ValueName,
    ValueData
};

struct RegistryEditState {
    bool active = false;
    RegistryEditTarget target = RegistryEditTarget::None;
    int index = -1;
    std::wstring text;
};
static RegistryEditState g_registryEdit;

// User panel & column resizing
static float g_registryLeftPaneRatio = 0.38f;
static float g_registryValueNameRatio = 0.35f;
static bool g_registryResizingSplit = false;
static bool g_registryResizingValueName = false;

static float RegClampF(float value, float minValue, float maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

// Base helpers
static void RegInvalidate() {
    InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
}

static bool HitRect(const RectF& rect, float x, float y) {
    return
        x >= rect.X &&
        x <= rect.X + rect.Width &&
        y >= rect.Y &&
        y <= rect.Y + rect.Height;
}

static std::wstring CombineRegPath(
    const std::wstring& a,
    const std::wstring& b
) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + L"\\" + b;
}

static std::wstring GetLastPathSegment(const std::wstring& path) {
    if (path.empty()) {
        return L"";
    }
    size_t pos = path.find_last_of(L'\\');
    if (pos == std::wstring::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

static std::wstring GetParentPath(const std::wstring& path) {
    if (path.empty()) {
        return L"";
    }
    size_t pos = path.find_last_of(L'\\');
    if (pos == std::wstring::npos) {
        return L"";
    }
    return path.substr(0, pos);
}

static std::wstring RootName(HKEY root) {
    if (root == HKEY_CLASSES_ROOT) return L"HKEY_CLASSES_ROOT";
    if (root == HKEY_CURRENT_USER) return L"HKEY_CURRENT_USER";
    if (root == HKEY_LOCAL_MACHINE) return L"HKEY_LOCAL_MACHINE";
    if (root == HKEY_USERS) return L"HKEY_USERS";
    if (root == HKEY_CURRENT_CONFIG) return L"HKEY_CURRENT_CONFIG";
    return L"UNKNOWN_ROOT";
}

static void CopyTextToClipboard(const std::wstring& text) {
    if (text.empty()) {
        return;
    }
    if (!OpenClipboard(nullptr)) {
        return;
    }
    EmptyClipboard();
    HGLOBAL hGlobal = GlobalAlloc(
        GMEM_MOVEABLE,
        (text.size() + 1) * sizeof(wchar_t)
    );
    if (hGlobal) {
        wchar_t* buffer = static_cast<wchar_t*>(GlobalLock(hGlobal));
        if (buffer) {
            wcscpy_s(buffer, text.size() + 1, text.c_str());
            GlobalUnlock(hGlobal);
            SetClipboardData(CF_UNICODETEXT, hGlobal);
        }
    }
    CloseClipboard();
}

// === Local offline functions ===
static bool Local_EnablePrivilege(const wchar_t* privilegeName) {
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

static bool Local_EnableBackupRestorePrivileges() {
    bool backup = Local_EnablePrivilege(L"SeBackupPrivilege");
    bool restore = Local_EnablePrivilege(L"SeRestorePrivilege");
    return backup || restore;
}

static bool Local_IsLikelyRecoveryEnvironment() {
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

static bool IsRecoveryCached() {
    if (!g_recoveryChecked) {
        g_isRecovery = Local_IsLikelyRecoveryEnvironment();
        g_recoveryChecked = true;
    }
    return g_isRecovery;
}

static std::vector<std::wstring> Local_FindWindowsInstallations() {
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

static bool Local_LoadHive(
    HKEY root,
    const std::wstring& mountName,
    const std::wstring& hiveFile
) {
    Local_EnableBackupRestorePrivileges();
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

static bool Local_UnloadHive(
    HKEY root,
    const std::wstring& mountName
) {
    Local_EnableBackupRestorePrivileges();
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

// === Local search ===
static std::vector<RegistryEditor::SearchResult> Local_SearchRegistryAtPath(
    HKEY root,
    const std::wstring& startPath,
    const std::wstring& searchText,
    bool searchKeyNames,
    bool searchValueNames,
    bool searchData,
    DWORD maxResults
) {
    std::vector<RegistryEditor::SearchResult> results;
    if (searchText.empty()) {
        return results;
    }
    HKEY hStart = root;
    bool closeStart = false;
    if (!startPath.empty()) {
        hStart = reinterpret_cast<HKEY>(
            RegistryEditor::OpenKey(root, startPath, KEY_READ)
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
        if (searchKeyNames && !currentPath.empty()) {
            size_t pos = currentPath.find_last_of(L'\\');
            std::wstring keyName =
                (pos != std::wstring::npos)
                ? currentPath.substr(pos + 1)
                : currentPath;
            if (keyName.find(searchText) != std::wstring::npos) {
                RegistryEditor::SearchResult sr;
                sr.keyPath = currentPath;
                sr.valueName.clear();
                results.push_back(sr);
            }
        }
        auto values = RegistryEditor::EnumValues(static_cast<HANDLE>(hKey));
        for (const auto& val : values) {
            if (results.size() >= maxResults) {
                break;
            }
            if (searchValueNames && val.name.find(searchText) != std::wstring::npos) {
                RegistryEditor::SearchResult sr;
                sr.keyPath = currentPath;
                sr.valueName = val.name;
                sr.type = val.type;
                if (val.type == RegistryEditor::RegValueType::String ||
                    val.type == RegistryEditor::RegValueType::ExpandString) {
                    if (!val.data.empty()) {
                        sr.data.assign(
                            reinterpret_cast<const wchar_t*>(val.data.data()),
                            val.data.size() / sizeof(wchar_t)
                        );
                    }
                }
                results.push_back(sr);
            }
            else if (searchData) {
                if (val.type == RegistryEditor::RegValueType::String ||
                    val.type == RegistryEditor::RegValueType::ExpandString) {
                    std::wstring str;
                    if (!val.data.empty()) {
                        str.assign(
                            reinterpret_cast<const wchar_t*>(val.data.data()),
                            val.data.size() / sizeof(wchar_t)
                        );
                    }
                    if (str.find(searchText) != std::wstring::npos) {
                        RegistryEditor::SearchResult sr;
                        sr.keyPath = currentPath;
                        sr.valueName = val.name;
                        sr.type = val.type;
                        sr.data = str;
                        results.push_back(sr);
                    }
                }
            }
        }
        auto subKeys = RegistryEditor::EnumSubKeys(static_cast<HANDLE>(hKey));
        for (const auto& sub : subKeys) {
            if (results.size() >= maxResults) {
                break;
            }
            HKEY hSub = reinterpret_cast<HKEY>(
                RegistryEditor::OpenKey(hKey, sub, KEY_READ)
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
                RegistryEditor::CloseKey(static_cast<HANDLE>(hSub));
            }
        }
        };
    searchRecursive(hStart, startPath);
    if (closeStart) {
        RegistryEditor::CloseKey(static_cast<HANDLE>(hStart));
    }
    return results;
}

// Display current path
static std::wstring GetCurrentPathDisplay() {
    if (g_searchMode && !g_regRoot && g_searchRoot) {
        if (!g_searchRootDisplayName.empty()) {
            return g_searchRootDisplayName;
        }
        return RootName(g_searchRoot);
    }
    if (!g_regRoot) {
        return L"Выберите корень реестра";
    }
    std::wstring base = g_rootDisplayName;
    if (base.empty()) {
        base = RootName(g_regRoot);
    }
    std::wstring rel = g_regPath;
    if (!g_rootBasePath.empty()) {
        if (g_regPath == g_rootBasePath) {
            rel = L"";
        }
        else {
            std::wstring prefix = g_rootBasePath + L"\\";
            if (g_regPath.compare(
                0,
                prefix.size(),
                prefix
            ) == 0) {
                rel = g_regPath.substr(prefix.size());
            }
        }
    }
    if (rel.empty()) {
        return base;
    }
    return base + L"\\" + rel;
}

// Simple input box
static std::wstring g_inputBoxResult;
static bool g_inputBoxOk = false;

static LRESULT CALLBACK RegistryInputBoxWndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == 1) {
            HWND hEdit = GetDlgItem(hwnd, 1001);
            int len = GetWindowTextLengthW(hEdit);
            std::vector<wchar_t> buffer(len + 1, 0);
            GetWindowTextW(hEdit, buffer.data(), len + 1);
            g_inputBoxResult = buffer.data();
            g_inputBoxOk = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == 2) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE: {
        DestroyWindow(hwnd);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool ShowRegistryInputBox(
    HWND owner,
    const std::wstring& title,
    const std::wstring& prompt,
    std::wstring& text
) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = RegistryInputBoxWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"SYSIM_RegistryInputBoxW";
        ATOM atom = RegisterClassExW(&wc);
        if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        classRegistered = true;
    }
    g_inputBoxResult = text;
    g_inputBoxOk = false;
    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"SYSIM_RegistryInputBoxW",
        title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        460,
        170,
        owner,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );
    if (!hwnd) {
        return false;
    }
    if (owner) {
        RECT rcOwner{};
        RECT rcWnd{};
        GetWindowRect(owner, &rcOwner);
        GetWindowRect(hwnd, &rcWnd);
        int width = rcWnd.right - rcWnd.left;
        int height = rcWnd.bottom - rcWnd.top;
        int x = rcOwner.left + ((rcOwner.right - rcOwner.left) - width) / 2;
        int y = rcOwner.top + ((rcOwner.bottom - rcOwner.top) - height) / 2;
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    HFONT hFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HWND hPrompt = CreateWindowExW(
        0,
        L"STATIC",
        prompt.c_str(),
        WS_CHILD | WS_VISIBLE,
        12,
        12,
        420,
        20,
        hwnd,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );
    HWND hEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        text.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        12,
        40,
        420,
        26,
        hwnd,
        reinterpret_cast<HMENU>(1001),
        GetModuleHandleW(nullptr),
        nullptr
    );
    HWND hOk = CreateWindowExW(
        0,
        L"BUTTON",
        L"OK",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        250,
        80,
        85,
        30,
        hwnd,
        reinterpret_cast<HMENU>(1),
        GetModuleHandleW(nullptr),
        nullptr
    );
    HWND hCancel = CreateWindowExW(
        0,
        L"BUTTON",
        L"Отмена",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        345,
        80,
        85,
        30,
        hwnd,
        reinterpret_cast<HMENU>(2),
        GetModuleHandleW(nullptr),
        nullptr
    );
    SendMessageW(hPrompt, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
    SendMessageW(hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
    SendMessageW(hOk, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
    SendMessageW(hCancel, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
    if (owner) {
        EnableWindow(owner, FALSE);
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetFocus(hEdit);
    MSG msg{};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (g_inputBoxOk) {
        text = g_inputBoxResult;
    }
    return g_inputBoxOk;
}

// Root items
static void RefreshRootItems() {
    g_rootItems.clear();
    bool recovery = IsRecoveryCached();
    auto AddNormalRoot = [&](const std::wstring& display, HKEY root) {
        RootItem item;
        item.display = display;
        item.action = RootAction::Normal;
        item.root = root;
        item.path = L"";
        item.offline = false;
        g_rootItems.push_back(item);
        };
    if (recovery) {
        if (!g_offlineMounted) {
            if (!g_foundWindowsScanned) {
                g_foundWindows = Local_FindWindowsInstallations();
                g_foundWindowsScanned = true;
            }
            if (g_foundWindows.empty()) {
                RootItem item;
                item.display =
                    L"Офлайн Windows не найдена. Нажми \"Офлайн\", чтобы ввести путь вручную.";
                item.action = RootAction::MountWindows;
                item.offline = true;
                g_rootItems.push_back(item);
            }
            else {
                for (const std::wstring& windowsPath : g_foundWindows) {
                    RootItem item;
                    item.display = L"Монтировать офлайн: " + windowsPath;
                    item.action = RootAction::MountWindows;
                    item.offline = true;
                    item.windowsPath = windowsPath;
                    g_rootItems.push_back(item);
                }
            }
            RootItem sep;
            sep.display = L"--- Текущая среда восстановления ---";
            sep.action = RootAction::Normal;
            sep.root = nullptr;
            g_rootItems.push_back(sep);
            AddNormalRoot(L"Текущая среда: HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE);
            AddNormalRoot(L"Текущая среда: HKEY_CURRENT_USER", HKEY_CURRENT_USER);
            AddNormalRoot(L"Текущая среда: HKEY_CLASSES_ROOT", HKEY_CLASSES_ROOT);
            AddNormalRoot(L"Текущая среда: HKEY_USERS", HKEY_USERS);
            AddNormalRoot(L"Текущая среда: HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG);
        }
        else {
            if (g_offlineSoftwareMounted) {
                RootItem item;
                item.display = L"Офлайн: HKLM\\SOFTWARE";
                item.action = RootAction::OfflineHive;
                item.root = HKEY_LOCAL_MACHINE;
                item.path = OFFLINE_SOFTWARE_MOUNT;
                item.offline = true;
                g_rootItems.push_back(item);
            }
            if (g_offlineSystemMounted) {
                RootItem item;
                item.display = L"Офлайн: HKLM\\SYSTEM";
                item.action = RootAction::OfflineHive;
                item.root = HKEY_LOCAL_MACHINE;
                item.path = OFFLINE_SYSTEM_MOUNT;
                item.offline = true;
                g_rootItems.push_back(item);
            }
            for (const OfflineUserMount& user : g_offlineUsers) {
                RootItem item;
                item.display = L"Офлайн пользователь: " + user.userName;
                item.action = RootAction::OfflineHive;
                item.root = HKEY_USERS;
                item.path = user.mountName;
                item.offline = true;
                g_rootItems.push_back(item);
            }
            RootItem unmount;
            unmount.display = L"Размонтировать офлайн-кусты";
            unmount.action = RootAction::UnmountWindows;
            unmount.offline = true;
            g_rootItems.push_back(unmount);
            RootItem sep;
            sep.display = L"--- Текущая среда восстановления ---";
            sep.action = RootAction::Normal;
            sep.root = nullptr;
            g_rootItems.push_back(sep);
            AddNormalRoot(L"Текущая среда: HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE);
            AddNormalRoot(L"Текущая среда: HKEY_CURRENT_USER", HKEY_CURRENT_USER);
            AddNormalRoot(L"Текущая среда: HKEY_USERS", HKEY_USERS);
        }
    }
    else {
        AddNormalRoot(L"HKEY_CLASSES_ROOT", HKEY_CLASSES_ROOT);
        AddNormalRoot(L"HKEY_CURRENT_USER", HKEY_CURRENT_USER);
        AddNormalRoot(L"HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE);
        AddNormalRoot(L"HKEY_USERS", HKEY_USERS);
        AddNormalRoot(L"HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG);
    }
}

// Update data
static void RefreshRegistryView() {
    g_subKeys.clear();
    g_values.clear();
    g_selectedSubKey = -1;
    g_selectedValue = -1;
    if (!g_regRoot) {
        return;
    }
    HANDLE hKey = RegistryEditor::OpenKey(
        g_regRoot,
        g_regPath,
        KEY_READ
    );
    if (!hKey) {
        return;
    }
    g_subKeys = RegistryEditor::EnumSubKeys(hKey);
    g_values = RegistryEditor::EnumValues(hKey);
    RegistryEditor::CloseKey(hKey);
    std::sort(
        g_subKeys.begin(),
        g_subKeys.end(),
        [](const std::wstring& a, const std::wstring& b) {
            return lstrcmpiW(a.c_str(), b.c_str()) < 0;
        }
    );
}

static void RefreshRegistryUI() {
    RefreshRegistryView();
    RegInvalidate();
}

// Offline mount / unmount
static void UnmountOfflineWindows(bool showErrors) {
    if (g_currentRootOffline ||
        (!g_rootBasePath.empty() &&
            g_rootBasePath.find(L"SYSIM_OFFLINE_") == 0)) {
        g_regRoot = nullptr;
        g_regPath.clear();
        g_rootBasePath.clear();
        g_rootDisplayName.clear();
        g_currentRootOffline = false;
    }
    bool any = false;
    bool allOk = true;
    if (g_offlineSoftwareMounted) {
        any = true;
        if (!Local_UnloadHive(
            HKEY_LOCAL_MACHINE,
            OFFLINE_SOFTWARE_MOUNT
        )) {
            allOk = false;
        }
    }
    if (g_offlineSystemMounted) {
        any = true;
        if (!Local_UnloadHive(
            HKEY_LOCAL_MACHINE,
            OFFLINE_SYSTEM_MOUNT
        )) {
            allOk = false;
        }
    }
    for (const OfflineUserMount& user : g_offlineUsers) {
        any = true;
        if (!Local_UnloadHive(
            HKEY_USERS,
            user.mountName
        )) {
            allOk = false;
        }
    }
    g_offlineMounted = false;
    g_offlineSoftwareMounted = false;
    g_offlineSystemMounted = false;
    g_offlineUsers.clear();
    g_offlineWindowsPath.clear();
    RegistryEditor::ClearRegistryClipboard();
    RefreshRootItems();
    RegInvalidate();
    if (showErrors && any && !allOk) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Некоторые офлайн-кусты не удалось размонтировать.\r\n"
            L"Возможно, они всё ещё используются.",
            L"Реестр",
            MB_OK | MB_ICONWARNING
        );
    }
}

static bool IsDirectory(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool IsFile(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static std::wstring NormalizeWindowsRoot(std::wstring path) {
    while (!path.empty() &&
        (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    if (path.size() == 2 && path[1] == L':') {
        std::wstring candidate = path + L"\\Windows";
        if (IsDirectory(candidate)) {
            return candidate;
        }
    }
    return path;
}

static bool MountOfflineWindows(const std::wstring& windowsPathRaw) {
    HWND hwnd = App::Instance()->GetHWND();
    std::wstring windowsPath = NormalizeWindowsRoot(windowsPathRaw);
    if (windowsPath.empty()) {
        return false;
    }
    if (!IsDirectory(windowsPath)) {
        MessageBoxW(
            hwnd,
            L"Указанный путь не найден или не является папкой.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
        return false;
    }
    std::wstring softwareHive =
        windowsPath + L"\\System32\\config\\SOFTWARE";
    std::wstring systemHive =
        windowsPath + L"\\System32\\config\\SYSTEM";
    if (!IsFile(softwareHive) && !IsFile(systemHive)) {
        MessageBoxW(
            hwnd,
            L"В указанной папке Windows не найдены кусты реестра.\r\n"
            L"Ожидаются файлы:\r\n"
            L"Windows\\System32\\config\\SOFTWARE\r\n"
            L"Windows\\System32\\config\\SYSTEM",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
        return false;
    }
    if (g_offlineMounted) {
        UnmountOfflineWindows(false);
    }
    g_offlineSoftwareMounted = Local_LoadHive(
        HKEY_LOCAL_MACHINE,
        OFFLINE_SOFTWARE_MOUNT,
        softwareHive
    );
    g_offlineSystemMounted = Local_LoadHive(
        HKEY_LOCAL_MACHINE,
        OFFLINE_SYSTEM_MOUNT,
        systemHive
    );
    g_offlineUsers.clear();
    std::wstring usersRoot = windowsPath + L"\\Users";
    if (IsDirectory(usersRoot)) {
        WIN32_FIND_DATAW fd{};
        std::wstring searchPath = usersRoot + L"\\*";
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            int userIndex = 0;
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    continue;
                }
                std::wstring userName = fd.cFileName;
                if (userName == L"." ||
                    userName == L".." ||
                    userName == L"Public" ||
                    userName == L"Default" ||
                    userName == L"Default User" ||
                    userName == L"All Users") {
                    continue;
                }
                std::wstring ntuserPath =
                    usersRoot + L"\\" + userName + L"\\NTUSER.DAT";
                if (!IsFile(ntuserPath)) {
                    continue;
                }
                std::wstring mountName =
                    OFFLINE_USER_PREFIX +
                    std::to_wstring(userIndex++);
                if (Local_LoadHive(
                    HKEY_USERS,
                    mountName,
                    ntuserPath
                )) {
                    OfflineUserMount user;
                    user.userName = userName;
                    user.mountName = mountName;
                    g_offlineUsers.push_back(user);
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }
    g_offlineMounted =
        g_offlineSoftwareMounted ||
        g_offlineSystemMounted ||
        !g_offlineUsers.empty();
    if (!g_offlineMounted) {
        MessageBoxW(
            hwnd,
            L"Не удалось примонтировать офлайн-кусты реестра.\r\n"
            L"Проверь права и доступ к файлам реестра.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
        return false;
    }
    g_offlineWindowsPath = windowsPath;
    RefreshRootItems();
    RegInvalidate();
    return true;
}

static void OfflineButtonClicked() {
    HWND hwnd = App::Instance()->GetHWND();
    if (g_offlineMounted) {
        int answer = MessageBoxW(
            hwnd,
            L"Размонтировать офлайн-кусты реестра?",
            L"Офлайн-режим",
            MB_YESNO | MB_ICONQUESTION
        );
        if (answer == IDYES) {
            UnmountOfflineWindows(true);
        }
        return;
    }
    std::wstring path;
    if (ShowRegistryInputBox(
        hwnd,
        L"Офлайн-режим",
        L"Путь к папке Windows (например D:\\Windows):",
        path
    )) {
        MountOfflineWindows(path);
    }
}

// Navigation
static void GoToRootItem(const RootItem& item) {
    if (item.root == nullptr) {
        if (item.action == RootAction::MountWindows) {
            if (item.windowsPath.empty()) {
                OfflineButtonClicked();
            }
            else {
                MountOfflineWindows(item.windowsPath);
            }
        }
        else if (item.action == RootAction::UnmountWindows) {
            int answer = MessageBoxW(
                App::Instance()->GetHWND(),
                L"Размонтировать офлайн-кусты реестра?",
                L"Офлайн-режим",
                MB_YESNO | MB_ICONQUESTION
            );
            if (answer == IDYES) {
                UnmountOfflineWindows(true);
            }
        }
        return;
    }
    g_regRoot = item.root;
    g_regPath = item.path;
    g_rootBasePath = item.path;
    g_rootDisplayName = item.display;
    g_currentRootOffline = item.offline;
    g_searchMode = false;
    g_searchResults.clear();
    g_selectedSearch = -1;
    RefreshRegistryUI();
}

static void GoUp() {
    if (!g_regRoot) {
        return;
    }
    if (g_searchMode) {
        g_searchMode = false;
        g_searchResults.clear();
        g_selectedSearch = -1;
        RefreshRegistryUI();
        return;
    }
    if (g_regPath.empty() || g_regPath == g_rootBasePath) {
        g_regRoot = nullptr;
        g_regPath.clear();
        g_rootBasePath.clear();
        g_rootDisplayName.clear();
        g_currentRootOffline = false;
        RefreshRootItems();
        RegInvalidate();
        return;
    }
    g_regPath = GetParentPath(g_regPath);
    RefreshRegistryUI();
}

static void OpenSubKey(int index) {
    if (!g_regRoot) {
        return;
    }
    if (index < 0 || index >= static_cast<int>(g_subKeys.size())) {
        return;
    }
    g_regPath = CombineRegPath(g_regPath, g_subKeys[index]);
    RefreshRegistryUI();
}

static void OpenSearchResult(int index) {
    if (index < 0 || index >= static_cast<int>(g_searchResults.size())) {
        return;
    }
    const RegistryEditor::SearchResult& result = g_searchResults[index];
    g_searchMode = false;
    g_regRoot = g_searchRoot;
    g_rootBasePath = g_searchRootBasePath;
    g_rootDisplayName = g_searchRootDisplayName;
    g_currentRootOffline = g_searchRootOffline;
    g_regPath = result.keyPath;
    RefreshRegistryView();
    if (!result.valueName.empty()) {
        for (int i = 0; i < static_cast<int>(g_values.size()); ++i) {
            if (g_values[i].name == result.valueName) {
                g_selectedValue = i;
                break;
            }
        }
    }
    RegInvalidate();
}

// Inline editing
static void BeginEditKeyName(int index) {
    if (index < 0 || index >= static_cast<int>(g_subKeys.size())) {
        return;
    }
    g_registryEdit.active = true;
    g_registryEdit.target = RegistryEditTarget::KeyName;
    g_registryEdit.index = index;
    g_registryEdit.text = g_subKeys[index];
    RegInvalidate();
}

static void BeginEditValueName(int index) {
    if (index < 0 || index >= static_cast<int>(g_values.size())) {
        return;
    }
    g_registryEdit.active = true;
    g_registryEdit.target = RegistryEditTarget::ValueName;
    g_registryEdit.index = index;
    g_registryEdit.text = g_values[index].name;
    RegInvalidate();
}

static void BeginEditValueData(int index) {
    if (index < 0 || index >= static_cast<int>(g_values.size())) {
        return;
    }
    g_registryEdit.active = true;
    g_registryEdit.target = RegistryEditTarget::ValueData;
    g_registryEdit.index = index;
    g_registryEdit.text =
        RegistryEditor::ValueDataToDisplay(g_values[index]);
    RegInvalidate();
}

static void CancelRegistryEdit() {
    g_registryEdit.active = false;
    g_registryEdit.target = RegistryEditTarget::None;
    g_registryEdit.index = -1;
    g_registryEdit.text.clear();
    RegInvalidate();
}

static void CommitRegistryEdit() {
    if (!g_registryEdit.active) {
        return;
    }
    RegistryEditTarget target = g_registryEdit.target;
    int index = g_registryEdit.index;
    std::wstring text = g_registryEdit.text;
    g_registryEdit.active = false;
    g_registryEdit.target = RegistryEditTarget::None;
    g_registryEdit.index = -1;
    bool ok = false;
    if (!g_regRoot) {
        RegInvalidate();
        return;
    }
    if (target == RegistryEditTarget::KeyName) {
        if (index >= 0 && index < static_cast<int>(g_subKeys.size())) {
            std::wstring oldPath =
                CombineRegPath(g_regPath, g_subKeys[index]);
            ok = RegistryEditor::RenameKeySafe(
                g_regRoot,
                oldPath,
                text
            );
        }
    }
    else if (target == RegistryEditTarget::ValueName) {
        if (index >= 0 && index < static_cast<int>(g_values.size())) {
            ok = RegistryEditor::RenameValueSafe(
                g_regRoot,
                g_regPath,
                g_values[index].name,
                text
            );
        }
    }
    else if (target == RegistryEditTarget::ValueData) {
        if (index >= 0 && index < static_cast<int>(g_values.size())) {
            ok = RegistryEditor::SetValueDataFromString(
                g_regRoot,
                g_regPath,
                g_values[index].name,
                text
            );
        }
    }
    if (!ok) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Не удалось применить изменение.",
            L"Реестр",
            MB_OK | MB_ICONERROR
        );
    }
    RefreshRegistryUI();
}

// Registry data conversion
static std::wstring TrimNull(const std::wstring& s) {
    size_t pos = s.find(L'\0');
    if (pos != std::wstring::npos) {
        return s.substr(0, pos);
    }
    return s;
}

static std::wstring RegValueDataToDisplay(const RegistryEditor::RegValue& value) {
    if (value.data.empty()) {
        return L"";
    }
    switch (value.type) {
    case RegistryEditor::RegValueType::String:
    case RegistryEditor::RegValueType::ExpandString: {
        std::wstring s(
            reinterpret_cast<const wchar_t*>(value.data.data()),
            value.data.size() / sizeof(wchar_t)
        );
        return TrimNull(s);
    }
    case RegistryEditor::RegValueType::MultiString: {
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
    case RegistryEditor::RegValueType::DWord: {
        if (value.data.size() >= sizeof(DWORD)) {
            DWORD v = 0;
            memcpy(&v, value.data.data(), sizeof(DWORD));
            return std::to_wstring(v);
        }
        return L"";
    }
    case RegistryEditor::RegValueType::QWord: {
        if (value.data.size() >= sizeof(ULONGLONG)) {
            ULONGLONG v = 0;
            memcpy(&v, value.data.data(), sizeof(ULONGLONG));
            return std::to_wstring(v);
        }
        return L"";
    }
    case RegistryEditor::RegValueType::Binary: {
        std::wstring result;
        size_t count = (std::min)(value.data.size(), static_cast<size_t>(16));
        for (size_t i = 0; i < count; ++i) {
            wchar_t buffer[8]{};
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

static std::vector<BYTE> MakeStringData(const std::wstring& text) {
    std::vector<BYTE> data((text.size() + 1) * sizeof(wchar_t));
    memcpy(
        data.data(),
        text.c_str(),
        (text.size() + 1) * sizeof(wchar_t)
    );
    return data;
}

static std::vector<BYTE> MakeDwordData(DWORD value) {
    std::vector<BYTE> data(sizeof(DWORD));
    memcpy(data.data(), &value, sizeof(DWORD));
    return data;
}

// Value operations
static void ModifySelectedValue() {
    if (!g_regRoot) {
        return;
    }
    if (g_selectedValue < 0 ||
        g_selectedValue >= static_cast<int>(g_values.size())) {
        return;
    }
    RegistryEditor::RegValue value = g_values[g_selectedValue];
    HWND hwnd = App::Instance()->GetHWND();
    if (value.type == RegistryEditor::RegValueType::String ||
        value.type == RegistryEditor::RegValueType::ExpandString) {
        std::wstring text = RegValueDataToDisplay(value);
        if (!ShowRegistryInputBox(
            hwnd,
            L"Изменение строкового параметра",
            value.name.empty() ? L"(По умолчанию)" : value.name,
            text
        )) {
            return;
        }
        value.data = MakeStringData(text);
    }
    else if (value.type == RegistryEditor::RegValueType::DWord) {
        std::wstring text = RegValueDataToDisplay(value);
        if (!ShowRegistryInputBox(
            hwnd,
            L"Изменение DWORD параметра",
            value.name.empty() ? L"(По умолчанию)" : value.name,
            text
        )) {
            return;
        }
        DWORD number = 0;
        if (!text.empty()) {
            number = static_cast<DWORD>(
                wcstoul(text.c_str(), nullptr, 0)
                );
        }
        value.data = MakeDwordData(number);
    }
    else {
        MessageBoxW(
            hwnd,
            L"Редактирование этого типа параметров пока не поддерживается.",
            L"Реестр",
            MB_OK | MB_ICONINFORMATION
        );
        return;
    }
    HANDLE hKey = RegistryEditor::OpenKey(
        g_regRoot,
        g_regPath,
        KEY_SET_VALUE
    );
    if (!hKey) {
        MessageBoxW(
            hwnd,
            L"Не удалось открыть ключ для записи.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
        return;
    }
    bool ok = RegistryEditor::WriteValue(hKey, value);
    RegistryEditor::CloseKey(hKey);
    if (!ok) {
        MessageBoxW(
            hwnd,
            L"Не удалось изменить параметр.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
    }
    RefreshRegistryUI();
}

static void DeleteSelectedValue() {
    if (!g_regRoot) {
        return;
    }
    if (g_selectedValue < 0 ||
        g_selectedValue >= static_cast<int>(g_values.size())) {
        return;
    }
    const RegistryEditor::RegValue& value = g_values[g_selectedValue];
    HWND hwnd = App::Instance()->GetHWND();
    std::wstring msg =
        L"Удалить параметр?\r\n\r\n" +
        (value.name.empty() ? std::wstring(L"(По умолчанию)") : value.name);
    if (MessageBoxW(
        hwnd,
        msg.c_str(),
        L"Удаление параметра",
        MB_YESNO | MB_ICONWARNING
    ) != IDYES) {
        return;
    }
    HANDLE hKey = RegistryEditor::OpenKey(
        g_regRoot,
        g_regPath,
        KEY_SET_VALUE
    );
    if (!hKey) {
        MessageBoxW(
            hwnd,
            L"Не удалось открыть ключ для записи.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
        return;
    }
    bool ok = RegistryEditor::DeleteValue(hKey, value.name);
    RegistryEditor::CloseKey(hKey);
    if (!ok) {
        MessageBoxW(
            hwnd,
            L"Не удалось удалить параметр.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
    }
    RefreshRegistryUI();
}

static void RenameSelectedValue() {
    if (!g_regRoot) {
        return;
    }
    if (g_selectedValue < 0 ||
        g_selectedValue >= static_cast<int>(g_values.size())) {
        return;
    }
    RegistryEditor::RegValue value = g_values[g_selectedValue];
    std::wstring newName = value.name;
    if (!ShowRegistryInputBox(
        App::Instance()->GetHWND(),
        L"Переименование параметра",
        L"Новое имя параметра:",
        newName
    )) {
        return;
    }
    if (newName.empty() || newName == value.name) {
        return;
    }
    HANDLE hKey = RegistryEditor::OpenKey(
        g_regRoot,
        g_regPath,
        KEY_READ | KEY_SET_VALUE
    );
    if (!hKey) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Не удалось открыть ключ.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
        return;
    }
    bool ok = RegistryEditor::RenameValue(hKey, value.name, newName);
    RegistryEditor::CloseKey(hKey);
    if (!ok) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Не удалось переименовать параметр.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
    }
    RefreshRegistryUI();
}

// Key operations
static void CreateNewKey() {
    if (!g_regRoot) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Сначала выберите корень реестра.",
            L"Реестр",
            MB_OK | MB_ICONINFORMATION
        );
        return;
    }
    std::wstring name = L"NewKey";
    if (!ShowRegistryInputBox(
        App::Instance()->GetHWND(),
        L"Создание раздела",
        L"Имя нового раздела:",
        name
    )) {
        return;
    }
    if (name.empty()) {
        return;
    }
    if (name.find_first_of(L"\\/") != std::wstring::npos) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Имя раздела не должно содержать '\\' или '/'.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
        return;
    }
    std::wstring fullPath = CombineRegPath(g_regPath, name);
    HANDLE hKey = nullptr;
    if (!RegistryEditor::CreateKey(g_regRoot, fullPath, hKey)) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Не удалось создать раздел.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
        return;
    }
    RegistryEditor::CloseKey(hKey);
    RefreshRegistryUI();
}

static void CreateNewValue(DWORD type) {
    if (!g_regRoot) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Сначала выберите корень реестра.",
            L"Реестр",
            MB_OK | MB_ICONINFORMATION
        );
        return;
    }
    std::wstring name = L"NewValue";
    if (!ShowRegistryInputBox(
        App::Instance()->GetHWND(),
        L"Создание параметра",
        L"Имя нового параметра:",
        name
    )) {
        return;
    }
    RegistryEditor::RegValue value;
    value.name = name;
    if (type == REG_SZ) {
        value.type = RegistryEditor::RegValueType::String;
        value.data = MakeStringData(L"");
    }
    else if (type == REG_DWORD) {
        value.type = RegistryEditor::RegValueType::DWord;
        value.data = MakeDwordData(0);
    }
    else {
        return;
    }
    HANDLE hKey = RegistryEditor::OpenKey(
        g_regRoot,
        g_regPath,
        KEY_SET_VALUE
    );
    if (!hKey) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Не удалось открыть ключ для записи.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
        return;
    }
    bool ok = RegistryEditor::WriteValue(hKey, value);
    RegistryEditor::CloseKey(hKey);
    if (!ok) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Не удалось создать параметр.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
    }
    RefreshRegistryUI();
}

static void DeleteSelectedKey() {
    if (!g_regRoot) {
        return;
    }
    if (g_selectedSubKey < 0 ||
        g_selectedSubKey >= static_cast<int>(g_subKeys.size())) {
        return;
    }
    std::wstring fullPath =
        CombineRegPath(g_regPath, g_subKeys[g_selectedSubKey]);
    HWND hwnd = App::Instance()->GetHWND();
    std::wstring msg =
        L"Удалить раздел реестра вместе со всем содержимым?\r\n\r\n" +
        fullPath;
    if (MessageBoxW(
        hwnd,
        msg.c_str(),
        L"Удаление раздела",
        MB_YESNO | MB_ICONWARNING
    ) != IDYES) {
        return;
    }
    if (!RegistryEditor::DeleteKeyRecursive(g_regRoot, fullPath)) {
        MessageBoxW(
            hwnd,
            L"Не удалось удалить раздел.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
    }
    RefreshRegistryUI();
}

static void RenameSelectedKey() {
    if (!g_regRoot) {
        return;
    }
    if (g_selectedSubKey < 0 ||
        g_selectedSubKey >= static_cast<int>(g_subKeys.size())) {
        return;
    }
    std::wstring oldPath =
        CombineRegPath(g_regPath, g_subKeys[g_selectedSubKey]);
    std::wstring newName = g_subKeys[g_selectedSubKey];
    if (!ShowRegistryInputBox(
        App::Instance()->GetHWND(),
        L"Переименование раздела",
        L"Новое имя раздела:",
        newName
    )) {
        return;
    }
    if (newName.empty() || newName == g_subKeys[g_selectedSubKey]) {
        return;
    }
    if (newName.find_first_of(L"\\/") != std::wstring::npos) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Имя раздела не должно содержать '\\' или '/'.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
        return;
    }
    if (!RegistryEditor::RenameKey(g_regRoot, oldPath, newName)) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Не удалось переименовать раздел.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
    }
    RefreshRegistryUI();
}

// Registry clipboard
static void CopySelectedKey(bool cut) {
    if (!g_regRoot) {
        return;
    }
    if (g_selectedSubKey < 0 ||
        g_selectedSubKey >= static_cast<int>(g_subKeys.size())) {
        return;
    }
    std::wstring fullPath =
        CombineRegPath(g_regPath, g_subKeys[g_selectedSubKey]);
    RegistryEditor::SetRegistryClipboard(
        RegistryEditor::RegistryClipboardType::Key,
        g_regRoot,
        fullPath,
        L"",
        cut
    );
}

static void CopySelectedValue(bool cut) {
    if (!g_regRoot) {
        return;
    }
    if (g_selectedValue < 0 ||
        g_selectedValue >= static_cast<int>(g_values.size())) {
        return;
    }
    RegistryEditor::SetRegistryClipboard(
        RegistryEditor::RegistryClipboardType::Value,
        g_regRoot,
        g_regPath,
        g_values[g_selectedValue].name,
        cut
    );
}

static bool CopyValueDirect(
    HKEY rootSrc,
    const std::wstring& srcPath,
    const std::wstring& valueName,
    HKEY rootDst,
    const std::wstring& dstPath,
    bool move
) {
    HKEY hSrc = nullptr;
    if (RegOpenKeyExW(rootSrc, srcPath.c_str(), 0, KEY_READ, &hSrc) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(
        hSrc,
        valueName.c_str(),
        nullptr,
        &type,
        nullptr,
        &size
    ) != ERROR_SUCCESS) {
        RegCloseKey(hSrc);
        return false;
    }
    std::vector<BYTE> data(size);
    if (size > 0) {
        if (RegQueryValueExW(
            hSrc,
            valueName.c_str(),
            nullptr,
            &type,
            data.data(),
            &size
        ) != ERROR_SUCCESS) {
            RegCloseKey(hSrc);
            return false;
        }
    }
    HKEY hDst = nullptr;
    if (RegOpenKeyExW(rootDst, dstPath.c_str(), 0, KEY_SET_VALUE, &hDst) != ERROR_SUCCESS) {
        RegCloseKey(hSrc);
        return false;
    }
    bool ok =
        RegSetValueExW(
            hDst,
            valueName.c_str(),
            0,
            type,
            data.data(),
            size
        ) == ERROR_SUCCESS;
    if (ok && move) {
        HKEY hSrcWrite = nullptr;
        if (RegOpenKeyExW(
            rootSrc,
            srcPath.c_str(),
            0,
            KEY_SET_VALUE,
            &hSrcWrite
        ) == ERROR_SUCCESS) {
            RegDeleteValueW(hSrcWrite, valueName.c_str());
            RegCloseKey(hSrcWrite);
        }
    }
    RegCloseKey(hSrc);
    RegCloseKey(hDst);
    return ok;
}

static void PasteClipboardToCurrentKey() {
    if (!g_regRoot) {
        return;
    }
    if (!RegistryEditor::HasRegistryClipboard()) {
        return;
    }
    RegistryEditor::RegistryClipboardType type =
        RegistryEditor::RegistryClipboardType::None;
    HKEY rootSrc = nullptr;
    std::wstring pathSrc;
    std::wstring valueName;
    bool cut = false;
    if (!RegistryEditor::GetRegistryClipboard(
        type,
        rootSrc,
        pathSrc,
        valueName,
        cut
    )) {
        return;
    }
    HWND hwnd = App::Instance()->GetHWND();
    bool ok = false;
    if (type == RegistryEditor::RegistryClipboardType::Key) {
        std::wstring keyName = GetLastPathSegment(pathSrc);
        if (keyName.empty()) {
            MessageBoxW(
                hwnd,
                L"Нельзя вставить корневой ключ.",
                L"Реестр",
                MB_OK | MB_ICONINFORMATION
            );
            return;
        }
        std::wstring dstPath = CombineRegPath(g_regPath, keyName);
        if (rootSrc == g_regRoot && pathSrc == dstPath) {
            MessageBoxW(
                hwnd,
                L"Нельзя вставить ключ сам в себя.",
                L"Реестр",
                MB_OK | MB_ICONINFORMATION
            );
            return;
        }
        if (cut) {
            ok = RegistryEditor::MoveKey(rootSrc, pathSrc, g_regRoot, dstPath);
        }
        else {
            ok = RegistryEditor::CopyKey(rootSrc, pathSrc, g_regRoot, dstPath);
        }
    }
    else if (type == RegistryEditor::RegistryClipboardType::Value) {
        if (rootSrc == g_regRoot && pathSrc == g_regPath) {
            return;
        }
        ok = CopyValueDirect(
            rootSrc,
            pathSrc,
            valueName,
            g_regRoot,
            g_regPath,
            cut
        );
    }
    if (cut) {
        RegistryEditor::ClearRegistryClipboard();
    }
    if (!ok) {
        MessageBoxW(
            hwnd,
            L"Не удалось выполнить вставку.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
    }
    RefreshRegistryUI();
}

// Export / Import
static std::wstring BrowseSaveRegFile() {
    wchar_t file[MAX_PATH] = L"export.reg";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = App::Instance()->GetHWND();
    ofn.lpstrFilter = L"Registry files (*.reg)\0*.reg\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&ofn)) {
        return file;
    }
    return L"";
}

static std::wstring BrowseOpenRegFile() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = App::Instance()->GetHWND();
    ofn.lpstrFilter = L"Registry files (*.reg)\0*.reg\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        return file;
    }
    return L"";
}

static bool Local_ExportRegistryKeyToFile(
    HKEY root,
    const std::wstring& path,
    const std::wstring& filePath
) {
    std::wstring fullPath = RootName(root);
    if (!path.empty()) {
        fullPath += L"\\" + path;
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

static bool Local_ImportRegistryFile(const std::wstring& filePath) {
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

static void ExportCurrentOrSelectedKey() {
    if (!g_regRoot) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Сначала выберите корень реестра.",
            L"Реестр",
            MB_OK | MB_ICONINFORMATION
        );
        return;
    }
    std::wstring exportPath = g_regPath;
    if (g_selectedSubKey >= 0 &&
        g_selectedSubKey < static_cast<int>(g_subKeys.size())) {
        exportPath = CombineRegPath(g_regPath, g_subKeys[g_selectedSubKey]);
    }
    std::wstring filePath = BrowseSaveRegFile();
    if (filePath.empty()) {
        return;
    }
    if (Local_ExportRegistryKeyToFile(g_regRoot, exportPath, filePath)) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Экспорт завершён.",
            L"Реестр",
            MB_OK | MB_ICONINFORMATION
        );
    }
    else {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Не удалось выполнить экспорт.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
    }
}

static void ImportRegistryFileUI() {
    std::wstring filePath = BrowseOpenRegFile();
    if (filePath.empty()) {
        return;
    }
    if (Local_ImportRegistryFile(filePath)) {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Импорт завершён.",
            L"Реестр",
            MB_OK | MB_ICONINFORMATION
        );
        RefreshRegistryUI();
    }
    else {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Не удалось выполнить импорт.",
            L"Ошибка",
            MB_OK | MB_ICONERROR
        );
    }
}

// Search
static void DoRegistrySearch() {
    HWND hwnd = App::Instance()->GetHWND();
    std::wstring text;
    if (!ShowRegistryInputBox(
        hwnd,
        L"Поиск в реестре",
        L"Что искать?",
        text
    )) {
        return;
    }
    if (text.empty()) {
        return;
    }
    HKEY searchRoot = nullptr;
    std::wstring searchStartPath;
    std::wstring searchBasePath;
    std::wstring searchDisplay;
    bool searchOffline = false;
    if (g_regRoot) {
        searchRoot = g_regRoot;
        searchStartPath = g_regPath;
        searchBasePath = g_rootBasePath;
        searchDisplay = g_rootDisplayName.empty()
            ? RootName(g_regRoot)
            : g_rootDisplayName;
        searchOffline = g_currentRootOffline;
    }
    else {
        if (IsRecoveryCached() &&
            g_offlineMounted &&
            g_offlineSoftwareMounted) {
            searchRoot = HKEY_LOCAL_MACHINE;
            searchStartPath = OFFLINE_SOFTWARE_MOUNT;
            searchBasePath = OFFLINE_SOFTWARE_MOUNT;
            searchDisplay = L"Офлайн: HKLM\\SOFTWARE";
            searchOffline = true;
        }
        else {
            searchRoot = HKEY_LOCAL_MACHINE;
            searchStartPath = L"";
            searchBasePath = L"";
            searchDisplay = L"HKEY_LOCAL_MACHINE";
            searchOffline = false;
        }
    }
    g_searchResults = Local_SearchRegistryAtPath(
        searchRoot,
        searchStartPath,
        text,
        true,
        true,
        true,
        200
    );
    g_searchRoot = searchRoot;
    g_searchRootBasePath = searchBasePath;
    g_searchRootDisplayName = searchDisplay;
    g_searchRootOffline = searchOffline;
    g_searchMode = true;
    g_selectedSearch = -1;
    RegInvalidate();
}

// Layout
struct RegistryLayout {
    RectF toolbar;
    RectF path;
    RectF leftList;
    RectF rightList;
    float leftDataTop = 0.0f;
    float rightDataTop = 0.0f;
    float rowHeight = 22.0f;
};

static RegistryLayout GetRegistryLayout(const RectF& contentArea) {
    RegistryLayout layout;
    const float margin = 8.0f;
    const float spacing = 6.0f;
    float topSafe = 8.0f;
    if (g_useVerticalLayout && contentArea.Y < 20.0f) {
        topSafe = 40.0f;
    }
    layout.toolbar = RectF(
        contentArea.X + margin,
        contentArea.Y + topSafe,
        contentArea.Width - 2.0f * margin,
        34.0f
    );
    layout.path = RectF(
        contentArea.X + margin,
        layout.toolbar.Y + layout.toolbar.Height + 6.0f,
        contentArea.Width - 2.0f * margin,
        26.0f
    );
    float listY = layout.path.Y + layout.path.Height + 6.0f;
    float listHeight =
        contentArea.Height - (listY - contentArea.Y) - margin;
    if (listHeight < 0.0f) {
        listHeight = 0.0f;
    }
    float totalListWidth =
        contentArea.Width - 2.0f * margin - spacing;
    if (totalListWidth < 1.0f) {
        totalListWidth = 1.0f;
    }
    const float minLeftWidth = 200.0f;
    const float minRightWidth = 280.0f;
    float leftWidth = totalListWidth * g_registryLeftPaneRatio;
    if (leftWidth < minLeftWidth) {
        leftWidth = minLeftWidth;
    }
    float maxLeftWidth = totalListWidth - minRightWidth;
    if (maxLeftWidth < minLeftWidth) {
        maxLeftWidth = minLeftWidth;
    }
    if (leftWidth > maxLeftWidth) {
        leftWidth = maxLeftWidth;
    }
    float rightWidth = totalListWidth - leftWidth;
    if (rightWidth < 0.0f) {
        rightWidth = 0.0f;
    }
    layout.leftList = RectF(
        contentArea.X + margin,
        listY,
        leftWidth,
        listHeight
    );
    layout.rightList = RectF(
        layout.leftList.X + layout.leftList.Width + spacing,
        listY,
        rightWidth,
        listHeight
    );
    const float headerHeight = 22.0f;
    layout.leftDataTop = layout.leftList.Y + headerHeight + 6.0f;
    layout.rightDataTop = layout.rightList.Y + headerHeight + 6.0f;
    layout.rowHeight = 22.0f;
    return layout;
}

// Registry panel & column resizing
static float GetRegistryValueNameWidth(const RectF& rightList) {
    float nameWidth = rightList.Width * g_registryValueNameRatio;
    const float minNameWidth = 90.0f;
    const float minDataWidth = 120.0f;
    if (nameWidth < minNameWidth) {
        nameWidth = minNameWidth;
    }
    float maxNameWidth = rightList.Width - minDataWidth;
    if (maxNameWidth < minNameWidth) {
        maxNameWidth = minNameWidth;
    }
    if (nameWidth > maxNameWidth) {
        nameWidth = maxNameWidth;
    }
    return nameWidth;
}

static int GetRegistryResizeHit(
    float fx,
    float fy,
    const RectF& contentArea
) {
    RegistryLayout layout = GetRegistryLayout(contentArea);
    if (fy < layout.leftList.Y ||
        fy > layout.leftList.Y + layout.leftList.Height) {
        return 0;
    }
    float splitX =
        layout.leftList.X +
        layout.leftList.Width +
        3.0f;
    float d1 = fx - splitX;
    if (d1 < 0.0f) {
        d1 = -d1;
    }
    if (d1 <= 6.0f) {
        return 1;
    }
    if (g_regRoot && !g_searchMode) {
        float nameWidth = GetRegistryValueNameWidth(layout.rightList);
        float valueSeparatorX =
            layout.rightList.X + nameWidth;
        float d2 = fx - valueSeparatorX;
        if (d2 < 0.0f) {
            d2 = -d2;
        }
        if (d2 <= 6.0f) {
            return 2;
        }
    }
    return 0;
}

bool RegistryLeftButtonDown(
    int x,
    int y,
    const RectF& contentArea
) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    int hit = GetRegistryResizeHit(fx, fy, contentArea);
    if (hit == 0) {
        return false;
    }
    if (hit == 1) {
        g_registryResizingSplit = true;
        g_registryResizingValueName = false;
    }
    else if (hit == 2) {
        g_registryResizingSplit = false;
        g_registryResizingValueName = true;
    }
    SetCapture(App::Instance()->GetHWND());
    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
    return true;
}

bool RegistryMouseMove(
    int x,
    int y,
    const RectF& contentArea
) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    if (g_registryResizingSplit) {
        const float margin = 8.0f;
        const float spacing = 6.0f;
        float totalListWidth =
            contentArea.Width - 2.0f * margin - spacing;
        if (totalListWidth < 1.0f) {
            totalListWidth = 1.0f;
        }
        const float minLeftWidth = 200.0f;
        const float minRightWidth = 280.0f;
        float maxLeftWidth = totalListWidth - minRightWidth;
        if (maxLeftWidth < minLeftWidth) {
            maxLeftWidth = minLeftWidth;
        }
        float newLeftWidth =
            fx -
            (contentArea.X + margin) -
            spacing * 0.5f;
        newLeftWidth = RegClampF(
            newLeftWidth,
            minLeftWidth,
            maxLeftWidth
        );
        g_registryLeftPaneRatio = newLeftWidth / totalListWidth;
        RegInvalidate();
        return true;
    }
    if (g_registryResizingValueName) {
        RegistryLayout layout = GetRegistryLayout(contentArea);
        if (layout.rightList.Width > 1.0f) {
            float newNameWidth = fx - layout.rightList.X;
            const float minNameWidth = 90.0f;
            const float minDataWidth = 120.0f;
            float maxNameWidth =
                layout.rightList.Width - minDataWidth;
            if (maxNameWidth < minNameWidth) {
                maxNameWidth = minNameWidth;
            }
            newNameWidth = RegClampF(
                newNameWidth,
                minNameWidth,
                maxNameWidth
            );
            g_registryValueNameRatio =
                newNameWidth / layout.rightList.Width;
            RegInvalidate();
        }
        return true;
    }
    int hit = GetRegistryResizeHit(fx, fy, contentArea);
    if (hit != 0) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return true;
    }
    return false;
}

bool RegistryLeftButtonUp() {
    if (!g_registryResizingSplit &&
        !g_registryResizingValueName) {
        return false;
    }
    g_registryResizingSplit = false;
    g_registryResizingValueName = false;
    ReleaseCapture();
    RegInvalidate();
    return true;
}

static int GetLeftItemAt(
    float fx,
    float fy,
    const RectF& contentArea
) {
    RegistryLayout layout = GetRegistryLayout(contentArea);
    if (!HitRect(layout.leftList, fx, fy)) {
        return -1;
    }
    if (fy < layout.leftDataTop) {
        return -1;
    }
    int row = static_cast<int>(
        (fy - layout.leftDataTop) / layout.rowHeight
        );
    int count = 0;
    if (!g_regRoot) {
        if (g_rootItems.empty()) {
            RefreshRootItems();
        }
        count = static_cast<int>(g_rootItems.size());
    }
    else if (g_searchMode) {
        count = static_cast<int>(g_searchResults.size());
    }
    else {
        count = static_cast<int>(g_subKeys.size());
    }
    if (row >= 0 && row < count) {
        return row;
    }
    return -1;
}

enum class RegistryValueColumn {
    None,
    Name,
    Data
};

static int GetRightItemAtEx(
    float fx,
    float fy,
    const RectF& contentArea,
    RegistryValueColumn& column
) {
    column = RegistryValueColumn::None;
    if (!g_regRoot || g_searchMode) {
        return -1;
    }
    RegistryLayout layout = GetRegistryLayout(contentArea);
    if (!HitRect(layout.rightList, fx, fy)) {
        return -1;
    }
    if (fy < layout.rightDataTop) {
        return -1;
    }
    int row = static_cast<int>(
        (fy - layout.rightDataTop) / layout.rowHeight
        );
    int count = static_cast<int>(g_values.size());
    if (row < 0 || row >= count) {
        return -1;
    }
    float nameWidth = GetRegistryValueNameWidth(layout.rightList);
    float xInsideList = fx - layout.rightList.X;
    if (xInsideList <= nameWidth) {
        column = RegistryValueColumn::Name;
    }
    else {
        column = RegistryValueColumn::Data;
    }
    return row;
}

// Execute context menu commands
static void ExecuteRegistryCommand(int cmd) {
    switch (cmd) {
    case IDM_REG_OPEN: {
        if (g_selectedSubKey >= 0) {
            OpenSubKey(g_selectedSubKey);
        }
        else if (g_selectedSearch >= 0) {
            OpenSearchResult(g_selectedSearch);
        }
        break;
    }
    case IDM_REG_GOTO: {
        if (g_selectedSearch >= 0) {
            OpenSearchResult(g_selectedSearch);
        }
        break;
    }
    case IDM_REG_COPY_PATH: {
        if (g_selectedSearch >= 0 &&
            g_selectedSearch < static_cast<int>(g_searchResults.size())) {
            const RegistryEditor::SearchResult& result =
                g_searchResults[g_selectedSearch];
            std::wstring fullPath = RootName(g_searchRoot);
            if (!result.keyPath.empty()) {
                fullPath += L"\\" + result.keyPath;
            }
            if (!result.valueName.empty()) {
                fullPath += L"\\" + result.valueName;
            }
            CopyTextToClipboard(fullPath);
        }
        break;
    }
    case IDM_REG_NEW_KEY: {
        CreateNewKey();
        break;
    }
    case IDM_REG_NEW_VALUE_SZ: {
        CreateNewValue(REG_SZ);
        break;
    }
    case IDM_REG_NEW_VALUE_DWORD: {
        CreateNewValue(REG_DWORD);
        break;
    }
    case IDM_REG_MODIFY: {
        ModifySelectedValue();
        break;
    }
    case IDM_REG_COPY_KEY: {
        CopySelectedKey(false);
        break;
    }
    case IDM_REG_CUT_KEY: {
        CopySelectedKey(true);
        break;
    }
    case IDM_REG_COPY_VALUE: {
        CopySelectedValue(false);
        break;
    }
    case IDM_REG_CUT_VALUE: {
        CopySelectedValue(true);
        break;
    }
    case IDM_REG_PASTE: {
        PasteClipboardToCurrentKey();
        break;
    }
    case IDM_REG_DELETE_KEY: {
        DeleteSelectedKey();
        break;
    }
    case IDM_REG_DELETE_VALUE: {
        DeleteSelectedValue();
        break;
    }
    case IDM_REG_RENAME_KEY: {
        RenameSelectedKey();
        break;
    }
    case IDM_REG_RENAME_VALUE: {
        RenameSelectedValue();
        break;
    }
    case IDM_REG_EXPORT: {
        ExportCurrentOrSelectedKey();
        break;
    }
    case IDM_REG_IMPORT: {
        ImportRegistryFileUI();
        break;
    }
    case IDM_REG_SEARCH: {
        DoRegistrySearch();
        break;
    }
    case IDM_REG_OFFLINE: {
        OfflineButtonClicked();
        break;
    }
    case IDM_REG_REFRESH: {
        if (!g_regRoot &&
            IsRecoveryCached() &&
            !g_offlineMounted) {
            g_foundWindowsScanned = false;
            RefreshRootItems();
            RegInvalidate();
        }
        else {
            g_searchMode = false;
            g_searchResults.clear();
            g_selectedSearch = -1;
            RefreshRegistryUI();
        }
        break;
    }
    case IDM_REG_PERMISSIONS: {
        MessageBoxW(
            App::Instance()->GetHWND(),
            L"Права доступа будут реализованы позже.",
            L"Реестр",
            MB_OK | MB_ICONINFORMATION
        );
        break;
    }
    default:
        break;
    }
}

// Context menu
static int ShowRegistryPopupMenu(HWND hwnd, HMENU menu, int x, int y) {
    POINT pt{ x, y };
    ClientToScreen(hwnd, &pt);
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        pt.x,
        pt.y,
        0,
        hwnd,
        nullptr
    );
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    return cmd;
}

static HMENU BuildNewValueSubmenu() {
    HMENU submenu = CreatePopupMenu();
    AppendMenuW(
        submenu,
        MF_STRING,
        IDM_REG_NEW_VALUE_SZ,
        L"Строковый параметр (REG_SZ)"
    );
    AppendMenuW(
        submenu,
        MF_STRING,
        IDM_REG_NEW_VALUE_DWORD,
        L"Параметр DWORD (32-bit)"
    );
    return submenu;
}

// Basic elements rendering
static void DrawRegButton(
    Graphics& g,
    const RectF& rect,
    const std::wstring& text,
    Font& font,
    SolidBrush& bgBrush,
    SolidBrush& textBrush,
    Pen& borderPen
) {
    g.FillRectangle(&bgBrush, rect);
    g.DrawRectangle(&borderPen, rect);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text.c_str(), -1, &font, rect, &format, &textBrush);
}

// === НОВАЯ ФУНКЦИЯ: Рисуем строку с цветным квадратиком ===
static void DrawRegListItemWithIcon(
    Graphics& g,
    const RectF& listRect,
    float y,
    float rowHeight,
    const std::wstring& text,
    const Color& iconColor,
    Font& font,
    SolidBrush& textBrush,
    SolidBrush& selectedBrush,
    bool selected
) {
    RectF rowRect(
        listRect.X + 2.0f,
        y,
        listRect.Width - 4.0f,
        rowHeight
    );
    if (selected) {
        g.FillRectangle(&selectedBrush, rowRect);
    }

    // Цветной квадратик (как в проводнике)
    const float iconSize = 10.0f;
    const float iconX = rowRect.X + 6.0f;
    const float iconY = y + (rowHeight - iconSize) / 2.0f;
    SolidBrush iconBrush(iconColor);
    g.FillRectangle(&iconBrush, RectF(iconX, iconY, iconSize, iconSize));

    // Текст со сдвигом
    RectF textRect(
        iconX + iconSize + 6.0f,
        rowRect.Y,
        rowRect.Width - iconSize - 18.0f,
        rowRect.Height
    );
    StringFormat format;
    format.SetAlignment(StringAlignmentNear);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(text.c_str(), -1, &font, textRect, &format, &textBrush);
}

// === Цвета иконок ===
static const Color COLOR_ICON_ROOT = Color(255, 156, 39, 176);   // Фиолетовый — корни реестра
static const Color COLOR_ICON_KEY = Color(255, 33, 150, 243);    // Синий — разделы/ключи (как папки)
static const Color COLOR_ICON_VALUE = Color(255, 76, 175, 80);   // Зелёный — параметры (как файлы)
static const Color COLOR_ICON_OFFLINE = Color(255, 255, 152, 0); // Оранжевый — офлайн-кусты

// Toolbar
struct ToolDef {
    int id;
    const wchar_t* text;
    float width;
};

static const ToolDef TOOL_DEFS[] = {
    { TOOL_UP,        L"Вверх",     64.0f },
    { TOOL_REFRESH,   L"Обновить",  84.0f },
    { TOOL_OFFLINE,   L"Офлайн",    70.0f },
    { TOOL_SEARCH,    L"Поиск",     64.0f },
    { TOOL_EXPORT,    L"Экспорт",   78.0f },
    { TOOL_IMPORT,    L"Импорт",    76.0f },
    { TOOL_NEW_KEY,   L"Раздел",    72.0f },
    { TOOL_NEW_VALUE, L"Параметр",  88.0f }
};

static void DrawToolbar(
    Graphics& g,
    const RectF& toolbarRect,
    Font& font,
    SolidBrush& bgBrush,
    SolidBrush& textBrush,
    Pen& borderPen
) {
    g_toolbarButtons.clear();
    g_toolbarArrowsVisible = false;
    g_toolbarLeftArrow = RectF();
    g_toolbarRightArrow = RectF();
    if (toolbarRect.Width <= 0.0f || toolbarRect.Height <= 0.0f) {
        return;
    }
    const float spacing = 6.0f;
    const float buttonHeight = 26.0f;
    const float arrowWidth = 24.0f;
    float totalWidth = 0.0f;
    for (const auto& def : TOOL_DEFS) {
        totalWidth += def.width + spacing;
    }
    if (totalWidth > 0.0f) {
        totalWidth -= spacing;
    }
    float buttonsX = toolbarRect.X;
    float buttonsWidth = toolbarRect.Width;
    bool needArrows = totalWidth > toolbarRect.Width;
    if (needArrows && toolbarRect.Width < 60.0f) {
        needArrows = false;
    }
    if (needArrows) {
        g_toolbarArrowsVisible = true;
        g_toolbarLeftArrow = RectF(
            toolbarRect.X,
            toolbarRect.Y,
            arrowWidth,
            toolbarRect.Height
        );
        g_toolbarRightArrow = RectF(
            toolbarRect.X + toolbarRect.Width - arrowWidth,
            toolbarRect.Y,
            arrowWidth,
            toolbarRect.Height
        );
        buttonsX += arrowWidth + 4.0f;
        buttonsWidth -= arrowWidth * 2.0f + 8.0f;
    }
    if (buttonsWidth < 0.0f) {
        buttonsWidth = 0.0f;
    }
    float maxOffset = totalWidth - buttonsWidth;
    if (maxOffset < 0.0f) {
        maxOffset = 0.0f;
    }
    if (g_toolbarOffset < 0.0f) {
        g_toolbarOffset = 0.0f;
    }
    if (g_toolbarOffset > maxOffset) {
        g_toolbarOffset = maxOffset;
    }
    if (needArrows) {
        if (g_toolbarOffset > 0.0f) {
            SolidBrush enabledBrush(COLOR_TEXT);
            DrawRegButton(
                g,
                g_toolbarLeftArrow,
                L"<",
                font,
                bgBrush,
                enabledBrush,
                borderPen
            );
        }
        else {
            SolidBrush disabledBrush(COLOR_TEXT_MUTED);
            DrawRegButton(
                g,
                g_toolbarLeftArrow,
                L"<",
                font,
                bgBrush,
                disabledBrush,
                borderPen
            );
        }
        if (g_toolbarOffset < maxOffset) {
            SolidBrush enabledBrush(COLOR_TEXT);
            DrawRegButton(
                g,
                g_toolbarRightArrow,
                L">",
                font,
                bgBrush,
                enabledBrush,
                borderPen
            );
        }
        else {
            SolidBrush disabledBrush(COLOR_TEXT_MUTED);
            DrawRegButton(
                g,
                g_toolbarRightArrow,
                L">",
                font,
                bgBrush,
                disabledBrush,
                borderPen
            );
        }
    }
    float buttonY =
        toolbarRect.Y +
        (toolbarRect.Height - buttonHeight) * 0.5f;
    float x = buttonsX - g_toolbarOffset;
    RectF clipRect(
        buttonsX,
        toolbarRect.Y,
        buttonsWidth,
        toolbarRect.Height
    );
    GraphicsContainer container = g.BeginContainer();
    g.SetClip(clipRect);
    for (const auto& def : TOOL_DEFS) {
        RectF buttonRect(
            x,
            buttonY,
            def.width,
            buttonHeight
        );
        bool visible =
            buttonRect.X >= buttonsX &&
            buttonRect.X + buttonRect.Width <= buttonsX + buttonsWidth;
        if (visible) {
            DrawRegButton(
                g,
                buttonRect,
                def.text,
                font,
                bgBrush,
                textBrush,
                borderPen
            );
            ToolbarButton tb;
            tb.id = def.id;
            tb.rect = buttonRect;
            g_toolbarButtons.push_back(tb);
        }
        x += def.width + spacing;
    }
    g.EndContainer(container);
}

// Tab rendering
void DrawRegistryContent(
    Graphics& g,
    const RectF& contentArea,
    Font& contentFont
) {
    (void)contentFont;
    g_maxScroll[3] = 0;
    g_scrollOffset[3] = 0;
    FontFamily fontFamily(g_fontFamilyName.c_str());
    Font headFont(
        &fontFamily,
        12.0f,
        FontStyleBold,
        UnitPixel
    );
    Font itemFont(
        &fontFamily,
        11.5f,
        FontStyleRegular,
        UnitPixel
    );
    Font smallFont(
        &fontFamily,
        10.5f,
        FontStyleRegular,
        UnitPixel
    );
    Font toolbarFont(
        &fontFamily,
        10.5f,
        FontStyleRegular,
        UnitPixel
    );
    SolidBrush textBrush(COLOR_TEXT);
    SolidBrush mutedBrush(COLOR_TEXT_MUTED);
    SolidBrush bgBrush(COLOR_TAB_BG);
    SolidBrush selectedBrush(COLOR_TAB_ACTIVE);
    Pen borderPen(COLOR_BORDER, 1.0f);
    RegistryLayout layout = GetRegistryLayout(contentArea);

    // Toolbar
    DrawToolbar(
        g,
        layout.toolbar,
        toolbarFont,
        bgBrush,
        textBrush,
        borderPen
    );

    // Address bar
    g.FillRectangle(&bgBrush, layout.path);
    g.DrawRectangle(&borderPen, layout.path);
    std::wstring pathText = GetCurrentPathDisplay();
    if (g_searchMode) {
        pathText += L"   [режим поиска]";
    }
    RectF pathTextRect(
        layout.path.X + 8.0f,
        layout.path.Y,
        layout.path.Width - 16.0f,
        layout.path.Height
    );
    StringFormat pathFormat;
    pathFormat.SetAlignment(StringAlignmentNear);
    pathFormat.SetLineAlignment(StringAlignmentCenter);
    pathFormat.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(
        pathText.c_str(),
        -1,
        &itemFont,
        pathTextRect,
        &pathFormat,
        &mutedBrush
    );

    // Left & right panels
    g.FillRectangle(&bgBrush, layout.leftList);
    g.DrawRectangle(&borderPen, layout.leftList);
    g.FillRectangle(&bgBrush, layout.rightList);
    g.DrawRectangle(&borderPen, layout.rightList);

    StringFormat headerFormat;
    headerFormat.SetAlignment(StringAlignmentNear);
    headerFormat.SetLineAlignment(StringAlignmentCenter);
    RectF leftHeaderRect(
        layout.leftList.X + 6.0f,
        layout.leftList.Y + 3.0f,
        layout.leftList.Width - 12.0f,
        20.0f
    );
    RectF rightHeaderRect(
        layout.rightList.X + 6.0f,
        layout.rightList.Y + 3.0f,
        layout.rightList.Width - 12.0f,
        20.0f
    );
    std::wstring leftHeader = L"Разделы";
    if (!g_regRoot) {
        if (IsRecoveryCached() && !g_offlineMounted) {
            leftHeader = L"Офлайн Windows / текущая среда";
        }
        else {
            leftHeader = L"Корни реестра";
        }
    }
    else if (g_searchMode) {
        leftHeader = L"Результаты поиска";
    }
    g.DrawString(
        leftHeader.c_str(),
        -1,
        &headFont,
        leftHeaderRect,
        &headerFormat,
        &textBrush
    );
    g.DrawString(
        L"Параметры",
        -1,
        &headFont,
        rightHeaderRect,
        &headerFormat,
        &textBrush
    );

    // === Визуальный разделитель между панелями (толстая линия) ===
    float splitX = layout.leftList.X + layout.leftList.Width + 3.0f;
    Pen splitPen(Color(80, 80, 80), 2.0f);
    g.DrawLine(&splitPen, splitX, layout.leftList.Y, splitX, layout.leftList.Y + layout.leftList.Height);

    float rowY = layout.leftDataTop;

    // === LEFT PANEL ===
    if (!g_regRoot) {
        if (g_rootItems.empty()) {
            RefreshRootItems();
        }
        for (int i = 0; i < static_cast<int>(g_rootItems.size()); ++i) {
            const RootItem& item = g_rootItems[i];
            bool isSeparator = item.root == nullptr && item.action == RootAction::Normal && item.display.find(L"---") == 0;
            if (isSeparator) {
                // Разделитель — просто текст без иконки
                RectF rowRect(layout.leftList.X + 2.0f, rowY, layout.leftList.Width - 4.0f, layout.rowHeight);
                RectF textRect(rowRect.X + 6.0f, rowRect.Y, rowRect.Width - 12.0f, rowRect.Height);
                StringFormat format;
                format.SetAlignment(StringAlignmentNear);
                format.SetLineAlignment(StringAlignmentCenter);
                format.SetTrimming(StringTrimmingEllipsisCharacter);
                g.DrawString(item.display.c_str(), -1, &itemFont, textRect, &format, &mutedBrush);
            }
            else {
                // Корень реестра — фиолетовый квадратик (офлайн — оранжевый)
                Color iconColor = item.offline ? COLOR_ICON_OFFLINE : COLOR_ICON_ROOT;
                DrawRegListItemWithIcon(g, layout.leftList, rowY, layout.rowHeight, item.display, iconColor, itemFont, textBrush, selectedBrush, false);
            }
            rowY += layout.rowHeight;
        }
    }
    else if (g_searchMode) {
        if (g_searchResults.empty()) {
            RectF emptyRect(
                layout.leftList.X + 8.0f,
                layout.leftDataTop,
                layout.leftList.Width - 16.0f,
                layout.rowHeight
            );
            g.DrawString(
                L"Ничего не найдено",
                -1,
                &itemFont,
                emptyRect,
                &headerFormat,
                &mutedBrush
            );
        }
        else {
            for (int i = 0; i < static_cast<int>(g_searchResults.size()); ++i) {
                const RegistryEditor::SearchResult& result =
                    g_searchResults[i];
                std::wstring text = result.keyPath;
                if (text.empty()) {
                    text = L"(корень)";
                }
                if (!result.valueName.empty()) {
                    text += L" → " + result.valueName;
                }
                // Если есть valueName — зелёный (параметр), иначе синий (ключ)
                Color iconColor = result.valueName.empty() ? COLOR_ICON_KEY : COLOR_ICON_VALUE;
                DrawRegListItemWithIcon(g, layout.leftList, rowY, layout.rowHeight, text, iconColor, itemFont, textBrush, selectedBrush, i == g_selectedSearch);
                rowY += layout.rowHeight;
            }
        }
    }
    else {
        for (int i = 0; i < static_cast<int>(g_subKeys.size()); ++i) {
            // Ключ/раздел — синий квадратик (как папка)
            DrawRegListItemWithIcon(g, layout.leftList, rowY, layout.rowHeight, g_subKeys[i], COLOR_ICON_KEY, itemFont, textBrush, selectedBrush, i == g_selectedSubKey);
            rowY += layout.rowHeight;
        }
    }

    // === RIGHT PANEL ===
    rowY = layout.rightDataTop;
    if (!g_regRoot) {
        RectF hintRect(
            layout.rightList.X + 8.0f,
            layout.rightDataTop,
            layout.rightList.Width - 16.0f,
            layout.rowHeight
        );
        g.DrawString(
            L"Выберите корень реестра слева",
            -1,
            &itemFont,
            hintRect,
            &headerFormat,
            &mutedBrush
        );
    }
    else if (g_searchMode) {
        if (g_selectedSearch >= 0 &&
            g_selectedSearch < static_cast<int>(g_searchResults.size())) {
            const RegistryEditor::SearchResult& result =
                g_searchResults[g_selectedSearch];
            float infoY = layout.rightDataTop;
            auto DrawInfoLine = [&](const std::wstring& text) {
                RectF lineRect(
                    layout.rightList.X + 8.0f,
                    infoY,
                    layout.rightList.Width - 16.0f,
                    layout.rowHeight
                );
                g.DrawString(
                    text.c_str(),
                    -1,
                    &itemFont,
                    lineRect,
                    &headerFormat,
                    &textBrush
                );
                infoY += layout.rowHeight;
                };
            DrawInfoLine(L"Ключ: " + result.keyPath);
            DrawInfoLine(L"Параметр: " + result.valueName);
            DrawInfoLine(L"Данные: " + result.data);
        }
        else {
            RectF hintRect(
                layout.rightList.X + 8.0f,
                layout.rightDataTop,
                layout.rightList.Width - 16.0f,
                layout.rowHeight
            );
            g.DrawString(
                L"Выберите результат поиска слева",
                -1,
                &itemFont,
                hintRect,
                &headerFormat,
                &mutedBrush
            );
        }
    }
    else {
        float nameWidth = GetRegistryValueNameWidth(layout.rightList);

        // === Разделитель между колонками "Имя" и "Данные" ===
        float valueSepX = layout.rightList.X + nameWidth;
        Pen valueSepPen(Color(60, 60, 60), 1.0f);
        g.DrawLine(&valueSepPen, valueSepX, layout.rightList.Y, valueSepX, layout.rightList.Y + layout.rightList.Height);

        for (int i = 0; i < static_cast<int>(g_values.size()); ++i) {
            const RegistryEditor::RegValue& value = g_values[i];
            RectF rowRect(
                layout.rightList.X + 2.0f,
                rowY,
                layout.rightList.Width - 4.0f,
                layout.rowHeight
            );
            if (i == g_selectedValue) {
                g.FillRectangle(&selectedBrush, rowRect);
            }

            // Зелёный квадратик для параметра (как файл)
            const float iconSize = 10.0f;
            const float iconX = rowRect.X + 6.0f;
            const float iconY = rowY + (layout.rowHeight - iconSize) / 2.0f;
            SolidBrush iconBrush(COLOR_ICON_VALUE);
            g.FillRectangle(&iconBrush, RectF(iconX, iconY, iconSize, iconSize));

            float textStartX = iconX + iconSize + 6.0f;
            RectF nameRect(
                textStartX,
                rowRect.Y,
                nameWidth - (textStartX - rowRect.X) - 6.0f,
                rowRect.Height
            );
            RectF dataRect(
                rowRect.X + nameWidth,
                rowRect.Y,
                rowRect.Width - nameWidth - 8.0f,
                rowRect.Height
            );
            StringFormat rowFormat;
            rowFormat.SetAlignment(StringAlignmentNear);
            rowFormat.SetLineAlignment(StringAlignmentCenter);
            rowFormat.SetTrimming(StringTrimmingEllipsisCharacter);
            std::wstring displayName = value.name;
            if (displayName.empty()) {
                displayName = L"(По умолчанию)";
            }
            if (g_registryEdit.active &&
                g_registryEdit.target == RegistryEditTarget::ValueName &&
                i == g_registryEdit.index) {
                Pen editPen(COLOR_TAB_ACTIVE, 1.5f);
                g.DrawRectangle(&editPen, nameRect);
                g.DrawString(
                    g_registryEdit.text.c_str(),
                    -1,
                    &itemFont,
                    nameRect,
                    &rowFormat,
                    &textBrush
                );
            }
            else {
                g.DrawString(
                    displayName.c_str(),
                    -1,
                    &itemFont,
                    nameRect,
                    &rowFormat,
                    &textBrush
                );
            }
            if (g_registryEdit.active &&
                g_registryEdit.target == RegistryEditTarget::ValueData &&
                i == g_registryEdit.index) {
                Pen editPen(COLOR_TAB_ACTIVE, 1.5f);
                g.DrawRectangle(&editPen, dataRect);
                g.DrawString(
                    g_registryEdit.text.c_str(),
                    -1,
                    &smallFont,
                    dataRect,
                    &rowFormat,
                    &textBrush
                );
            }
            else {
                g.DrawString(
                    RegValueDataToDisplay(value).c_str(),
                    -1,
                    &smallFont,
                    dataRect,
                    &rowFormat,
                    &mutedBrush
                );
            }
            rowY += layout.rowHeight;
        }
    }
}

// Left Click
bool OnRegistryClick(
    int x,
    int y,
    const RectF& contentArea
) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    // Toolbar scroll arrows
    if (g_toolbarArrowsVisible) {
        if (HitRect(g_toolbarLeftArrow, fx, fy)) {
            g_toolbarOffset -= 140.0f;
            if (g_toolbarOffset < 0.0f) {
                g_toolbarOffset = 0.0f;
            }
            RegInvalidate();
            return true;
        }
        if (HitRect(g_toolbarRightArrow, fx, fy)) {
            g_toolbarOffset += 140.0f;
            RegInvalidate();
            return true;
        }
    }
    // Toolbar buttons
    for (const ToolbarButton& button : g_toolbarButtons) {
        if (HitRect(button.rect, fx, fy)) {
            switch (button.id) {
            case TOOL_UP:
                GoUp();
                break;
            case TOOL_REFRESH:
                if (!g_regRoot &&
                    IsRecoveryCached() &&
                    !g_offlineMounted) {
                    g_foundWindowsScanned = false;
                    RefreshRootItems();
                    RegInvalidate();
                }
                else {
                    g_searchMode = false;
                    g_searchResults.clear();
                    g_selectedSearch = -1;
                    RefreshRegistryUI();
                }
                break;
            case TOOL_OFFLINE:
                OfflineButtonClicked();
                break;
            case TOOL_SEARCH:
                DoRegistrySearch();
                break;
            case TOOL_EXPORT:
                ExportCurrentOrSelectedKey();
                break;
            case TOOL_IMPORT:
                ImportRegistryFileUI();
                break;
            case TOOL_NEW_KEY:
                CreateNewKey();
                break;
            case TOOL_NEW_VALUE:
                CreateNewValue(REG_SZ);
                break;
            default:
                break;
            }
            return true;
        }
    }
    // Left panel
    int leftItem = GetLeftItemAt(fx, fy, contentArea);
    if (leftItem >= 0) {
        if (!g_regRoot) {
            if (g_rootItems.empty()) {
                RefreshRootItems();
            }
            if (leftItem >= 0 &&
                leftItem < static_cast<int>(g_rootItems.size())) {
                GoToRootItem(g_rootItems[leftItem]);
            }
            return true;
        }
        if (g_searchMode) {
            g_selectedSearch = leftItem;
            OpenSearchResult(leftItem);
            return true;
        }
        if (g_selectedSubKey == leftItem) {
            BeginEditKeyName(leftItem);
        }
        else {
            g_selectedSubKey = leftItem;
        }
        OpenSubKey(leftItem);
        return true;
    }
    // Right panel
    RegistryValueColumn valueColumn = RegistryValueColumn::None;
    int rightItem = GetRightItemAtEx(
        fx,
        fy,
        contentArea,
        valueColumn
    );
    if (rightItem >= 0) {
        if (g_selectedValue == rightItem) {
            if (valueColumn == RegistryValueColumn::Name) {
                BeginEditValueName(rightItem);
            }
            else if (valueColumn == RegistryValueColumn::Data) {
                BeginEditValueData(rightItem);
            }
        }
        else {
            g_selectedValue = rightItem;
        }
        RegInvalidate();
        return true;
    }
    return false;
}

// Right-click / Context menu
bool OnRegistryRightClick(
    int x,
    int y,
    const RectF& contentArea
) {
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    HWND hwnd = App::Instance()->GetHWND();
    // Registry roots / Offline list
    if (!g_regRoot) {
        if (g_rootItems.empty()) {
            RefreshRootItems();
        }
        int itemIndex = GetLeftItemAt(fx, fy, contentArea);
        if (itemIndex >= 0 &&
            itemIndex < static_cast<int>(g_rootItems.size())) {
            const RootItem& item = g_rootItems[itemIndex];
            if (item.root == nullptr && item.action == RootAction::Normal) {
                return false;
            }
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, IDM_REG_OPEN, L"Открыть");
            int cmd = ShowRegistryPopupMenu(hwnd, menu, x, y);
            if (cmd == IDM_REG_OPEN) {
                GoToRootItem(item);
            }
            return true;
        }
        return false;
    }
    // Search result
    if (g_searchMode) {
        int item = GetLeftItemAt(fx, fy, contentArea);
        if (item >= 0) {
            g_selectedSearch = item;
            RegInvalidate();
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, IDM_REG_GOTO, L"Перейти к ключу");
            AppendMenuW(menu, MF_STRING, IDM_REG_COPY_PATH, L"Копировать путь");
            int cmd = ShowRegistryPopupMenu(hwnd, menu, x, y);
            ExecuteRegistryCommand(cmd);
            return true;
        }
        return false;
    }
    // Parameter click
    RegistryValueColumn valueColumn = RegistryValueColumn::None;
    int valueItem = GetRightItemAtEx(
        fx,
        fy,
        contentArea,
        valueColumn
    );
    if (valueItem >= 0) {
        g_selectedValue = valueItem;
        RegInvalidate();
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDM_REG_MODIFY, L"Изменить");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_REG_COPY_VALUE, L"Копировать значение");
        AppendMenuW(menu, MF_STRING, IDM_REG_CUT_VALUE, L"Вырезать значение");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_REG_RENAME_VALUE, L"Переименовать");
        AppendMenuW(menu, MF_STRING, IDM_REG_DELETE_VALUE, L"Удалить");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_REG_PERMISSIONS, L"Права доступа");
        int cmd = ShowRegistryPopupMenu(hwnd, menu, x, y);
        ExecuteRegistryCommand(cmd);
        return true;
    }
    // Key click
    int keyItem = GetLeftItemAt(fx, fy, contentArea);
    if (keyItem >= 0) {
        g_selectedSubKey = keyItem;
        RegInvalidate();
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDM_REG_OPEN, L"Открыть");
        AppendMenuW(menu, MF_STRING, IDM_REG_NEW_KEY, L"Создать раздел");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_REG_COPY_KEY, L"Копировать ключ");
        AppendMenuW(menu, MF_STRING, IDM_REG_CUT_KEY, L"Вырезать ключ");
        AppendMenuW(
            menu,
            RegistryEditor::HasRegistryClipboard() ? MF_STRING : MF_GRAYED,
            IDM_REG_PASTE,
            L"Вставить"
        );
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_REG_RENAME_KEY, L"Переименовать");
        AppendMenuW(menu, MF_STRING, IDM_REG_DELETE_KEY, L"Удалить");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_REG_EXPORT, L"Экспорт ключа");
        AppendMenuW(menu, MF_STRING, IDM_REG_PERMISSIONS, L"Права доступа");
        int cmd = ShowRegistryPopupMenu(hwnd, menu, x, y);
        ExecuteRegistryCommand(cmd);
        return true;
    }
    // Click on empty space
    g_selectedSubKey = -1;
    g_selectedValue = -1;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_REG_NEW_KEY, L"Создать раздел");
    HMENU newValueSubmenu = BuildNewValueSubmenu();
    AppendMenuW(
        menu,
        MF_POPUP,
        reinterpret_cast<UINT_PTR>(newValueSubmenu),
        L"Создать значение"
    );
    AppendMenuW(
        menu,
        RegistryEditor::HasRegistryClipboard() ? MF_STRING : MF_GRAYED,
        IDM_REG_PASTE,
        L"Вставить"
    );
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        menu,
        MF_STRING,
        IDM_REG_OFFLINE,
        g_offlineMounted ? L"Размонтировать офлайн" : L"Монтировать офлайн"
    );
    AppendMenuW(menu, MF_STRING, IDM_REG_IMPORT, L"Импорт");
    AppendMenuW(menu, MF_STRING, IDM_REG_EXPORT, L"Экспорт");
    AppendMenuW(menu, MF_STRING, IDM_REG_SEARCH, L"Поиск");
    AppendMenuW(menu, MF_STRING, IDM_REG_REFRESH, L"Обновить");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_REG_PERMISSIONS, L"Права доступа");
    int cmd = ShowRegistryPopupMenu(hwnd, menu, x, y);
    ExecuteRegistryCommand(cmd);
    return true;
}

// Keyboard handling for inline editing
bool OnRegistryKey(
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
) {
    (void)lParam;
    if (!g_registryEdit.active) {
        return false;
    }
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            CommitRegistryEdit();
            return true;
        }
        if (wParam == VK_ESCAPE) {
            CancelRegistryEdit();
            return true;
        }
        return true;
    }
    if (msg == WM_CHAR) {
        wchar_t ch = static_cast<wchar_t>(wParam);
        if (ch == L'\r') {
            CommitRegistryEdit();
            return true;
        }
        if (ch == L'\x1b') {
            CancelRegistryEdit();
            return true;
        }
        if (ch == L'\b') {
            if (!g_registryEdit.text.empty()) {
                g_registryEdit.text.pop_back();
                RegInvalidate();
            }
            return true;
        }
        if (ch >= L' ') {
            g_registryEdit.text.push_back(ch);
            RegInvalidate();
            return true;
        }
        return true;
    }
    return false;
}