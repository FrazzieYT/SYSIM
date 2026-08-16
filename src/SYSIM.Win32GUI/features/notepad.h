#pragma once
#include <windows.h>
#include <gdiplus.h>
#include <string>

using namespace Gdiplus;

namespace Notepad {

    constexpr UINT WM_NP_APPEND = WM_APP + 410;

    void EnsureUI(HWND parent);
    void Show(const RectF& area);
    void Hide();
    bool IsActive();

    void Draw(Graphics& g, const RectF& area, Font& contentFont);
    bool OnClick(int x, int y, const RectF& area);
    bool OnMouseMove(int x, int y, const RectF& area);
    void OnLButtonUp();

    void NewDoc();
    void OpenFile(const std::wstring& path);

    void AppendOutput(wchar_t* buffer);
    void OnEditChanged();
    HBRUSH GetDarkBrush();
}