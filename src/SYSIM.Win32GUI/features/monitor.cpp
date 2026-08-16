#include "monitor.h"
#include "core/globals.h"
#include "core/app.h"
#include "utils/history/system_monitor.h"
#include <commdlg.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <sstream>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
using namespace Gdiplus;

static const Color CLR_BG(255, 24, 24, 24);
static const Color CLR_CARD(255, 32, 32, 32);
static const Color CLR_LINE(255, 48, 48, 48);
static const Color CLR_ACCENT(255, 0, 188, 212);
static const Color CLR_ACCENT_DIM(255, 0, 140, 160);
static const Color CLR_TEXT(255, 224, 224, 224);
static const Color CLR_TEXT_DIM(255, 140, 140, 140);
static const Color CLR_DANGER(255, 244, 67, 54);
static const Color CLR_SUCCESS(255, 76, 175, 80);

static const float ROW_H = 20.0f;
static const float BTN_H = 28.0f;
static const float PILL_H = 22.0f;
static const float PAD = 12.0f;

static RectF g_btnPickRun, g_btnStart, g_btnClear, g_btnSave, g_btnCopy;
static RectF g_pillFiles, g_pillReg, g_pillProc, g_pillSvc, g_pillNet, g_pillSysmon;
static RectF g_listRect;
static int g_offsetY = 0;
static int g_rowCount = 0;
static int g_selectedRow = -1;
static bool g_optFiles = true, g_optReg = true, g_optProc = true, g_optSvc = true, g_optNet = true, g_optSysmon = false;
static int g_monScrollY = 0;
static int g_monMaxScroll = 0;

static float g_lastTotalH = 0.0f;
static float g_lastListH = 0.0f;
static bool  g_draggingThumb = false;
static int   g_dragStartMouseY = 0;
static int   g_dragStartScroll = 0;

enum { MC_COPY_LINE = 1, MC_COPY_PATH, MC_COPY_ALL, MC_TRACK_PROCESS };

// ==================== COLORS ====================
static Color ColorForType(ActivityMonitor::EventType t) {
    using T = ActivityMonitor::EventType;
    switch (t) {
    case T::FileCreated:     return Color(255, 76, 175, 80);
    case T::FileDeleted:     return Color(255, 244, 67, 54);
    case T::FileRenamed:     return Color(255, 33, 150, 243);
    case T::FolderCreated:   return Color(255, 139, 195, 74);
    case T::FolderDeleted:   return Color(255, 255, 87, 34);
    case T::RegistryChanged: return Color(255, 156, 39, 176);
    case T::ProcessStarted:  return Color(255, 0, 188, 212);
    case T::ProcessStopped:  return Color(255, 121, 85, 72);
    case T::ServiceStarted:  return Color(255, 255, 193, 7);
    case T::ServiceStopped:  return Color(255, 255, 152, 0);
    case T::NetConnect:      return Color(255, 0, 255, 156);
    }
    return CLR_TEXT_DIM;
}

// ==================== HELPERS ====================
static bool Hit(const RectF& r, float x, float y) {
    return x >= r.X && x < r.X + r.Width && y >= r.Y && y < r.Y + r.Height;
}

