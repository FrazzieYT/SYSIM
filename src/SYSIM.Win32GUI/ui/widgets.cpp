#include "widgets.h"
#include "core/globals.h"

void DrawTabs(Graphics& g, const RectF& clientRect, Font& tabFont, std::vector<Tab>& tabs, int activeTab) {
    const float tabHeight = 28.0f;
    const float tabPadding = 12.0f;

    // Рассчитываем ширину вкладок, чтобы они занимали всю доступную ширину
    float availableWidth = clientRect.Width;

    std::vector<float> tabWidths;
    float totalTabsWidth = 0.0f;
    for (auto& tab : tabs) {
        RectF bounds;
        g.MeasureString(tab.title.c_str(), -1, &tabFont, PointF(0, 0), &bounds);
        float w = bounds.Width + tabPadding * 2;
        tabWidths.push_back(w);
        totalTabsWidth += w;
    }

    if (totalTabsWidth > availableWidth) {
        float tabWidth = availableWidth / (float)tabs.size();
        for (auto& w : tabWidths) w = tabWidth;
    }

    // Фон полосы вкладок
    SolidBrush tabBg(COLOR_TAB_BG);
    RectF bgRect(0, 0, clientRect.Width, tabHeight);
    g.FillRectangle(&tabBg, bgRect);
    Pen borderPen(COLOR_BORDER, 1.0f);
    g.DrawLine(&borderPen, PointF(0, tabHeight), PointF(clientRect.Width, tabHeight));

    // Рисуем вкладки
    float x = 0.0f;
    for (size_t i = 0; i < tabs.size(); ++i) {
        float w = tabWidths[i];
        RectF tabRect(x, 0, w, tabHeight);
        tabs[i].bounds = tabRect;
        Color bgColor = (i == (size_t)activeTab) ? COLOR_TAB_ACTIVE : COLOR_TAB_BG;
        SolidBrush brush(bgColor);
        g.FillRectangle(&brush, tabRect);
        if (i != (size_t)activeTab) {
            Pen borderPen2(COLOR_BORDER, 1.0f);
            g.DrawRectangle(&borderPen2, tabRect);
        }
        SolidBrush textBrush(COLOR_TEXT);
        StringFormat format;
        format.SetAlignment(StringAlignmentCenter);
        format.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(tabs[i].title.c_str(), -1, &tabFont, tabRect, &format, &textBrush);
        x += w;
    }
}

bool HitTestRect(const RectF& rect, float x, float y) {
    return x >= rect.X && x < rect.X + rect.Width &&
        y >= rect.Y && y < rect.Y + rect.Height;
}

void DrawWindowButtons(Graphics& g, const RectF& clientRect, Font& font) {
    const float btnWidth = 30.0f;
    const float btnGap = 6.0f;
    const float btnRightMargin = 8.0f;
    const float btnHeight = 28.0f - 4.0f;
    const float topOffset = 2.0f;

    // Позиционируем кнопки справа
    float startX = clientRect.Width - btnRightMargin - btnWidth; // последняя кнопка (закрытие)
    // Кнопка закрытия (✕)
    RectF closeBtn(startX, topOffset, btnWidth, btnHeight);
    g_tabBtnClose = closeBtn;
    // Кнопка свернуть (─)
    RectF minBtn(startX - btnWidth - btnGap, topOffset, btnWidth, btnHeight);
    g_tabBtnMinimize = minBtn;
    // Кнопка настроек (⚙)
    RectF setBtn(startX - 2 * (btnWidth + btnGap), topOffset, btnWidth, btnHeight);
    g_tabBtnSettings = setBtn;

    // Цвета кнопок (при наведении светлее)
    Color bgSet = g_tabBtnSettingsHover ? Color(80, 80, 80) : COLOR_BUTTON_BG;
    Color bgMin = g_tabBtnMinimizeHover ? Color(80, 80, 80) : COLOR_BUTTON_BG;
    Color bgClose = g_tabBtnCloseHover ? Color(80, 80, 80) : COLOR_BUTTON_BG;
    if (g_tabBtnCloseHover) bgClose = Color(180, 40, 40);

    SolidBrush setBrush(bgSet);
    SolidBrush minBrush(bgMin);
    SolidBrush closeBrush(bgClose);
    Pen btnBorder(COLOR_BORDER, 1.0f);
    SolidBrush textBrush(COLOR_TEXT);
    StringFormat centerFormat;
    centerFormat.SetAlignment(StringAlignmentCenter);
    centerFormat.SetLineAlignment(StringAlignmentCenter);

    // Кнопка настроек (⚙)
    g.FillRectangle(&setBrush, setBtn);
    g.DrawRectangle(&btnBorder, setBtn);
    g.DrawString(L"⚙", -1, &font, setBtn, &centerFormat, &textBrush);

    // Кнопка свернуть (─)
    g.FillRectangle(&minBrush, minBtn);
    g.DrawRectangle(&btnBorder, minBtn);
    g.DrawString(L"─", -1, &font, minBtn, &centerFormat, &textBrush);

    // Кнопка закрыть (✕)
    g.FillRectangle(&closeBrush, closeBtn);
    g.DrawRectangle(&btnBorder, closeBtn);
    g.DrawString(L"✕", -1, &font, closeBtn, &centerFormat, &textBrush);
}

