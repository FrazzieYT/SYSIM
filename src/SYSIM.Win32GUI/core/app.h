#pragma once
#include <windows.h>

class App {
public:
    bool Init(HINSTANCE hInstance);
    int Run(int nCmdShow);

    HWND GetHWND() const { return m_hWnd; }
    HINSTANCE GetInstance() const { return m_hInst; }
    static App* Instance() { return s_pInstance; }

    // Системный трей
    void MinimizeToTray();
    void RestoreFromTray();
    void Quit();

    // Ддвойная буферизация
    void EnsureBackBuffer(int width, int height);
    void ReleaseBackBuffer();
    HDC GetBackBufferDC() const { return m_memDC; }

    static constexpr UINT WM_TRAYICON = WM_APP + 420;

private:
    HINSTANCE m_hInst = nullptr;
    HWND m_hWnd = nullptr;
    bool m_trayAdded = false;
    static App* s_pInstance;

    // Back buffer
    HDC m_memDC = nullptr;
    HBITMAP m_memBitmap = nullptr;
    HBITMAP m_oldBitmap = nullptr;
    int m_memWidth = 0;
    int m_memHeight = 0;
};