static void CopyToClipboard(const std::wstring& text) {
    if (!OpenClipboard(App::Instance()->GetHWND())) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (h) {
        wchar_t* p = (wchar_t*)GlobalLock(h);
        if (p) {
            wcscpy_s(p, text.size() + 1, text.c_str());
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
    }
    CloseClipboard();
}

static int RowAt(int x, int y) {
    float fx = (float)x, fy = (float)y;
    if (!Hit(g_listRect, fx, fy)) return -1;
    int idx = (int)((fy - g_listRect.Y + g_offsetY) / ROW_H);
    if (idx < 0 || idx >= g_rowCount) return -1;
    return idx;
}

// ==================== DRAW PRIMITIVES ====================
static void DrawRoundedRect(Graphics& g, const RectF& r, const Color& fill, float radius = 5.0f) {
    SolidBrush brush(fill);
    g.FillRectangle(&brush, RectF(r.X + radius, r.Y, r.Width - 2 * radius, r.Height));
    g.FillRectangle(&brush, RectF(r.X, r.Y + radius, r.Width, r.Height - 2 * radius));
    g.FillEllipse(&brush, RectF(r.X, r.Y, 2 * radius, 2 * radius));
    g.FillEllipse(&brush, RectF(r.X + r.Width - 2 * radius, r.Y, 2 * radius, 2 * radius));
    g.FillEllipse(&brush, RectF(r.X, r.Y + r.Height - 2 * radius, 2 * radius, 2 * radius));
    g.FillEllipse(&brush, RectF(r.X + r.Width - 2 * radius, r.Y + r.Height - 2 * radius, 2 * radius, 2 * radius));
}

static void DrawButton(Graphics& g, const RectF& r, const wchar_t* label, Font& f,
    const Color& bg, const Color& text, bool primary = false) {
    DrawRoundedRect(g, r, bg, 6.0f);
    if (primary) {
        Pen pen(CLR_ACCENT, 1.0f);
        g.DrawRectangle(&pen, r);
    }
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush brush(text);
    g.DrawString(label, -1, &f, r, &sf, &brush);
}

static void DrawPill(Graphics& g, const RectF& r, bool active, const wchar_t* label, Font& f) {
    Color bg = active ? CLR_ACCENT : CLR_CARD;
    Color text = active ? Color(255, 20, 20, 20) : CLR_TEXT_DIM;
    DrawRoundedRect(g, r, bg, 10.0f);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush brush(text);
    g.DrawString(label, -1, &f, r, &sf, &brush);
}

static void DrawLabel(Graphics& g, const wchar_t* text, float x, float y, Font& font, const Color& color) {
    SolidBrush brush(color);
    g.DrawString(text, -1, &font, PointF(x, y), &brush);
}

// ==================== SCROLLBAR ====================
static bool GetTrackRect(const RectF& listRect, RectF& outTrack) {
    if (g_lastTotalH <= g_lastListH) return false;
    float trackW = 6.0f;
    float trackX = listRect.X + listRect.Width - trackW - 3.0f;
    outTrack = RectF(trackX, listRect.Y + 4.0f, trackW, listRect.Height - 8.0f);
    return true;
}

static bool GetThumbRect(const RectF& listRect, RectF& outThumb) {
    if (g_lastTotalH <= g_lastListH) return false;
    RectF track;
    if (!GetTrackRect(listRect, track)) return false;
    float ratio = g_lastListH / g_lastTotalH;
    float thumbH = track.Height * ratio;
    if (thumbH < 24.0f) thumbH = 24.0f;
    float maxScroll = g_lastTotalH - g_lastListH;
    float t = (maxScroll > 0.0f) ? (float)g_monScrollY / maxScroll : 0.0f;
    float thumbY = track.Y + (track.Height - thumbH) * t;
    outThumb = RectF(track.X - 4.0f, thumbY, track.Width + 8.0f, thumbH);
    return true;
}

static void DrawScrollBar(Graphics& g, const RectF& listRect, float totalH, float visibleH, int scrollY) {
    (void)totalH; (void)visibleH; (void)scrollY;
    RectF track;
    if (!GetTrackRect(listRect, track)) return;

    SolidBrush trackBrush(Color(60, 80, 80, 80));
    g.FillRectangle(&trackBrush, track);

    RectF thumb;
    if (!GetThumbRect(listRect, thumb)) return;
    RectF thumbDraw(track.X, thumb.Y, track.Width, thumb.Height);
    SolidBrush thumbBrush(Color(200, 140, 140, 140));
    g.FillRectangle(&thumbBrush, thumbDraw);
}

// ==================== PICK + RUN + AUTO-TRACK ====================
static bool PickRunAndTrack() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = App::Instance()->GetHWND();
    ofn.lpstrFilter = L"Программы (*.exe)\0*.exe\0Все файлы (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(file, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        MessageBoxW(App::Instance()->GetHWND(), L"Не удалось запустить файл", L"Ошибка", MB_OK | MB_ICONERROR);
        return false;
    }
    CloseHandle(pi.hThread);

    std::wstring path = file;
    auto pos = path.find_last_of(L"\\/");
    std::wstring name = (pos != std::wstring::npos) ? path.substr(pos + 1) : path;

    ActivityMonitor::StartTrackingProcess(pi.dwProcessId, name);
    CloseHandle(pi.hProcess);

    if (!ActivityMonitor::IsRunning()) {
        ActivityMonitor::Start(g_optFiles, g_optReg, g_optProc, g_optSvc, g_optNet, g_optSysmon);
    }
    return true;
}

