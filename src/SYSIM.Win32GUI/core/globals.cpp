#include "globals.h"
#include "app.h"

// ===== НАВИГАЦИЯ =====
std::vector<std::wstring> g_mainTabs = {
    L"Главная",          // 0
    L"Диспетчер задач",  // 1
    L"Проводник",        // 2
    L"Реестр",           // 3
    L"Разблокировка",    // 4
    L"Слежка",           // 5
    L"Учётные записи",   // 6
    L"Блокнот",          // 7
    L"Настройки",        // 8
    L"Автозагрузка",     // 9
    L"Кейбинды"          // 10
};
int g_activeMainTab = 0;
int g_previousMainTab = 0;

int g_activeSubTab = 0;
int g_activeTab = 0;

bool g_draggingFromTab = false;
POINT g_dragStartPoint = {};
int g_dragTabIndex = -1;

bool g_subTabsExpanded = true;
RectF g_subTabsToggleRect;

// ===== ГОРИЗОНТАЛЬНЫЕ ТАБЫ =====
RectF g_horizontalTabRects[20] = {};
int g_horizontalTabHover = -1;

// ===== СКРОЛЛ КОНТЕНТА =====
int g_scrollOffset[20] = { 0 };
int g_maxScroll[20] = { 0 };
int g_scrollBarWidth = 16;
bool g_scrollBarDragging = false;
int g_scrollBarDragStartY = 0;
int g_scrollBarDragStartOffset = 0;

std::array<Gdiplus::RectF, 20> g_sidebarTabRects{};
std::array<Gdiplus::RectF, 20> g_subTabRects{};

// ===== КНОПКИ УПРАВЛЕНИЯ ОКНОМ =====
RectF g_tabBtnSettings;
RectF g_tabBtnMinimize;
RectF g_tabBtnClose;
bool g_tabBtnSettingsHover = false;
bool g_tabBtnMinimizeHover = false;
bool g_tabBtnCloseHover = false;

// ===== ДОПОЛНИТЕЛЬНЫЕ КНОПКИ =====
RectF g_settingsButtonRect;
RectF g_notepadButtonRect;

// ===== ТЕМА =====
std::wstring g_embeddedFontFamilyName = L"Segoe UI";
std::wstring g_fontFamilyName = L"Segoe UI";
bool g_useSystemFonts = true;
const Color COLOR_BG(255, 24, 24, 24);
const Color COLOR_HEADER_BG(255, 32, 32, 32);
const Color COLOR_TAB_BG(255, 34, 34, 34);
const Color COLOR_TAB_ACTIVE(255, 0, 120, 212);
const Color COLOR_TAB_HOVER(255, 45, 45, 48);
const Color COLOR_TEXT(255, 235, 235, 235);
const Color COLOR_TEXT_MUTED(255, 180, 180, 180);
const Color COLOR_BORDER(255, 70, 70, 70);
const Color COLOR_BUTTON_BG(255, 50, 50, 50);

bool g_blockProcessLaunches = false;
bool g_blockSystemHotkeys = false;
bool g_keepWindowOnTop = false;

