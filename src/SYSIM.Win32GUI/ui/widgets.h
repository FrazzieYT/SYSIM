#pragma once
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <functional>

using namespace Gdiplus;

struct Button {
    RectF rect;
    std::wstring text;
    std::function<void()> onClick;
    bool HitTest(float x, float y) const {
        return x >= rect.X && x <= rect.X + rect.Width &&
            y >= rect.Y && y <= rect.Y + rect.Height;
    }
};

struct Tab {
    std::wstring title;
    RectF bounds;
};

bool HitTestRect(const RectF& rect, float x, float y);

// Рисование горизонтальных вкладок
void DrawTabs(Graphics& g, const RectF& clientRect, Font& tabFont, std::vector<Tab>& tabs, int activeTab);
void DrawWindowButtons(Graphics& g, const RectF& clientRect, Font& font);
void DrawSidebar(Graphics& g, const RectF& clientRect, Font& font,
    const std::vector<std::wstring>& mainTabs, int activeMainTab);
void DrawSubTabs(Graphics& g, const RectF& contentArea, Font& font,
    const std::vector<std::wstring>& subTabs, int activeSubTab);