// ==================== DRAW ====================
void DrawMonitorContent(Graphics& g, const RectF& contentArea, Font& contentFont) {
    (void)contentFont;
    SolidBrush bgBrush(CLR_BG);
    g.FillRectangle(&bgBrush, contentArea);

    float x = contentArea.X + PAD;
    float y = contentArea.Y + PAD;
    float fullW = contentArea.Width - PAD * 2.0f;

    extern std::wstring g_fontFamilyName;
    const wchar_t* famName = g_fontFamilyName.empty() ? L"Segoe UI" : g_fontFamilyName.c_str();
    FontFamily ff(famName);
    Font font(&ff, 10.0f, FontStyleRegular, UnitPixel);
    Font fontSmall(&ff, 9.0f, FontStyleRegular, UnitPixel);

    const bool running = ActivityMonitor::IsRunning();
    const bool isTracking = ActivityMonitor::IsTrackingProcess();
    const bool hasSysmon = ActivityMonitor::IsSysmonAvailable();
    const auto stats = ActivityMonitor::GetStats();

    // === Строка 1: Кнопки ===
    float btnY = y;
    float gap = 6.0f;
    float bx = x;
    auto placeBtn = [&](RectF& r, float w) { r = RectF(bx, btnY, w, BTN_H); bx += w + gap; };

    placeBtn(g_btnPickRun, 170.0f);
    placeBtn(g_btnStart, 90.0f);
    placeBtn(g_btnClear, 84.0f);
    placeBtn(g_btnSave, 90.0f);
    placeBtn(g_btnCopy, 96.0f);

    DrawButton(g, g_btnPickRun, L"Выбрать файл и следить", fontSmall, CLR_ACCENT_DIM, CLR_TEXT, true);
    DrawButton(g, g_btnStart, running ? L"Стоп" : L"Старт", fontSmall,
        running ? CLR_DANGER : CLR_ACCENT, Color(255, 20, 20, 20), true);
    DrawButton(g, g_btnClear, L"Очистить", fontSmall, CLR_CARD, CLR_TEXT);
    DrawButton(g, g_btnSave, L"Сохранить", fontSmall, CLR_CARD, CLR_TEXT);
    DrawButton(g, g_btnCopy, L"Копировать", fontSmall, CLR_CARD, CLR_TEXT);

    // === Строка 2: Пиллы + счётчики + статус ===
    float pillY = btnY + BTN_H + 8.0f;
    float px = x;
    auto placePill = [&](RectF& r, bool active, const wchar_t* label, float w) {
        r = RectF(px, pillY, w, PILL_H);
        DrawPill(g, r, active, label, fontSmall);
        px += w + 5.0f;
        };
    placePill(g_pillFiles, g_optFiles, L"Файлы", 64.0f);
    placePill(g_pillReg, g_optReg, L"Реестр", 68.0f);
    placePill(g_pillProc, g_optProc, L"Процессы", 82.0f);
    placePill(g_pillSvc, g_optSvc, L"Службы", 68.0f);
    placePill(g_pillNet, g_optNet, L"Сеть", 56.0f);
    placePill(g_pillSysmon, g_optSysmon, L"Sysmon", 68.0f);

    wchar_t rightBuf[512];
    swprintf_s(rightBuf, L"%s  ·  Sysmon: %s  ·  F %d  R %d  P %d  S %d  N %d",
        running ? L"●" : L"○",
        hasSysmon ? L"ok" : L"-",
        stats.files, stats.registry, stats.processes, stats.services, stats.network);

    float rightWidth = 340.0f;
    StringFormat rf;
    rf.SetAlignment(StringAlignmentFar);
    rf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush rightBrush(CLR_TEXT_DIM);
    g.DrawString(rightBuf, -1, &fontSmall, RectF(x + fullW - rightWidth, pillY, rightWidth, PILL_H),
        &rf, &rightBrush);

    // === Строка 3: Слежка ===
    float listY = pillY + PILL_H + 8.0f;
    if (isTracking) {
        wchar_t trackBuf[512];
        swprintf_s(trackBuf, L"Слежка: %s (PID %lu)",
            ActivityMonitor::GetTrackedRootName().c_str(),
            (unsigned long)ActivityMonitor::GetTrackedRootPid());
        DrawLabel(g, trackBuf, x, listY, fontSmall, CLR_ACCENT);
        listY += 14.0f;
    }

    // === Строка 4: Список ===
    float listH = contentArea.Y + contentArea.Height - listY - PAD;
    if (listH < 30.0f) return;

    auto events = ActivityMonitor::GetEvents();
    std::vector<const ActivityMonitor::Event*> filtered;
    filtered.reserve(events.size());
    for (const auto& ev : events) {
        using T = ActivityMonitor::EventType;
        bool show = false;
        switch (ev.type) {
        case T::FileCreated: case T::FileDeleted: case T::FileRenamed:
        case T::FolderCreated: case T::FolderDeleted: show = g_optFiles; break;
        case T::RegistryChanged: show = g_optReg; break;
        case T::ProcessStarted: case T::ProcessStopped: show = g_optProc; break;
        case T::ServiceStarted: case T::ServiceStopped: show = g_optSvc; break;
        case T::NetConnect: show = g_optNet; break;
        }
        if (show) filtered.push_back(&ev);
    }

    g_rowCount = (int)filtered.size();
    float totalH = (float)filtered.size() * ROW_H;
    g_lastTotalH = totalH;
    g_lastListH = listH;

    g_monMaxScroll = (totalH > listH) ? (int)(totalH - listH) : 0;
    if (g_monScrollY < 0) g_monScrollY = 0;
    if (g_monScrollY > g_monMaxScroll) g_monScrollY = g_monMaxScroll;
    g_offsetY = g_monScrollY;
    if (g_selectedRow >= g_rowCount) g_selectedRow = -1;

    g_listRect = RectF(x, listY, fullW, listH);
    DrawRoundedRect(g, g_listRect, CLR_CARD, 6.0f);

    if (filtered.empty()) {
        const wchar_t* emptyText = running
            ? L"- нет событий -"
            : L"- мониторинг остановлен -";
        StringFormat cf;
        cf.SetAlignment(StringAlignmentCenter);
        cf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush emptyBrush(Color(255, 90, 90, 90));
        Font emptyFont(&ff, 11.0f, FontStyleItalic, UnitPixel);
        g.DrawString(emptyText, -1, &emptyFont, g_listRect, &cf, &emptyBrush);
    }
    else {
        int startRow = g_offsetY / (int)ROW_H;
        int maxRows = (int)(listH / ROW_H) + 1;
        int endRow = (std::min)(startRow + maxRows, (int)filtered.size());

        SolidBrush suspiciousBrush(CLR_DANGER);
        SolidBrush normalTextBrush(CLR_TEXT);
        for (int i = startRow; i < endRow; ++i) {
            float yPos = listY + i * ROW_H - (float)g_offsetY;
            if (yPos + ROW_H < listY || yPos > listY + listH) continue;
            const auto& ev = *filtered[i];

            if (i == g_selectedRow) {
                SolidBrush selBg(Color(40, 0, 188, 212));
                g.FillRectangle(&selBg, RectF(x + 2.0f, yPos, fullW - 4.0f, ROW_H));
            }

            SolidBrush dotBrush(ColorForType(ev.type));
            g.FillEllipse(&dotBrush, RectF(x + 8.0f, yPos + 6.0f, 8.0f, 8.0f));

            std::wstring line = ActivityMonitor::EventToString(ev);
            const Brush* textBrush = ev.suspicious ? (const Brush*)&suspiciousBrush : (const Brush*)&normalTextBrush;
            g.DrawString(line.c_str(), -1, &fontSmall, PointF(x + 22.0f, yPos + 3.0f), textBrush);
        }

        DrawScrollBar(g, g_listRect, totalH, listH, g_monScrollY);
    }
}

