#include "app.h"
#include "wndproc.h"
#include "globals.h"
#include "features/home.h"
#include "features/settings.h"
#include <gdiplus.h>
#include <shellapi.h>
#include <string>
#include "Resource.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;
static ULONG_PTR gdiplusToken = 0;
App* App::s_pInstance = nullptr;

bool App::Init(HINSTANCE hInstance) {
    m_hInst = hInstance;
    s_pInstance = this;

    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Ok) {
        MessageBoxW(nullptr, L"GDI+ startup failed", L"Error", MB_ICONERROR);
        return false;
    }

    // === Init ===
    // Ресурсы
    std::wstring fontPath = L"assets\\embedded_font.ttf";
    PrivateFontCollection pfc;
    if (pfc.AddFontFile(fontPath.c_str()) == Ok) {
        int familyCount = 0;
        pfc.GetFamilies(0, nullptr, &familyCount);
        if (familyCount > 0) {
            std::vector<FontFamily> families(familyCount);
            pfc.GetFamilies(familyCount, &families[0], &familyCount);
            WCHAR name[256] = { 0 };
            families[0].GetFamilyName(name);
            g_fontFamilyName = name;
        }
    }

    // Окно
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_DBLCLKS;
    wcex.lpfnWndProc = MainWndProc;
    wcex.hInstance = m_hInst;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = nullptr;
    wcex.lpszClassName = L"SYSIM_Win32_GUI";
    wcex.hIcon = (HICON)LoadImageW(m_hInst, MAKEINTRESOURCE(IDI_SYSIM),
        IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
    wcex.hIconSm = (HICON)LoadImageW(m_hInst, MAKEINTRESOURCE(IDI_SYSIM),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wcex);

    DWORD exStyle = WS_EX_APPWINDOW;
    m_hWnd = CreateWindowExW(exStyle, wcex.lpszClassName, L"SYSIM - Utility",
        WS_POPUP | WS_CLIPCHILDREN,
        CW_USEDEFAULT, 0, 900, 600, nullptr, nullptr, m_hInst, nullptr);
    if (!m_hWnd) return false;

    InitHomeButtons();
    ApplyDefaultSettings();
    return true;
}

int App::Run(int nCmdShow) {
    ShowWindow(m_hWnd, nCmdShow);
    UpdateWindow(m_hWnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    GdiplusShutdown(gdiplusToken);
    return (int)msg.wParam;
}

// === Back buffer ===
void App::EnsureBackBuffer(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (m_memBitmap && m_memWidth == width && m_memHeight == height) return;

    ReleaseBackBuffer();

    HDC hdcScreen = GetDC(nullptr);
    m_memDC = CreateCompatibleDC(hdcScreen);
    m_memBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    if (m_memDC && m_memBitmap) {
        m_oldBitmap = (HBITMAP)SelectObject(m_memDC, m_memBitmap);
    }
    ReleaseDC(nullptr, hdcScreen);

    m_memWidth = width;
    m_memHeight = height;
}

void App::ReleaseBackBuffer() {
    if (m_memDC) {
        if (m_oldBitmap) SelectObject(m_memDC, m_oldBitmap);
        DeleteDC(m_memDC);
        m_memDC = nullptr;
        m_oldBitmap = nullptr;
    }
    if (m_memBitmap) {
        DeleteObject(m_memBitmap);
        m_memBitmap = nullptr;
    }
    m_memWidth = 0;
    m_memHeight = 0;
}

// === Shell ===
// Управление треем
void App::MinimizeToTray() {
    if (!m_trayAdded) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = m_hWnd;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = (HICON)LoadImageW(m_hInst, MAKEINTRESOURCE(IDI_SYSIM),
            IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
        if (!nid.hIcon) {
            nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        }
        wcscpy_s(nid.szTip, L"SYSIM Utility");
        if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
            DWORD err = GetLastError();
            wchar_t buf[256];
            wsprintfW(buf, L"Shell_NotifyIcon failed: %lu", err);
            MessageBoxW(m_hWnd, buf, L"Tray Error", MB_OK);
            return;
        }
        m_trayAdded = true;
    }
    ShowWindow(m_hWnd, SW_HIDE);
}

void App::RestoreFromTray() {
    ShowWindow(m_hWnd, SW_SHOW);
    ShowWindow(m_hWnd, SW_RESTORE);
    SetForegroundWindow(m_hWnd);
}

void App::Quit() {
    if (m_trayAdded) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = m_hWnd;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        m_trayAdded = false;
    }
    ReleaseBackBuffer();
    if (m_hWnd) {
        DestroyWindow(m_hWnd);
    }
}