std::vector<SystemKeybindEntry> g_systemKeybindCatalog = {
    { L"Системные", L"Win", L"Пуск" },
    { L"Системные", L"Win + A", L"Быстрые настройки" },
    { L"Системные", L"Win + B", L"Фокус на область уведомлений" },
    { L"Системные", L"Win + C", L"Copilot / соответствующая функция Windows" },
    { L"Системные", L"Win + D", L"Показать/скрыть рабочий стол" },
    { L"Системные", L"Win + E", L"Проводник" },
    { L"Системные", L"Win + F", L"Feedback Hub" },
    { L"Системные", L"Win + G", L"Xbox Game Bar" },
    { L"Системные", L"Win + H", L"Голосовой ввод" },
    { L"Системные", L"Win + I", L"Параметры" },
    { L"Системные", L"Win + K", L"Подключение дисплея/устройств" },
    { L"Системные", L"Win + L", L"Заблокировать ПК" },
    { L"Системные", L"Win + M", L"Свернуть все окна" },
    { L"Системные", L"Win + N", L"Уведомления/календарь" },
    { L"Системные", L"Win + P", L"Режим проецирования" },
    { L"Системные", L"Win + R", L"Выполнить" },
    { L"Системные", L"Win + S", L"Поиск" },
    { L"Системные", L"Win + U", L"Специальные возможности" },
    { L"Системные", L"Win + V", L"История буфера обмена" },
    { L"Системные", L"Win + W", L"Виджеты" },
    { L"Системные", L"Win + X", L"Меню быстрого доступа" },
    { L"Системные", L"Win + Z", L"Snap Layouts" },
    { L"Системные", L"Win + Tab", L"Представление задач" },
    { L"Системные", L"Win + Space", L"Смена раскладки" },
    { L"Системные", L"Win + Ctrl + D", L"Новый виртуальный рабочий стол" },
    { L"Системные", L"Win + Ctrl + ←/→", L"Переключить рабочий стол" },
    { L"Системные", L"Win + Ctrl + F4", L"Закрыть рабочий стол" },
    { L"Системные", L"Win + Shift + S", L"Область снимка экрана" },
    { L"Системные", L"Win + Shift + V", L"Источник ввода" },
    { L"Системные", L"Win + Shift + M", L"Восстановить свернутые окна" },
    { L"Управление окнами", L"Win + ↑", L"Развернуть окно" },
    { L"Управление окнами", L"Win + ↓", L"Восстановить/свернуть" },
    { L"Управление окнами", L"Win + ←", L"Прикрепить слева" },
    { L"Управление окнами", L"Win + →", L"Прикрепить справа" },
    { L"Управление окнами", L"Win + Home", L"Свернуть остальные окна" },
    { L"Управление окнами", L"Win + Shift + ↑", L"Растянуть окно вертикально" },
    { L"Управление окнами", L"Win + Shift + ←", L"Переместить окно на монитор слева" },
    { L"Управление окнами", L"Win + Shift + →", L"Переместить окно на монитор справа" },
    { L"Управление окнами", L"Alt + Tab", L"Переключение окон" },
    { L"Управление окнами", L"Alt + Shift + Tab", L"Переключение назад" },
    { L"Управление окнами", L"Alt + F4", L"Закрыть окно" },
    { L"Управление окнами", L"Alt + Space", L"Системное меню окна" },
    { L"Управление окнами", L"Ctrl + Shift + Esc", L"Диспетчер задач" },
    { L"Управление окнами", L"Ctrl + Alt + Del", L"Экран безопасности Windows" },
    { L"Проводник", L"Win + E", L"Открыть Проводник" },
    { L"Проводник", L"Ctrl + N", L"Новое окно" },
    { L"Проводник", L"Ctrl + Shift + N", L"Новая папка" },
    { L"Проводник", L"Ctrl + W", L"Закрыть окно/вкладку" },
    { L"Проводник", L"Ctrl + L", L"Адресная строка" },
    { L"Проводник", L"Alt + D", L"Адресная строка" },
    { L"Проводник", L"Alt + ←", L"Назад" },
    { L"Проводник", L"Alt + →", L"Вперед" },
    { L"Проводник", L"Alt + ↑", L"Родительская папка" },
    { L"Проводник", L"F2", L"Переименовать" },
    { L"Проводник", L"F5", L"Обновить" },
    { L"Проводник", L"Delete", L"Удалить" },
    { L"Проводник", L"Shift + Delete", L"Удалить без корзины" },
    { L"Проводник", L"Ctrl + A", L"Выделить всё" },
    { L"Проводник", L"Ctrl + C", L"Копировать" },
    { L"Проводник", L"Ctrl + X", L"Вырезать" },
    { L"Проводник", L"Ctrl + V", L"Вставить" },
    { L"Проводник", L"Alt + Enter", L"Свойства" },
    { L"Текст", L"Ctrl + A", L"Выделить всё" },
    { L"Текст", L"Ctrl + C", L"Копировать" },
    { L"Текст", L"Ctrl + X", L"Вырезать" },
    { L"Текст", L"Ctrl + V", L"Вставить" },
    { L"Текст", L"Ctrl + Z", L"Отменить" },
    { L"Текст", L"Ctrl + Y", L"Повторить" },
    { L"Текст", L"Ctrl + F", L"Найти" },
    { L"Текст", L"Ctrl + H", L"Заменить" },
    { L"Текст", L"Home", L"В начало строки" },
    { L"Текст", L"End", L"В конец строки" },
    { L"Текст", L"Ctrl + ←/→", L"Перейти на слово" },
    { L"Текст", L"Ctrl + Backspace", L"Удалить слово слева" },
    { L"Текст", L"Ctrl + Delete", L"Удалить слово справа" },
    { L"Текст", L"Shift + ←/→", L"Выделение символов" },
    { L"Текст", L"Ctrl + Shift + ←/→", L"Выделение по словам" },
    { L"Терминал / консоль", L"Win + R", L"Открыть «Выполнить»" },
    { L"Терминал / консоль", L"Win + X", L"Быстрое системное меню" },
    { L"Терминал / консоль", L"Ctrl + Shift + Enter", L"Запуск команды от администратора через «Выполнить»" },
    { L"Терминал / консоль", L"Ctrl + Shift + Esc", L"Диспетчер задач" },
    { L"Терминал / консоль", L"Ctrl + Shift + C", L"Копировать выделенный текст в Windows Terminal" },
    { L"Терминал / консоль", L"Ctrl + Shift + V", L"Вставить в Windows Terminal" },
    { L"Терминал / консоль", L"Ctrl + Shift + F", L"Поиск в Windows Terminal" },
    { L"Терминал / консоль", L"Alt + Shift + +", L"Новая вкладка/профиль в некоторых конфигурациях Terminal" },
    { L"Скриншоты", L"PrtSc", L"Снимок экрана" },
    { L"Скриншоты", L"Win + PrtSc", L"Сохранить снимок экрана" },
    { L"Скриншоты", L"Win + Shift + S", L"Выбрать область" },
    { L"Скриншоты", L"Alt + PrtSc", L"Снимок активного окна" },
    { L"Скриншоты", L"Win + G", L"Game Bar" },
    { L"Специальные", L"Win + Ctrl + Shift + B", L"Перезапустить графический драйвер" },
    { L"Специальные", L"Win + Ctrl + Enter", L"Экранный диктор" },
    { L"Специальные", L"Win + +", L"Экранная лупа" },
    { L"Специальные", L"Win + -", L"Уменьшить масштаб лупы" },
    { L"Специальные", L"Win + Esc", L"Закрыть лупу" },
    { L"Специальные", L"Win + Ctrl + O", L"Экранная клавиатура" },
    { L"Специальные", L"Win + U", L"Специальные возможности" }
};

bool IsProcessLaunchBlocked(HWND owner) {
    if (!g_blockProcessLaunches) return false;
    MessageBoxW(owner,
        L"Запуск новых процессов заблокирован.",
        L"Защита запуска", MB_OK | MB_ICONWARNING);
    return true;
}

void ApplySystemHotkeyProtection() {
    HWND hwnd = App::Instance() ? App::Instance()->GetHWND() : nullptr;
    if (!hwnd) return;

    SetWindowPos(hwnd, g_keepWindowOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// ===== ПРОВОДНИК =====
std::wstring g_explorerPath = L"C:\\";
int g_explorerSelectedIndex = -1;
std::vector<int> g_explorerSelectedIndices;
std::vector<FileExplorer::FileItem> g_explorerItems;

// ===== LAYOUT =====
bool g_useVerticalLayout = true;