// ==================== INPUT ====================
bool OnMonitorClick(int x, int y, const RectF& contentArea) {
    (void)contentArea;
    float fx = (float)x, fy = (float)y;

    if (Hit(g_btnPickRun, fx, fy)) {
        PickRunAndTrack();
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }
    if (Hit(g_btnStart, fx, fy)) {
        if (ActivityMonitor::IsRunning()) ActivityMonitor::Stop();
        else ActivityMonitor::Start(g_optFiles, g_optReg, g_optProc, g_optSvc, g_optNet, g_optSysmon);
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }
    if (Hit(g_btnClear, fx, fy)) {
        ActivityMonitor::Clear();
        g_selectedRow = -1;
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }
    if (Hit(g_btnSave, fx, fy)) {
        wchar_t file[MAX_PATH] = L"SYSIM_report.txt";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = App::Instance()->GetHWND();
        ofn.lpstrFilter = L"Отчёт (*.txt)\0*.txt\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT;
        if (GetSaveFileNameW(&ofn)) ActivityMonitor::SaveReport(file);
        return true;
    }
    if (Hit(g_btnCopy, fx, fy)) {
        std::wstring all;
        for (const auto& ev : ActivityMonitor::GetEvents())
            all += ActivityMonitor::EventToString(ev) + L"\r\n";
        CopyToClipboard(all);
        return true;
    }

    if (!ActivityMonitor::IsRunning()) {
        if (Hit(g_pillFiles, fx, fy)) { g_optFiles = !g_optFiles; InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE); return true; }
        if (Hit(g_pillReg, fx, fy)) { g_optReg = !g_optReg; InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE); return true; }
        if (Hit(g_pillProc, fx, fy)) { g_optProc = !g_optProc; InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE); return true; }
        if (Hit(g_pillSvc, fx, fy)) { g_optSvc = !g_optSvc; InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE); return true; }
        if (Hit(g_pillNet, fx, fy)) { g_optNet = !g_optNet; InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE); return true; }
        if (Hit(g_pillSysmon, fx, fy)) {
            g_optSysmon = !g_optSysmon;
            InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
            return true;
        }
    }

    {
        RectF thumb;
        if (GetThumbRect(g_listRect, thumb) && Hit(thumb, fx, fy)) {
            g_draggingThumb = true;
            g_dragStartMouseY = y;
            g_dragStartScroll = g_monScrollY;
            SetCapture(App::Instance()->GetHWND());
            return true;
        }
        RectF track;
        if (GetTrackRect(g_listRect, track) && Hit(track, fx, fy)) {
            RectF thumb2;
            if (GetThumbRect(g_listRect, thumb2)) {
                int page = (int)g_lastListH;
                if (fy < thumb2.Y) g_monScrollY -= page;
                else if (fy > thumb2.Y + thumb2.Height) g_monScrollY += page;
                if (g_monScrollY < 0) g_monScrollY = 0;
                if (g_monScrollY > g_monMaxScroll) g_monScrollY = g_monMaxScroll;
                InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
                return true;
            }
        }
    }

    int row = RowAt(x, y);
    if (row >= 0) {
        g_selectedRow = row;
        InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        return true;
    }
    return false;
}