void DrawSidebar(Graphics& g, const RectF& clientRect, Font& font,
    const std::vector<std::wstring>& mainTabs, int activeMainTab) {
    const float sidebarWidth = 140.0f;
    const float tabHeight = 32.0f;
    const float tabPadding = 8.0f;

    // Фон левой панели
    SolidBrush sidebarBg(Color(40, 40, 40));
    g.FillRectangle(&sidebarBg, RectF(0, 0, sidebarWidth, clientRect.Height));
    Pen borderPen(Color(70, 70, 70), 1.0f);
    g.DrawLine(&borderPen, PointF(sidebarWidth, 0), PointF(sidebarWidth, clientRect.Height));

    // Рисуем основные вкладки
    float y = tabPadding;
    StringFormat format;
    format.SetAlignment(StringAlignmentNear);
    format.SetLineAlignment(StringAlignmentCenter);

    for (size_t i = 0; i < mainTabs.size(); ++i) {
        RectF tabRect(tabPadding, y, sidebarWidth - tabPadding * 2, tabHeight);
        g_sidebarTabRects[i] = tabRect;

        Color bgColor = (i == (size_t)activeMainTab) ? COLOR_TAB_ACTIVE : Color(50, 50, 50);
        SolidBrush bgBrush(bgColor);
        g.FillRectangle(&bgBrush, tabRect);

        Pen borderPen2(COLOR_BORDER, 1.0f);
        g.DrawRectangle(&borderPen2, tabRect);

        SolidBrush textBrush(COLOR_TEXT);
        g.DrawString(mainTabs[i].c_str(), -1, &font, tabRect, &format, &textBrush);

        y += tabHeight + tabPadding;
    }

    // Кнопка настроек (шестерёнка) внизу
    float btnSize = 28.0f;
    float btnX = (sidebarWidth - btnSize) / 2.0f;
    float btnY = clientRect.Height - btnSize - 12.0f;
    RectF settingsBtn(btnX, btnY, btnSize, btnSize);
    g_settingsButtonRect = settingsBtn;

    SolidBrush settingsBg(Color(50, 50, 50));
    g.FillRectangle(&settingsBg, settingsBtn);
    Pen borderPen3(COLOR_BORDER, 1.0f);
    g.DrawRectangle(&borderPen3, settingsBtn);

    StringFormat centerFormat;
    centerFormat.SetAlignment(StringAlignmentCenter);
    centerFormat.SetLineAlignment(StringAlignmentCenter);
    FontFamily smallFF(g_fontFamilyName.c_str());
    Font smallFont(&smallFF, 18.0f, FontStyleRegular, UnitPixel);
    SolidBrush textBrush(COLOR_TEXT);
    g.DrawString(L"⚙", -1, &smallFont, settingsBtn, &centerFormat, &textBrush);
}

void DrawSubTabs(Graphics& g, const RectF& contentArea, Font& font,
    const std::vector<std::wstring>& subTabs, int activeSubTab) {
    if (subTabs.empty()) return;

    const float subTabHeight = 28.0f;
    const float padding = 4.0f;
    float x = 0.0f;

    // Фон подвкладок (тёмный)
    SolidBrush bgBrush(Color(45, 45, 45));
    g.FillRectangle(&bgBrush, RectF(contentArea.X, contentArea.Y, contentArea.Width, subTabHeight));
    Pen borderPen(COLOR_BORDER, 1.0f);
    g.DrawLine(&borderPen, PointF(contentArea.X, contentArea.Y + subTabHeight),
        PointF(contentArea.X + contentArea.Width, contentArea.Y + subTabHeight));

    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);

    for (size_t i = 0; i < subTabs.size(); ++i) {
        RectF bounds;
        g.MeasureString(subTabs[i].c_str(), -1, &font, PointF(0, 0), &bounds);
        float w = bounds.Width + padding * 2 + 10;
        RectF tabRect(contentArea.X + x, contentArea.Y, w, subTabHeight);
        g_subTabRects[i] = tabRect;

        Color bgColor = (i == (size_t)activeSubTab) ? COLOR_TAB_ACTIVE : Color(50, 50, 50);
        SolidBrush bgBrush2(bgColor);
        g.FillRectangle(&bgBrush2, tabRect);
        g.DrawRectangle(&borderPen, tabRect);

        SolidBrush textBrush(COLOR_TEXT);
        g.DrawString(subTabs[i].c_str(), -1, &font, tabRect, &format, &textBrush);

        x += w + 4.0f;
        if (x > contentArea.Width) break;
    }
}