bool OnMonitorRightClick(int x, int y, const RectF& contentArea) {
    (void)contentArea;
    auto events = ActivityMonitor::GetEvents();
    int row = RowAt(x, y);
    HMENU menu = CreatePopupMenu();
    if (row >= 0 && row < (int)events.size()) {
        AppendMenuW(menu, MF_STRING, MC_COPY_LINE, L"Копировать строку");
        AppendMenuW(menu, MF_STRING, MC_COPY_PATH, L"Копировать путь");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, MC_TRACK_PROCESS, L"Следить за этим процессом");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, MC_COPY_ALL, L"Копировать всё");

    POINT pt{ x, y };
    ClientToScreen(App::Instance()->GetHWND(), &pt);
    SetForegroundWindow(App::Instance()->GetHWND());
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        pt.x, pt.y, 0, App::Instance()->GetHWND(), nullptr);
    DestroyMenu(menu);

    if (cmd == MC_COPY_LINE && row >= 0 && row < (int)events.size()) {
        CopyToClipboard(ActivityMonitor::EventToString(events[row]));
    }
    else if (cmd == MC_COPY_PATH && row >= 0 && row < (int)events.size()) {
        CopyToClipboard(events[row].path);
    }
    else if (cmd == MC_COPY_ALL) {
        std::wstring all;
        for (const auto& ev : events) all += ActivityMonitor::EventToString(ev) + L"\r\n";
        CopyToClipboard(all);
    }
    else if (cmd == MC_TRACK_PROCESS && row >= 0 && row < (int)events.size()) {
        DWORD targetPid = events[row].pid;
        if (targetPid == 0) {
            size_t pidPos = events[row].extra.find(L"PID ");
            if (pidPos != std::wstring::npos) {
                try { targetPid = (DWORD)std::stoul(events[row].extra.substr(pidPos + 4)); }
                catch (...) {}
            }
        }
        if (targetPid > 0) {
            ActivityMonitor::StartTrackingProcess(targetPid, events[row].path);
            if (!ActivityMonitor::IsRunning()) {
                ActivityMonitor::Start(g_optFiles, g_optReg, g_optProc, g_optSvc, g_optNet, g_optSysmon);
            }
            InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
        }
    }
    return true;
}

bool OnMonitorDblClick(int x, int y, const RectF& contentArea) {
    (void)contentArea;
    auto events = ActivityMonitor::GetEvents();
    int row = RowAt(x, y);
    if (row >= 0 && row < (int)events.size()) {
        CopyToClipboard(events[row].path);
        return true;
    }
    return false;
}

// ==================== SCROLL INPUT ====================
bool OnMonitorWheel(int x, int y, int delta) {
    float fx = (float)x, fy = (float)y;
    if (!Hit(g_listRect, fx, fy)) return false;
    if (g_monMaxScroll <= 0) return true;

    int notches = delta / WHEEL_DELTA;
    if (notches == 0) notches = (delta > 0) ? 1 : -1;
    g_monScrollY -= notches * 60;
    if (g_monScrollY < 0) g_monScrollY = 0;
    if (g_monScrollY > g_monMaxScroll) g_monScrollY = g_monMaxScroll;
    InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
    return true;
}

bool OnMonitorMouseMove(int x, int y) {
    if (!g_draggingThumb) return false;

    RectF track, thumb;
    if (!GetTrackRect(g_listRect, track)) return true;
    if (!GetThumbRect(g_listRect, thumb)) return true;

    float thumbH = thumb.Height;
    float maxScroll = g_lastTotalH - g_lastListH;
    if (maxScroll <= 0.0f) return true;

    float trackRange = track.Height - thumbH;
    if (trackRange <= 0.0f) return true;

    int dy = y - g_dragStartMouseY;
    float scrollDelta = (float)dy * (maxScroll / trackRange);
    g_monScrollY = g_dragStartScroll + (int)scrollDelta;

    if (g_monScrollY < 0) g_monScrollY = 0;
    if (g_monScrollY > g_monMaxScroll) g_monScrollY = g_monMaxScroll;

    InvalidateRect(App::Instance()->GetHWND(), nullptr, TRUE);
    return true;
}

bool OnMonitorMouseUp(int x, int y) {
    (void)x; (void)y;
    if (!g_draggingThumb) return false;
    g_draggingThumb = false;
    ReleaseCapture();
    return true;
}