#include "platform/main_window.h"
#include "platform/app_paths.h"
#include "platform/display_info.h"
#include "platform/hardware_info_win.h"
#include "platform/win_util.h"
#include "ui/bottleneck_view.h"
#include "ui/chrome.h"
#include "ui/overlay_view.h"
#include "render/overlay_painter.h"
#include "ui/dashboard_view.h"
#include "ui/layout.h"
#include "ui/sensors_view.h"
#include "ui/system_view.h"
#include "ui/settings_view.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>
#include <array>
#include <chrono>

namespace yawa {
namespace {

constexpr wchar_t kClassName[] = L"YawaFpsHubMain";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTickMessage = WM_APP + 2;
constexpr UINT kExportMessage = WM_APP + 3;
constexpr UINT kTrayIconId = 1;
// DWMWA_BORDER_COLOR = 34, DWMWA_COLOR_NONE = 0xFFFFFFFE. На Windows 10 атрибут
// неизвестен, вызов просто вернёт ошибку — там рамку убирает растянутая рамка DWM.
constexpr DWORD kDwmBorderColor = 34;
constexpr COLORREF kDwmColorNone = 0xFFFFFFFE;

void removeDwmBorder(HWND window)
{
    DwmSetWindowAttribute(window, kDwmBorderColor, &kDwmColorNone, sizeof(kDwmColorNone));
}
// Идентификаторы WM_HOTKEY: RegisterHotKey принимает int, поэтому и константы int.
constexpr int kHotkeyOverlay = 1;
constexpr int kHotkeyPosition = 2;
constexpr int kHotkeyWindow = 3;
constexpr int kHotkeyClickThrough = 4;

} // namespace

MainWindow::~MainWindow()
{
    stopSampling();
    destroyTrayIcon();
    m_overlay.destroy();
    m_sensors.shutdown();
    m_renderer.shutdown();
}

bool MainWindow::create(HINSTANCE instance, Log& log, std::string& error)
{
    m_instance = instance;
    m_log = &log;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &MainWindow::windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Иконка из ресурса (ID 101, app/resources/resource.rc): она же на ярлыке и в панели задач.
    HICON appIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    if (appIcon == nullptr) {
        appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    windowClass.hIcon = appIcon;
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    // Фон класса совпадает с фоном интерфейса: пока не пришёл первый кадр D2D,
    // окно не мигает белым.
    windowClass.hbrBackground = CreateSolidBrush(RGB(11, 14, 18));
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);

    // WS_POPUP + WS_THICKFRAME: рамка нужна только для изменения размера, заголовок
    // рисуем сами — системный светлый caption выбивался из тёмного интерфейса.
    m_window = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"YawaFpsHub",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 1280,
        800, nullptr, nullptr, instance, this);

    if (m_window == nullptr) {
        error = "не удалось создать главное окно";
        return false;
    }

    // Тёмный системный заголовок: приложение не рисует рамку само, но и светлая полоса
    // рядом с тёмным интерфейсом выглядит чужой.
    const BOOL darkTitle = TRUE;
    DwmSetWindowAttribute(m_window, 20, &darkTitle, sizeof(darkTitle));

    // Скруглённые углы Windows 11: своя рамка без них выглядит угловатой.
    const int cornerPreference = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_window, 33, &cornerPreference, sizeof(cornerPreference));

    // DWM рисует вокруг окна светлую рамку акцента — на тёмном интерфейсе она читается
    // как белая полоса. DWMWA_BORDER_COLOR с DWMWA_COLOR_NONE убирает её целиком.
    // На Windows 10 атрибут неизвестен, вызов просто вернёт ошибку.
    removeDwmBorder(m_window);

    // На Windows 10 атрибута 34 нет, и DWM дорисовывает вокруг окна светлую кромку.
    // Растянутая рамка (лист стекла) убирает её и на десятке: клиентскую область
    // окно рисует само, поэтому negative margins ничего не портят.
    const MARGINS glass{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(m_window, &glass);


    if (!m_renderer.initialize(error)) {
        return false;
    }

    std::string settingsText;
    if (readTextFile(settingsPath(), settingsText)) {
        std::string settingsError;
        if (!parseSettings(settingsText, m_model.settings, settingsError)) {
            log.warning("ini: " + settingsError + " — взяты значения по умолчанию");
            m_model.settings = defaultSettings();
        }
    } else {
        log.info("ini не найден, создаём новый");
        m_model.settings = defaultSettings();
        saveSettings();
    }

    // Разрешение и герцовку приложение определяет само: они нужны в подписях и как
    // цель кадра по умолчанию, чтобы бюджет кадра не был заведомо провальным.
    const DisplayMode display = currentDisplayMode();
    m_model.displayWidth = display.width;
    m_model.displayHeight = display.height;
    m_model.displayRefreshHz = display.refreshHz;
    if (m_model.settings.capture.targetAuto && display.refreshHz > 0) {
        m_model.settings.capture.targetHz = std::clamp(display.refreshHz, 30, 480);
        log.info("монитор: " + std::to_string(display.width) + "x" + std::to_string(display.height) + ", "
            + std::to_string(display.refreshHz) + " Гц — цель кадра из монитора");
    }

    m_model.settingsPath = settingsPath();
    m_theme = makeTheme(m_model.settings.ui.accent);

    if (!m_sensors.initialize(log, error)) {
        return false;
    }

    const GpuRegistry& registry = m_sensors.gpus();

    // Паспорт машины нужен «Балансу» и «Системе». Сбор — реестр, CPUID, SMBIOS и
    // EnumDisplaySettings: это десятки миллисекунд один раз при старте, а не работа в кадре.
    {
        std::string hardwareError;
        if (collectHardwareInfo(m_model.hardware, registry, hardwareError)) {
            const CpuIdentity& cpu = m_model.hardware.cpu;
            log.info("железо: " + (cpu.name.empty() ? std::string("CPU не определён") : cpu.name) + ", ядер "
                + std::to_string(cpu.physicalCores) + "/" + std::to_string(cpu.logicalProcessors) + ", модулей "
                + std::to_string(m_model.hardware.memory.modules.size()) + ", мониторов "
                + std::to_string(m_model.hardware.displays.size()));
        } else {
            log.warning("сведения о системе: " + hardwareError);
        }
    }

    for (std::size_t index = 0; index < registry.count(); ++index) {
        const GpuAdapter* adapter = registry.byIndex(index);
        if (adapter != nullptr) {
            m_model.gpuNames.push_back(adapter->name);
        }
    }

    if (m_model.settings.gpu.hasSelection) {
        const std::size_t resolved = registry.resolve(m_model.settings.gpu.luidLow, m_model.settings.gpu.luidHigh);
        m_sensors.selectGpu(resolved);
        m_model.activeGpu = resolved;
    } else {
        m_model.activeGpu = m_sensors.activeGpu();
        const GpuAdapter* adapter = registry.byIndex(m_model.activeGpu);
        if (adapter != nullptr) {
            m_model.settings.gpu.hasSelection = true;
            m_model.settings.gpu.luidLow = adapter->luidLow;
            m_model.settings.gpu.luidHigh = adapter->luidHigh;
        }
    }

    std::string overlayError;
    if (!m_overlay.create(instance, overlayError)) {
        log.warning("оверлей: " + overlayError);
    } else {
        m_overlay.setAccent(m_model.settings.ui.accent);
        // Правый клик по оверлею возвращает окно приложения.
        m_overlay.setOwnerWindow(m_window);
        m_overlay.setSettings(m_model.settings.overlay);
        // Перетаскивание прямо по оверлею: позицию сохраняем только по отпускании,
        // иначе ini переписывался бы сотни раз за одно движение.
        m_overlay.setMoveHandler([this](int offsetX, int offsetY, bool finished) {
            {
                std::lock_guard<std::mutex> guard(m_modelMutex);
                m_model.settings.overlay.offsetX = offsetX;
                m_model.settings.overlay.offsetY = offsetY;
            }
            if (finished) {
                saveSettings();
            }
            InvalidateRect(m_window, nullptr, FALSE);
        });
    }

    m_model.settingsPath = settingsPath();
    m_model.logPath = logPath();
    m_model.sessionsPath = sessionsDirectory();

    createTrayIcon();
    registerHotkeys();

    // Сначала рисуем кадр в ещё невидимое окно и только потом показываем его:
    // так при запуске нет белой полосы неотрисованной клиентской области.
    RECT client{};
    GetClientRect(m_window, &client);
    m_dpiScale = static_cast<float>(GetDpiForWindow(m_window)) / 96.0f;
    m_renderer.resize(static_cast<UINT>(client.right), static_cast<UINT>(client.bottom), m_dpiScale);
    paint();
    ShowWindow(m_window, SW_SHOWNORMAL);
    UpdateWindow(m_window);

    m_running = true;
    startSampling();
    return true;
}

void MainWindow::exportOverlayStyles()
{
    if (m_overlayShotDir.empty()) {
        return;
    }

    static constexpr std::array<std::wstring_view, 5> kStyles{
        L"glass", L"neon", L"capsule", L"terminal", L"minimal" };

    OverlaySettings original;
    {
        std::lock_guard<std::mutex> guard(m_modelMutex);
        original = m_model.settings.overlay;
        m_overlay.setContent(m_model.store, m_model.fps,
            m_model.activeGpu < m_model.gpuNames.size() ? m_model.gpuNames[m_model.activeGpu] : L"");
    }

    for (std::size_t index = 0; index < kStyles.size(); ++index) {
        OverlaySettings settings = original;
        settings.style = static_cast<OverlayStyle>(index);
        m_overlay.setSettings(settings);

        std::string error;
        const std::wstring path = m_overlayShotDir + L"overlay-" + std::wstring(kStyles[index]) + L".png";
        if (!m_overlay.capturePng(path, error) && m_log != nullptr) {
            m_log->warning("скриншот оверлея: " + error);
        }
    }

    m_overlay.setSettings(original);
}

void MainWindow::toggleOverlay()
{
    std::string error;
    m_overlay.toggle(&error);
    m_model.overlayVisible = m_overlay.isVisible();

    if (!error.empty()) {
        // Раньше сбой показа проходил молча, и оверлей «не появлялся». Теперь причина
        // видна в логе, в статус-баре и в журнале окон.
        if (m_log != nullptr) {
            m_log->warning("оверлей: " + error);
        }
        m_model.overlayError = L"Оверлей: " + widen(error);
    } else {
        m_model.overlayError.clear();
    }

    InvalidateRect(m_window, nullptr, FALSE);
    if (m_log != nullptr) {
        m_log->info(std::string("оверлей ") + (m_model.overlayVisible ? "включён" : "выключен"));
    }
}

void MainWindow::setInitialSize(int width, int height)
{
    m_initialWidth = width;
    m_initialHeight = height;
    // Окно уже создано: меняем размер сразу, чтобы CI снял кадр нужного размера.
    if (m_window != nullptr && width > 0 && height > 0) {
        SetWindowPos(m_window, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void MainWindow::setOverlayVisible(bool visible)
{
    if (visible != m_overlay.isVisible()) {
        toggleOverlay();
    }
}

int MainWindow::run(int smokeSeconds, const std::wstring& overlayShotDir)
{
    m_overlayShotDir = overlayShotDir;

    std::thread smoke;
    if (smokeSeconds > 0) {
        // WM_CLOSE в трей-приложении не завершает процесс, поэтому самопроверка
        // ставит WM_QUIT прямо в очередь главного потока.
        const DWORD mainThread = GetCurrentThreadId();
        smoke = std::thread([smokeSeconds, mainThread, this]() {
            if (!m_overlayShotDir.empty()) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                PostMessageW(m_window, kExportMessage, 0, 0);
                std::this_thread::sleep_for(std::chrono::seconds(1));
            } else {
                std::this_thread::sleep_for(std::chrono::seconds(smokeSeconds));
            }
            PostThreadMessageW(mainThread, WM_QUIT, 0, 0);
        });
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (smoke.joinable()) {
        smoke.join();
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK MainWindow::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        auto* self = static_cast<MainWindow*>(create->lpCreateParams);
        self->m_window = window;
        return TRUE;
    }

    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return self->handleMessage(message, wParam, lParam);
}

LRESULT MainWindow::handleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_SIZE:
        if (m_renderer.isReady()) {
            m_renderer.resize(LOWORD(lParam), HIWORD(lParam), m_dpiScale);
        }
        m_model.maximized = IsZoomed(m_window) != FALSE;
        return 0;

    case WM_GETMINMAXINFO: {
        // Карточки больше не сжимаются под окно, а прокручиваются, поэтому минимум
        // держим по шапке и рельсу: ниже окно становится нечитаемым.
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = static_cast<LONG>(760.0f * m_dpiScale);
        info->ptMinTrackSize.y = static_cast<LONG>(600.0f * m_dpiScale);
        return 0;
    }

    case WM_NCACTIVATE:
        // Рамку рисуем сами: без этого DWM на миг показывает светлую полосу при активации.
        return TRUE;

    case WM_ACTIVATE:
        // При активации DWM пересоздаёт кромку окна: возвращаем «без рамки» каждый раз.
        if (LOWORD(wParam) != WA_INACTIVE) {
            removeDwmBorder(m_window);
        }
        break;

    case WM_NCPAINT:
        // Нестандартную рамку не отдаём DWM: иначе при активации окна по краям
        // появляется белая кромка, которую видно на тёмном интерфейсе.
        return 0;

    case WM_NCCALCSIZE:
        // Заголовок убираем полностью: клиентская область занимает всё окно.
        // В развёрнутом состоянии Windows добавляет рамку, её компенсируем.
        if (wParam == TRUE) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            if (IsZoomed(m_window) != FALSE) {
                const int borderX = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                const int borderY = GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                params->rgrc[0].left += borderX;
                params->rgrc[0].right -= borderX;
                params->rgrc[0].top += borderY;
                params->rgrc[0].bottom -= borderY;
            }
            return 0;
        }
        break;

    case WM_NCHITTEST: {
        POINT cursor{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(m_window, &cursor);

        // Края тянем сами: у окна без системного заголовка DefWindowProc не отдаёт
        // HTLEFT/HTBOTTOM, и за края изменить размер было нельзя.
        if (IsZoomed(m_window) == FALSE) {
            const LONG border = static_cast<LONG>(8.0f * m_dpiScale);
            RECT client{};
            GetClientRect(m_window, &client);

            const bool left = cursor.x < border;
            const bool right = cursor.x >= client.right - border;
            const bool top = cursor.y < border;
            const bool bottom = cursor.y >= client.bottom - border;
            if (left && top) {
                return HTTOPLEFT;
            }
            if (right && top) {
                return HTTOPRIGHT;
            }
            if (left && bottom) {
                return HTBOTTOMLEFT;
            }
            if (right && bottom) {
                return HTBOTTOMRIGHT;
            }
            if (left) {
                return HTLEFT;
            }
            if (right) {
                return HTRIGHT;
            }
            if (top) {
                return HTTOP;
            }
            if (bottom) {
                return HTBOTTOM;
            }
        }

        const float x = static_cast<float>(cursor.x) / m_dpiScale;
        const float y = static_cast<float>(cursor.y) / m_dpiScale;

        // Кнопки заголовка остаются обычной клиентской областью: на них приходят клики.
        if (y < kTitleBarHeight && !titleBarButtonAt(x, y, m_renderer.width())) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_DPICHANGED: {
        m_dpiScale = static_cast<float>(HIWORD(wParam)) / 96.0f;
        const RECT* target = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(m_window, nullptr, target->left, target->top, target->right - target->left,
            target->bottom - target->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(m_window, &paint);
        this->paint();
        EndPaint(m_window, &paint);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case kTickMessage:
        // Тик сборщика: он приходит по таймеру, а не по движению мыши, поэтому
        // и окно, и оверлей обновляются сами по себе.
        onTick();
        return 0;

    case kExportMessage:
        exportOverlayStyles();
        return 0;

    case WM_APP + 10:
        m_dpiScale = static_cast<float>(GetDpiForWindow(m_window)) / 96.0f;
        if (m_renderer.isReady()) {
            RECT client{};
            GetClientRect(m_window, &client);
            m_renderer.resize(static_cast<UINT>(client.right), static_cast<UINT>(client.bottom), m_dpiScale);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam)) / m_dpiScale;
        const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / m_dpiScale;

        for (const UiHit& hit : m_hits) {
            if (!hit.rect.contains(x, y)) {
                continue;
            }
            if (hit.action == ActionKind::DragOverlay) {
                m_dragAction = ActionKind::DragOverlay;
                m_dragStartX = x;
                m_dragStartY = y;
                m_dragOffsetX = m_model.settings.overlay.offsetX;
                m_dragOffsetY = m_model.settings.overlay.offsetY;
                SetCapture(m_window);
                return 0;
            }
            if (isSliderAction(hit.action)) {
                m_dragAction = hit.action;
                m_dragRect = hit.rect;
                SetCapture(m_window);
                applySlider(hit.action, hit.rect, x);
                return 0;
            }
            if (hit.action == ActionKind::ScrollContentThumb) {
                // Бегунок тянем: запоминаем, за какую точку его взяли.
                m_draggingScroll = true;
                m_scrollGrabDy = y - hit.rect.top;
                SetCapture(m_window);
                return 0;
            }
            if (hit.action == ActionKind::ScrollSensorsThumb) {
                m_draggingSensors = true;
                m_scrollGrabDy = y - hit.rect.top;
                m_scrollTrack = m_model.sensorScrollTrack;
                SetCapture(m_window);
                return 0;
            }
            if (hit.action == ActionKind::ScrollSensorsTrack) {
                const float maxScroll = m_model.sensorScrollMax;
                const float step = std::max(m_model.sensorViewportHeight * 0.85f, 120.0f);
                const float before = static_cast<float>(m_model.sensorScroll);
                const bool forward = y > hit.rect.centerY();
                const float next = std::clamp(before + (forward ? step : -step), 0.0f, maxScroll);
                m_model.sensorScroll = static_cast<int>(next);
                InvalidateRect(m_window, nullptr, FALSE);
                return 0;
            }
            if (hit.action == ActionKind::ScrollContentTrack) {
                // Клик по дорожке листает на страницу в сторону клика.
                const float limit = std::max(contentDesignHeight() - contentArea().height(), 0.0f);
                const float step = contentArea().height() * 0.85f;
                const bool forward = y > hit.rect.centerY();
                m_model.contentScroll = std::clamp(m_model.contentScroll + (forward ? step : -step), 0.0f, limit);
                InvalidateRect(m_window, nullptr, FALSE);
                return 0;
            }
        }

        handleClick(x, y);
        return 0;
    }

    case WM_MOUSEMOVE: {
        const float cursorX = static_cast<float>(GET_X_LPARAM(lParam)) / m_dpiScale;
        const float cursorY = static_cast<float>(GET_Y_LPARAM(lParam)) / m_dpiScale;
        if (!m_trackingMouse) {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = m_window;
            m_trackingMouse = TrackMouseEvent(&track) != FALSE;
        }
        const bool inTitleBar = cursorY < kTitleBarHeight;
        const bool wasInTitleBar = m_model.cursorY < kTitleBarHeight;
        m_model.cursorX = cursorX;
        m_model.cursorY = cursorY;
        m_model.cursorInside = true;

        // Перерисовка нужна только когда курсор перешёл на другой элемент:
        // иначе WM_MOUSEMOVE вызывал бы перерисовку всего окна на каждое движение.
        int hovered = -1;
        for (std::size_t index = m_hits.size(); index > 0; --index) {
            // Учитываем и строки таблиц с ActionKind::None: у них есть подсветка при наведении.
            if (m_hits[index - 1].rect.contains(cursorX, cursorY)) {
                hovered = static_cast<int>(index - 1);
                break;
            }
        }
        const bool repaint = inTitleBar || wasInTitleBar || hovered != m_hoverIndex;
        m_hoverIndex = hovered;
        if (repaint) {
            InvalidateRect(m_window, nullptr, FALSE);
        }

        if (m_draggingScroll || m_draggingSensors) {
            const float limit = m_draggingSensors ? m_model.sensorScrollMax
                : std::max(contentDesignHeight() - contentArea().height(), 0.0f);
            // Геометрия полосы берётся из сохранённой дорожки и текущего вылета бегунка.
            const Rect& track = m_scrollTrack;
            const float viewportHeight = m_draggingSensors ? m_model.sensorViewportHeight
                : contentArea().height();
            const float designHeight = m_draggingSensors ? std::max(viewportHeight + limit, 1.0f)
                : contentDesignHeight();
            const float trackHeight = track.height();
            const float thumbHeight = std::max(m_draggingSensors ? 24.0f : 40.0f,
                trackHeight * viewportHeight / designHeight);
            const float travel = std::max(trackHeight - thumbHeight, 1.0f);
            const float thumbTop = std::clamp(cursorY - m_scrollGrabDy, track.top, track.top + travel);
            const float fraction = (thumbTop - track.top) / travel;
            if (m_draggingSensors) {
                m_model.sensorScroll = static_cast<int>(limit * fraction);
            } else {
                m_model.contentScroll = limit * fraction;
            }
            InvalidateRect(m_window, nullptr, FALSE);
            return 0;
        }

        if (m_dragAction == ActionKind::DragOverlay) {
            const float x = static_cast<float>(GET_X_LPARAM(lParam)) / m_dpiScale;
            const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / m_dpiScale;
            m_model.settings.overlay.offsetX
                = std::clamp(m_dragOffsetX + static_cast<int>(std::lround(x - m_dragStartX)), -600, 600);
            m_model.settings.overlay.offsetY
                = std::clamp(m_dragOffsetY + static_cast<int>(std::lround(y - m_dragStartY)), -600, 600);
            m_overlay.setSettings(m_model.settings.overlay);
            InvalidateRect(m_window, nullptr, FALSE);
            return 0;
        }
        if (m_dragAction != ActionKind::None) {
            dragSlider(static_cast<float>(GET_X_LPARAM(lParam)) / m_dpiScale);
            return 0;
        }
        break;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(m_window, &cursor);
            const float x = static_cast<float>(cursor.x) / m_dpiScale;
            const float y = static_cast<float>(cursor.y) / m_dpiScale;
            for (const UiHit& hit : m_hits) {
                if (hit.action != ActionKind::None && hit.rect.contains(x, y)) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
        }
        break;

    case WM_MOUSELEAVE:
        m_trackingMouse = false;
        m_model.cursorInside = false;
        InvalidateRect(m_window, nullptr, FALSE);
        return 0;

    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const float steps = static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA);
        // На экране датчиков список внутри карточки прокручивается, пока есть что:
        // это привычнее, чем уезжающая страница.
        if (m_model.view == View::Sensors) {
            const int previous = m_model.sensorScroll;
            const int limit = static_cast<int>(m_model.sensorScrollMax);
            m_model.sensorScroll = std::clamp(previous - static_cast<int>(steps) * 3, 0, limit);
            if (m_model.sensorScroll != previous) {
                InvalidateRect(m_window, nullptr, FALSE);
                return 0;
            }
        }
        scrollContent(-steps * 64.0f);
        return 0;
    }

    case WM_LBUTTONUP:
        if (m_draggingScroll || m_draggingSensors) {
            m_draggingScroll = false;
            m_draggingSensors = false;
            ReleaseCapture();
            return 0;
        }
        if (isSliderAction(m_dragAction)) {
            m_dragAction = ActionKind::None;
            ReleaseCapture();
            saveSettings();
            return 0;
        }
        if (m_dragAction != ActionKind::None) {
            m_dragAction = ActionKind::None;
            ReleaseCapture();
            saveSettings();
            return 0;
        }
        break;

    // F10 приходит как WM_SYSKEYDOWN, поэтому захват клавиши слушаем в обоих сообщениях.
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (m_model.hotkeyEditing >= 0) {
            // Esc — отмена, Delete — отключить клавишу, всё остальное становится хоткеем.
            unsigned int* slot = nullptr;
            if (m_model.hotkeyEditing == 0) {
                slot = &m_model.settings.hotkeys.overlay;
            } else if (m_model.hotkeyEditing == 1) {
                slot = &m_model.settings.hotkeys.position;
            } else if (m_model.hotkeyEditing == 2) {
                slot = &m_model.settings.hotkeys.window;
            } else if (m_model.hotkeyEditing == 3) {
                slot = &m_model.settings.hotkeys.clickThrough;
            }

            if (wParam == VK_ESCAPE || slot == nullptr) {
                m_model.hotkeyEditing = -1;
            } else if (wParam == VK_DELETE) {
                *slot = 0;
                m_model.hotkeyEditing = -1;
                registerHotkeys();
                saveSettings();
            } else if (wParam < 0x100 && wParam != static_cast<WPARAM>(VK_SHIFT)
                && wParam != static_cast<WPARAM>(VK_CONTROL) && wParam != static_cast<WPARAM>(VK_MENU)
                && wParam != static_cast<WPARAM>(VK_LWIN) && wParam != static_cast<WPARAM>(VK_RWIN)
                && wParam != static_cast<WPARAM>(VK_CAPITAL)) {
                *slot = static_cast<unsigned int>(wParam);
                m_model.hotkeyEditing = -1;
                registerHotkeys();
                saveSettings();
            }
            InvalidateRect(m_window, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_CHAR:
    case WM_IME_CHAR:
        if (m_model.editingLabel >= 0) {
            editLabelChar(static_cast<wchar_t>(wParam));
            InvalidateRect(m_window, nullptr, FALSE);
        } else if (m_model.sensorSearchActive) {
            editSensorQuery(static_cast<wchar_t>(wParam));
            InvalidateRect(m_window, nullptr, FALSE);
        }
        return 0;

    case WM_HOTKEY:
        // wParam — WPARAM, идентификаторы int: сравниваем приведёнными, иначе C4018 при /W4.
        if (wParam == static_cast<WPARAM>(kHotkeyOverlay)) {
            toggleOverlay();
        } else if (wParam == static_cast<WPARAM>(kHotkeyPosition)) {
            const int next = (static_cast<int>(m_model.settings.overlay.position) + 1) % 4;
            m_model.settings.overlay.position = static_cast<OverlayPosition>(next);
            m_overlay.setSettings(m_model.settings.overlay);
            saveSettings();
            InvalidateRect(m_window, nullptr, FALSE);
        } else if (wParam == static_cast<WPARAM>(kHotkeyWindow)) {
            toggleVisibilityFromTray();
        } else if (wParam == static_cast<WPARAM>(kHotkeyClickThrough)) {
            m_model.settings.overlay.clickThrough = !m_model.settings.overlay.clickThrough;
            m_overlay.setSettings(m_model.settings.overlay);
            saveSettings();
            InvalidateRect(m_window, nullptr, FALSE);
        }
        return 0;

    case kTrayMessage:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            showTrayMenu();
        } else if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            toggleVisibilityFromTray();
        }
        return 0;

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case 1001:
            toggleVisibilityFromTray();
            break;
        case 1002:
            toggleOverlay();
            break;
        case 1003:
            m_quitRequested = true;
            PostMessageW(m_window, WM_CLOSE, 0, 0);
            break;
        default:
            break;
        }
        return 0;
    }

    case WM_CLOSE:
        // Крестик прячет окно в трей, если так настроено; «Выход» в меню трея
        // и снятая галочка «сворачивать в трей» закрывают приложение по-настоящему.
        if (!m_quitRequested && m_model.settings.ui.closeToTray) {
            ShowWindow(m_window, SW_HIDE);
            return 0;
        }
        DestroyWindow(m_window);
        return 0;

    case WM_DESTROY:
        m_overlay.hide();
        destroyTrayIcon();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(m_window, message, wParam, lParam);
}

Rect MainWindow::contentArea() const
{
    const float top = kTitleBarHeight + kTopBarHeight + 8.0f;
    const float bottom = m_renderer.height() - kStatusBarHeight - 10.0f;
    return Rect{ kRailWidth + 30.0f, top, m_renderer.width() - 22.0f, bottom };
}

float MainWindow::contentDesignHeight() const
{
    switch (m_model.view) {
    case View::Dashboard:
        // В узком окне карточки идут по одной — контент выше, и прокрутка должна доставать до конца.
        return contentArea().width() < kDashboardNarrowWidth ? kDashboardNarrowHeight : kDashboardHeight;
    case View::Sensors:
        return contentArea().height();
    case View::Overlay:
        return kOverlayHeight;
    case View::Settings:
        return kSettingsHeight;
    case View::Bottleneck:
        return kBottleneckHeight;
    case View::System:
        return kSystemHeight;
    }
    return kDashboardHeight;
}

void MainWindow::scrollContent(float delta)
{
    const float viewport = contentArea().height();
    const float limit = std::max(contentDesignHeight() - viewport, 0.0f);
    const float next = std::clamp(m_model.contentScroll + delta, 0.0f, limit);
    if (std::abs(next - m_model.contentScroll) < 0.5f) {
        return;
    }
    m_model.contentScroll = next;
    InvalidateRect(m_window, nullptr, FALSE);
}

void MainWindow::drawScrollBar(const Rect& viewport, std::vector<UiHit>& hits)
{
    const float limit = contentDesignHeight() - viewport.height();
    if (limit <= 1.0f) {
        return;
    }
    const float trackTop = viewport.top + 2.0f;
    const float trackBottom = viewport.bottom - 2.0f;
    const float trackHeight = trackBottom - trackTop;
    const float thumbHeight = std::max(40.0f, trackHeight * viewport.height() / contentDesignHeight());
    const float travel = trackHeight - thumbHeight;
    const float fraction = std::clamp(m_model.contentScroll / limit, 0.0f, 1.0f);
    const float thumbTop = trackTop + travel * fraction;

    // Дорожка и бегунок: по дорожке кликаем, бегунок тянем мышью.
    const Rect track{ viewport.right + 4.0f, trackTop, viewport.right + 11.0f, trackBottom };
    const Rect thumb{ track.left, thumbTop, track.right, thumbTop + thumbHeight };
    const bool hovered = isHovered(m_model, track) || m_draggingScroll;
    m_renderer.fillRoundRect(track, 3.0f, m_theme.line.withAlpha(hovered ? 0.55f : 0.28f));
    m_renderer.fillRoundRect(thumb, 3.0f,
        m_theme.lineStrong.withAlpha(hovered || m_draggingScroll ? 1.0f : 0.8f));

    hits.push_back(UiHit{ Rect{ track.left - 2.0f, thumbTop, track.right + 2.0f, thumbTop + thumbHeight },
        ActionKind::ScrollContentThumb, 0 });
    hits.push_back(UiHit{ track, ActionKind::ScrollContentTrack, 0 });
    m_scrollTrack = track;
}

std::vector<UiHit> MainWindow::buildHits()
{
    std::vector<UiHit> hits;

    const Rect viewport = contentArea();
    // Экран рисуется в демо-высоте и прокручивается: на маленьком окне видно меньше
    // карточек, но их размеры те же, что в прототипе.
    m_model.contentHeight = std::max(viewport.height(), contentDesignHeight());
    const Rect area{ viewport.left, viewport.top, viewport.right, viewport.top + m_model.contentHeight };
    // Клип по рабочей области: содержимое карточек не может вылезти в статус-бар.
    m_renderer.pushClip(viewport);
    const bool scrolled = m_model.contentScroll > 0.5f;
    if (scrolled) {
        m_renderer.pushTranslate(0.0f, -m_model.contentScroll);
    }
    // Курсор приходит в координатах окна: сдвигаем его в координаты содержимого,
    // чтобы наведение работало и после прокрутки.
    const float cursorY = m_model.cursorY;
    m_model.cursorY += m_model.contentScroll;
    switch (m_model.view) {
    case View::Dashboard:
        drawDashboard(m_renderer, m_model, m_theme, area, hits);
        break;
    case View::Sensors:
        drawSensors(m_renderer, m_model, m_theme, area, hits);
        break;
    case View::Overlay:
        drawOverlayView(m_renderer, m_model, m_theme, area, hits);
        break;
    case View::Settings:
        drawSettings(m_renderer, m_model, m_theme, area, hits);
        break;
    case View::Bottleneck:
        drawBottleneckView(m_renderer, m_model, m_theme, area, hits);
        break;
    case View::System:
        drawSystemView(m_renderer, m_model, m_theme, area, hits);
        break;
    }
    if (scrolled) {
        m_renderer.popTranslate();
    }
    m_model.cursorY = cursorY;
    m_renderer.popClip();
    return hits;
}

void MainWindow::paint()
{
    if (!m_renderer.begin(m_window)) {
        return;
    }

    // Снимок модели снимается под мьютексом: сборщик пишет в него с другого потока.
    std::lock_guard<std::mutex> guard(m_modelMutex);

    m_model.maximized = IsZoomed(m_window) != FALSE;
    m_renderer.clear(m_theme.background);
    // Свечение как в прототипе: акцент слева сверху и холодный отсвет справа.
    // Рисуем до интерфейса, чтобы карточки и панели легли поверх.
    const float width = m_renderer.width();
    m_renderer.fillRadialGlow(width * 0.12f, -60.0f, 560.0f, m_theme.accent.withAlpha(0.16f));
    m_renderer.fillRadialGlow(width * 0.94f, 30.0f, 440.0f, fromRgba(0xFF6AA8FF).withAlpha(0.10f));
    m_hits = buildHits();
    // Хиты построены в координатах содержимого — переводим их в координаты окна,
    // иначе после прокрутки клики попадали бы не туда.
    if (m_model.contentScroll > 0.5f) {
        for (UiHit& hit : m_hits) {
            hit.rect.top -= m_model.contentScroll;
            hit.rect.bottom -= m_model.contentScroll;
        }
    }
    // Полоса прокрутки живёт в координатах окна, поэтому её хиты добавляются после сдвига.
    drawScrollBar(contentArea(), m_hits);
    drawRail(m_renderer, m_model, m_theme, m_hits);
    drawTitleBar(m_renderer, m_model, m_theme, m_hits);
    drawTopBar(m_renderer, m_model, m_theme, m_hits);
    // Свечение поверх шапки: в прототипе оно мягко подсвечивает левый верхний угол,
    // а не только область контента.
    m_renderer.fillRadialGlow(width * 0.12f, -60.0f, 420.0f, m_theme.accent.withAlpha(0.10f));
    drawStatusBar(m_renderer, m_model, m_theme);

    m_renderer.end();
}

void MainWindow::handleClick(float x, float y)
{
    if (m_model.editingLabel >= 0) {
        m_model.editingLabel = -1;
    }
    m_model.sensorSearchActive = false;
    // Любой клик вне списка адаптеров закрывает его: открытие снова включит нужный обработчик.
    m_model.gpuMenuOpen = false;

    for (auto iterator = m_hits.rbegin(); iterator != m_hits.rend(); ++iterator) {
        const UiHit& hit = *iterator;
        if (!hit.rect.contains(x, y)) {
            continue;
        }
        applyAction(hit);
        InvalidateRect(m_window, nullptr, FALSE);
        return;
    }

    InvalidateRect(m_window, nullptr, FALSE);
}

void MainWindow::dragSlider(float x)
{
    applySlider(m_dragAction, m_dragRect, x);
}

void MainWindow::selectGpu(std::size_t index)
{
    if (index >= m_model.gpuNames.size()) {
        return;
    }

    m_sensors.selectGpu(index);
    m_model.activeGpu = m_sensors.activeGpu();
    const GpuAdapter* adapter = m_sensors.gpus().byIndex(m_model.activeGpu);
    if (adapter != nullptr) {
        m_model.settings.gpu.hasSelection = true;
        m_model.settings.gpu.luidLow = adapter->luidLow;
        m_model.settings.gpu.luidHigh = adapter->luidHigh;
    }
    saveSettings();
    InvalidateRect(m_window, nullptr, FALSE);
}

void MainWindow::applySlider(ActionKind action, const Rect& rect, float x)
{
    if (rect.width() <= 0.0f) {
        return;
    }

    const float fraction = std::clamp((x - rect.left) / rect.width(), 0.0f, 1.0f);
    UiHit hit{ rect, action, 0 };

    if (action == ActionKind::SetScale) {
        // Шаг 1 %: ползунок идёт за курсором без рывков.
        hit.value = 70 + static_cast<int>(std::lround(fraction * 90.0f));
    } else if (action == ActionKind::SetOverlayTextScale) {
        hit.value = 85 + static_cast<int>(std::lround(fraction * 45.0f));
    } else if (action == ActionKind::SetOverlayPadding) {
        hit.value = static_cast<int>(std::lround(fraction * 28.0f));
    } else if (action == ActionKind::SetOverlayRowGap) {
        hit.value = static_cast<int>(std::lround(fraction * 14.0f));
    } else if (action == ActionKind::SetOverlayRadius) {
        hit.value = static_cast<int>(std::lround(fraction * 24.0f));
    } else if (action == ActionKind::SetBorderWidth) {
        hit.value = static_cast<int>(std::lround(fraction * 3.0f));
    } else if (action == ActionKind::SetLetterSpacing) {
        hit.value = static_cast<int>(std::lround(fraction * 200.0f));
    } else {
        hit.value = static_cast<int>(std::lround(fraction * 100.0f));
    }

    applyAction(hit);
    InvalidateRect(m_window, nullptr, FALSE);
}

void MainWindow::editSensorQuery(wchar_t symbol)
{
    if (symbol == 27) { // Esc
        m_model.sensorSearchActive = false;
        m_model.sensorScroll = 0;
        return;
    }
    if (symbol == L'\r' || symbol == L'\n') {
        m_model.sensorSearchActive = false;
        return;
    }

    std::string& query = m_model.sensorQuery;
    if (symbol == L'\b') {
        // Стираем целый UTF-8 символ, иначе от кириллицы остаются обрывки байтов.
        while (!query.empty() && (static_cast<unsigned char>(query.back()) & 0xC0) == 0x80) {
            query.pop_back();
        }
        if (!query.empty()) {
            query.pop_back();
        }
    } else if (symbol == 127) {
        query.clear();
    } else if (symbol >= 32) {
        query += narrow(std::wstring(1, symbol));
    }

    m_model.sensorScroll = 0;
}

void MainWindow::editLabelChar(wchar_t symbol)
{
    const auto index = static_cast<std::size_t>(m_model.editingLabel);
    if (index >= kLabelCount) {
        return;
    }
    const LabelField field = static_cast<LabelField>(index);
    std::wstring current = wideText(labelValue(m_model.settings.overlay, field));

    if (symbol == L'\b') {
        if (!current.empty()) {
            current.pop_back();
        }
    } else if (symbol == L'\r' || symbol == L'\n') {
        m_model.editingLabel = -1;
        saveSettings();
        return;
    } else if (symbol == 27) { // Esc
        setLabel(m_model.settings.overlay, field, m_labelBackup);
        m_model.editingLabel = -1;
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        return;
    } else if (symbol == 127) { // Delete — стереть подпись целиком.
        current.clear();
    } else if (symbol >= 32) {
        current.push_back(symbol);
    }

    setLabel(m_model.settings.overlay, field, narrow(current));
    m_overlay.setSettings(m_model.settings.overlay);
}

void MainWindow::applyAction(const UiHit& hit)
{
    switch (hit.action) {
    case ActionKind::SwitchView:
        if (m_model.view != static_cast<View>(hit.value)) {
            m_model.contentScroll = 0.0f;
        }
        m_model.view = static_cast<View>(hit.value);
        break;

    case ActionKind::SetTargetHz:
        // Ноль — чип «Авто»: цель кадра берётся из герцовки монитора.
        m_model.settings.capture.targetAuto = hit.value == 0;
        if (hit.value == 0) {
            m_model.settings.capture.targetHz = m_model.displayRefreshHz > 0
                ? std::clamp(m_model.displayRefreshHz, 30, 480)
                : m_model.settings.capture.targetHz;
        } else {
            m_model.settings.capture.targetHz = std::clamp(hit.value, 30, 480);
        }
        saveSettings();
        break;

    case ActionKind::SetInterval:
        if (hit.value == 500 || hit.value == 1000 || hit.value == 2000) {
            m_model.settings.capture.intervalMs = hit.value;
            // Сборщик спит до следующего опроса — будим его, чтобы новый интервал
            // применялся сразу, а не через старую задержку.
            m_wake.notify_all();
        }
        saveSettings();
        break;

    case ActionKind::SetStyle:
        // Пресет приносит свои переключатели: у RTSS нет фона и рамки, у стекла есть.
        applyStyleDefaults(m_model.settings.overlay, static_cast<OverlayStyle>(hit.value));
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        InvalidateRect(m_window, nullptr, FALSE);
        break;

    case ActionKind::CaptureHotkey:
        m_model.hotkeyEditing = std::clamp(hit.value, 0, 3);
        break;

    case ActionKind::ToggleOverlayBackground:
        m_model.settings.overlay.showBackground = !m_model.settings.overlay.showBackground;
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ToggleOverlayBorder:
        m_model.settings.overlay.showBorder = !m_model.settings.overlay.showBorder;
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ToggleOverlayClickThrough:
        m_model.settings.overlay.clickThrough = !m_model.settings.overlay.clickThrough;
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ToggleOverlayShadow:
        m_model.settings.overlay.showShadow = !m_model.settings.overlay.showShadow;
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetOverlayFont:
        m_model.settings.overlay.fontFamily = std::clamp(hit.value, 0, 3);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ToggleOverlayBold:
        m_model.settings.overlay.fontBold = !m_model.settings.overlay.fontBold;
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ToggleOverlayItalic:
        m_model.settings.overlay.fontItalic = !m_model.settings.overlay.fontItalic;
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ChangeOverlayOutline:
        m_model.settings.overlay.textOutline
            = std::clamp(m_model.settings.overlay.textOutline + (hit.value < 0 ? -1 : 1), 0, 3);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetOverlayRadius:
        m_model.settings.overlay.cornerRadius = std::clamp(hit.value, 0, 24);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetPosition:
        m_model.settings.overlay.position = static_cast<OverlayPosition>(hit.value);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetLayout:
        m_model.settings.overlay.layout = static_cast<OverlayLayout>(hit.value);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetAccent:
        m_model.settings.ui.accent = static_cast<Accent>(hit.value);
        m_theme = makeTheme(m_model.settings.ui.accent);
        m_overlay.setAccent(m_model.settings.ui.accent);
        saveSettings();
        break;

    case ActionKind::CycleGpu:
        selectGpu((m_model.activeGpu + 1) % std::max<std::size_t>(m_model.gpuNames.size(), 1));
        break;

    case ActionKind::ToggleGpuMenu:
        // hit.value хранит состояние на момент отрисовки: клик по открытому списку закрывает его.
        m_model.gpuMenuOpen = hit.value == 0;
        break;

    case ActionKind::SelectGpu:
        selectGpu(static_cast<std::size_t>(std::max(hit.value, 0)));
        m_model.gpuMenuOpen = false;
        break;

    case ActionKind::SelectLabel: {
        const LabelField field = static_cast<LabelField>(std::clamp(hit.value, 0, static_cast<int>(kLabelCount) - 1));
        m_labelBackup = labelValue(m_model.settings.overlay, field);
        // Поле очищается сразу: иначе новое название пришлось бы стирать посимвольно.
        // Esc возвращает прежнюю подпись.
        setLabel(m_model.settings.overlay, field, std::string{});
        m_model.editingLabel = hit.value;
        m_overlay.setSettings(m_model.settings.overlay);
        break;
    }

    case ActionKind::ClearLabel: {
        const LabelField field = static_cast<LabelField>(std::clamp(hit.value, 0, static_cast<int>(kLabelCount) - 1));
        setLabel(m_model.settings.overlay, field, std::string{});
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;
    }

    case ActionKind::ApplyLabelPreset:
        m_model.settings.overlay.labels = hit.value == 0 ? std::array<std::string, kLabelCount>{ "FPS", "CPU", "GPU",
                                                                    "RAM", "VRAM" }
            : hit.value == 1                                ? std::array<std::string, kLabelCount>{ "КАДРЫ", "ЦП",
                                                                       "ГП", "ОЗУ", "ВИДЕО" }
                                                            : std::array<std::string, kLabelCount>{ "", "", "", "", "" };
        m_model.editingLabel = -1;
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ResetOverlayPlacement:
        m_model.settings.overlay.offsetX = 0;
        m_model.settings.overlay.offsetY = 0;
        m_model.settings.overlay.scalePercent = 100;
        m_model.settings.overlay.opacityPercent = 72;
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ResetLabels:
        // Сбрасываем только подписи: остальные настройки оверлея не трогаем.
        m_model.settings.overlay.labels = OverlaySettings{}.labels;
        m_model.settings.overlay.visible = OverlaySettings{}.visible;
        m_model.editingLabel = -1;
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetDensity:
        m_model.settings.ui.compactDensity = hit.value != 0;
        saveSettings();
        break;

    case ActionKind::ToggleCompact:
        m_model.settings.ui.compactDensity = !m_model.settings.ui.compactDensity;
        saveSettings();
        break;

    case ActionKind::ToggleTray:
        m_model.settings.ui.closeToTray = !m_model.settings.ui.closeToTray;
        saveSettings();
        break;

    case ActionKind::ToggleEtw:
        m_model.settings.capture.etwEnabled = !m_model.settings.capture.etwEnabled;
        saveSettings();
        restartSampling();
        break;

    case ActionKind::ToggleIdlePause:
        m_model.settings.capture.pauseWhenIdle = !m_model.settings.capture.pauseWhenIdle;
        saveSettings();
        break;

    case ActionKind::ToggleDiskNet:
        m_model.settings.capture.includeDiskNet = !m_model.settings.capture.includeDiskNet;
        saveSettings();
        break;

    case ActionKind::SetSensorFilter:
        m_model.sensorFilter = hit.value;
        break;

    case ActionKind::OpenSettingsFolder:
        revealInExplorer(executableDirectory());
        break;

    case ActionKind::ResetSettings:
        m_model.settings = defaultSettings();
        m_theme = makeTheme(m_model.settings.ui.accent);
        m_overlay.setAccent(m_model.settings.ui.accent);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetScale:
        m_model.settings.overlay.scalePercent = std::clamp(hit.value, 70, 160);
        m_overlay.setSettings(m_model.settings.overlay);
        break;

    case ActionKind::SetOpacity:
        m_model.settings.overlay.opacityPercent = std::clamp(hit.value, 0, 100);
        m_overlay.setSettings(m_model.settings.overlay);
        break;

    case ActionKind::SetOverlayTextScale:
        m_model.settings.overlay.textScalePercent = std::clamp(hit.value, 85, 130);
        m_overlay.setSettings(m_model.settings.overlay);
        break;

    case ActionKind::SetOverlayBgOpacity:
        m_model.settings.overlay.backgroundOpacityPercent = std::clamp(hit.value, 0, 100);
        m_overlay.setSettings(m_model.settings.overlay);
        break;

    case ActionKind::SetOverlayPadding:
        m_model.settings.overlay.paddingPx = std::clamp(hit.value, 0, 28);
        m_overlay.setSettings(m_model.settings.overlay);
        break;

    case ActionKind::SetOverlayRowGap:
        m_model.settings.overlay.rowGapPx = std::clamp(hit.value, 0, 14);
        m_overlay.setSettings(m_model.settings.overlay);
        break;

    case ActionKind::SetValueColor:
        m_model.settings.overlay.valueColor = std::clamp(hit.value, -1, kOverlayColorCount - 1);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetLabelColor:
        m_model.settings.overlay.labelColor = std::clamp(hit.value, -1, kOverlayColorCount - 1);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetAccentColor:
        m_model.settings.overlay.accentColor = std::clamp(hit.value, -1, kOverlayColorCount - 1);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetRowColor: {
        const int field = hit.value / 8;
        const int color = hit.value % 8 - 1;
        if (field >= 0 && field < static_cast<int>(kLabelCount)) {
            m_model.settings.overlay.rowColors[static_cast<std::size_t>(field)]
                = std::clamp(color, -1, kOverlayColorCount - 1);
            m_overlay.setSettings(m_model.settings.overlay);
            saveSettings();
        }
        break;
    }

    case ActionKind::SetBorderWidth:
        // Цвета, рамка, тень и типографика применяются без анимации:
        // пользователь двигает ползунок и ждёт мгновенного отклика.
        m_model.settings.overlay.borderWidth = std::clamp(hit.value, -1, 3);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetShadowStrength:
        m_model.settings.overlay.shadowStrength = std::clamp(hit.value, 0, 100);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetLetterSpacing:
        m_model.settings.overlay.letterSpacing = std::clamp(hit.value, 0, 200);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetUppercase:
        m_model.settings.overlay.uppercaseMode = std::clamp(hit.value, -1, 1);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetSeparators:
        m_model.settings.overlay.separatorsMode = std::clamp(hit.value, -1, 1);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetFpsDecimals:
        m_model.settings.overlay.fpsDecimals = std::clamp(hit.value, -1, 2);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ToggleMetric:
        if (hit.value >= 0 && hit.value < static_cast<int>(kLabelCount)) {
            auto& visible = m_model.settings.overlay.visible[static_cast<std::size_t>(hit.value)];
            visible = !visible;
            m_overlay.setSettings(m_model.settings.overlay);
            saveSettings();
        }
        break;

    case ActionKind::OffsetX:
        m_model.settings.overlay.offsetX = std::clamp(m_model.settings.overlay.offsetX + hit.value, -600, 600);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::OffsetY:
        m_model.settings.overlay.offsetY = std::clamp(m_model.settings.overlay.offsetY + hit.value, -600, 600);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ToggleOverlay:
        toggleOverlay();
        break;

    case ActionKind::TogglePause:
        m_model.paused = !m_model.paused;
        break;

    case ActionKind::WindowMinimize:
        ShowWindow(m_window, SW_MINIMIZE);
        break;

    case ActionKind::WindowMaximize:
        ShowWindow(m_window, IsZoomed(m_window) != FALSE ? SW_RESTORE : SW_MAXIMIZE);
        break;

    case ActionKind::WindowClose:
        PostMessageW(m_window, WM_CLOSE, 0, 0);
        break;

    case ActionKind::FocusSensorSearch:
        m_model.sensorSearchActive = true;
        break;

    case ActionKind::ToggleOverlayOnly:
        m_model.overlayOnly = !m_model.overlayOnly;
        m_model.sensorScroll = 0;
        break;

    case ActionKind::DragOverlay:
        // Само перетаскивание обрабатывается в WM_MOUSEMOVE, сюда попадает только клик.
        break;

    case ActionKind::ScrollContentThumb:
    case ActionKind::ScrollContentTrack:
    case ActionKind::ScrollSensorsThumb:
    case ActionKind::ScrollSensorsTrack:
        // Полосы прокрутки обрабатываются раньше, в WM_LBUTTONDOWN: клик до сюда не доходит.
        break;

    case ActionKind::None:
        break;
    }
}

void MainWindow::saveSettings()
{
    const std::string text = serializeSettings(m_model.settings);
    if (!writeTextFile(settingsPath(), text) && m_log != nullptr) {
        m_log->warning("не удалось записать " + settingsPath());
    }
}

void MainWindow::startSampling()
{
    m_stopRequested = false;
    m_worker = std::thread([this]() { samplingLoop(); });
}

void MainWindow::stopSampling()
{
    if (!m_worker.joinable()) {
        return;
    }

    m_stopRequested = true;
    m_wake.notify_all();
    m_worker.join();
}

void MainWindow::restartSampling()
{
    stopSampling();
    {
        std::lock_guard<std::mutex> guard(m_modelMutex);
        m_model.etwRunning = false;
        m_model.store.reset();
    }
    startSampling();
}

bool MainWindow::isSystemProcessCached(std::uint32_t processId)
{
    for (const auto& [cachedId, isSystem] : m_systemProcessCache) {
        if (cachedId == processId) {
            return isSystem;
        }
    }

    // Имя процесса: dwm.exe, explorer.exe и хосты оболочки подают кадры постоянно,
    // но игрой они не являются.
    static constexpr std::array<std::wstring_view, 14> kSystemNames{ L"dwm.exe", L"explorer.exe",
        L"csrss.exe", L"winlogon.exe", L"wininit.exe", L"services.exe", L"lsass.exe", L"smss.exe",
        L"searchhost.exe", L"startmenuexperiencehost.exe", L"textinputhost.exe",
        L"shellexperiencehost.exe", L"applicationframehost.exe", L"lockapp.exe" };

    bool system = true;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process != nullptr) {
        wchar_t path[512]{};
        DWORD size = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &size) != FALSE && size > 0) {
            std::wstring name(path, size);
            const std::size_t slash = name.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                name.erase(0, slash + 1);
            }
            for (wchar_t& symbol : name) {
                symbol = static_cast<wchar_t>(std::towlower(symbol));
            }
            system = false;
            for (std::wstring_view known : kSystemNames) {
                if (name == known) {
                    system = true;
                    break;
                }
            }
        }
        CloseHandle(process);
    }

    // Кэш короткий: pid переиспользуются, но за пару секунд состав игры не меняется.
    if (m_systemProcessCache.size() > 32) {
        m_systemProcessCache.clear();
    }
    m_systemProcessCache.emplace_back(processId, system);
    return system;
}

void MainWindow::samplingLoop()
{
    yawa::FrameTracker tracker(EtwFrameSession::qpcFrequency());
    yawa::SystemSampler sampler;

    if (m_model.settings.capture.etwEnabled) {
        std::string error;
        if (!m_etw.start(error)) {
            m_etw.stop();
            if (m_log != nullptr) {
                m_log->warning("ETW: " + error);
            }
        }
    }

    auto previous = std::chrono::steady_clock::now();

    while (!m_stopRequested) {
        int interval = 1000;
        bool paused = false;
        {
            std::lock_guard<std::mutex> guard(m_modelMutex);
            interval = m_model.settings.capture.intervalMs;
            paused = m_model.paused;
        }

        if (!paused) {
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - previous).count();
            previous = now;

            m_etw.drain(tracker);

            // Игра — это процесс активного окна, как в RTSS. Без этого на мониторе 144 Гц
            // композитор рабочего стола обгонял игру на 120 кадрах.
            DWORD foregroundPid = 0;
            if (GetForegroundWindow() != nullptr) {
                GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
            }
            if (foregroundPid == GetCurrentProcessId()) {
                foregroundPid = 0;
            }
            tracker.setForegroundProcess(static_cast<std::uint32_t>(foregroundPid));

            std::array<std::uint32_t, yawa::FrameTracker::kMaxStreams> streamPids{};
            const std::size_t streamCount = tracker.streamProcesses(streamPids.data(), streamPids.size());
            std::array<std::uint32_t, yawa::FrameTracker::kMaxStreams> systemPids{};
            std::size_t systemCount = 0;
            for (std::size_t index = 0; index < streamCount; ++index) {
                if (isSystemProcessCached(streamPids[index])) {
                    systemPids[systemCount++] = streamPids[index];
                }
            }
            tracker.setSystemProcesses(systemPids.data(), systemCount);

            yawa::SystemSnapshot snapshot;
            m_sensors.sample(snapshot);

            const yawa::SystemSampler::Deltas deltas = sampler.compute(snapshot, elapsed);
            const yawa::DerivedValues values = yawa::deriveValues(snapshot, deltas);
            const yawa::FpsSummary fps = tracker.summary(static_cast<double>(m_model.settings.capture.targetHz));

            const std::wstring foreground = foregroundProcessName();

            {
                std::lock_guard<std::mutex> guard(m_modelMutex);
                yawa::applySnapshot(m_model.store, snapshot, values, fps,
                    static_cast<std::uint8_t>(m_model.settings.capture.smoothingPercent));
                m_model.fps = fps;
                m_model.framesPresented = tracker.activePresents();
                m_model.availability = m_sensors.availability();
                m_model.cpuCores = m_sensors.coreCount();
                m_model.cpuThreads = m_sensors.cpuThreads();
                m_model.cpuName = m_sensors.cpuName();
                // Узкое место пересчитываем на том же снимке, что и экраны: отдельного
                // опроса железа нет, и цифры на «Балансе» всегда совпадают с «Обзором».
                m_model.balance = analyzeBottleneck(m_model.hardware,
                    loadFromStore(m_model.store, static_cast<double>(m_model.settings.capture.targetHz)));
                m_model.etwRunning = m_etw.isRunning();
                if (!foreground.empty()) {
                    m_model.foregroundApp = foreground;
                }
            }

            // В простое без кадров опрос можно замедлить: это и есть «экономия вне игры».
            if (fps.currentFps <= 0.0) {
                std::lock_guard<std::mutex> guard(m_modelMutex);
                if (m_model.settings.capture.pauseWhenIdle) {
                    interval = std::max(interval, 2000);
                }
            }

            PostMessageW(m_window, kTickMessage, 0, 0);
        }

        std::unique_lock<std::mutex> lock(m_wakeMutex);
        m_wake.wait_for(lock, std::chrono::milliseconds(interval),
            [this]() { return m_stopRequested.load(); });
    }

    m_etw.stop();
}

void MainWindow::onTick()
{
    static const auto started = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> guard(m_modelMutex);
        m_model.elapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count());
        m_model.overlayVisible = m_overlay.isVisible();
        m_overlay.setContent(m_model.store, m_model.fps,
            m_model.activeGpu < m_model.gpuNames.size() ? m_model.gpuNames[m_model.activeGpu] : L"");
    }

    m_overlay.repaint();

    // Свёрнутое или скрытое в трей окно не перерисовываем: это экономит CPU в простое.
    if (IsWindowVisible(m_window) != FALSE && IsIconic(m_window) == FALSE) {
        InvalidateRect(m_window, nullptr, FALSE);
    }
}

void MainWindow::createTrayIcon()
{
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = m_window;
    icon.uID = kTrayIconId;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = kTrayMessage;
    icon.hIcon = static_cast<HICON>(LoadImageW(m_instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (icon.hIcon == nullptr) {
        icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    wcscpy_s(icon.szTip, L"YawaFpsHub — мониторинг FPS и системы");

    m_trayCreated = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
}

void MainWindow::destroyTrayIcon()
{
    if (!m_trayCreated) {
        return;
    }

    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = m_window;
    icon.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    m_trayCreated = false;
}

void MainWindow::showTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1001, L"Открыть мониторинг");
    AppendMenuW(menu, MF_STRING, 1002, m_overlay.isVisible() ? L"Скрыть оверлей" : L"Показать оверлей");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1003, L"Выход");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(m_window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, cursor.x, cursor.y, 0, m_window, nullptr);
    DestroyMenu(menu);
    // Без этого меню иногда остаётся висеть: окно должно получить хотя бы одно сообщение.
    PostMessageW(m_window, WM_NULL, 0, 0);
}

void MainWindow::toggleVisibilityFromTray()
{
    if (IsWindowVisible(m_window)) {
        ShowWindow(m_window, SW_HIDE);
    } else {
        ShowWindow(m_window, SW_SHOWNORMAL);
        SetForegroundWindow(m_window);
    }
}

void MainWindow::registerHotkeys()
{
    UnregisterHotKey(m_window, kHotkeyOverlay);
    UnregisterHotKey(m_window, kHotkeyPosition);
    UnregisterHotKey(m_window, kHotkeyWindow);

    // Коды из настроек: ноль означает «клавиша отключена». MOD_NOREPEAT не даёт
    // автоповтору удерживаемой клавиши переключать оверлей десятки раз в секунду.
    const auto bind = [this](int id, unsigned int key) {
        if (key != 0 && RegisterHotKey(m_window, id, MOD_NOREPEAT, key) == FALSE && m_log != nullptr) {
            m_log->warning("хоткей " + std::to_string(key) + " занят другой программой");
        }
    };
    bind(kHotkeyOverlay, m_model.settings.hotkeys.overlay);
    bind(kHotkeyPosition, m_model.settings.hotkeys.position);
    bind(kHotkeyWindow, m_model.settings.hotkeys.window);
    bind(kHotkeyClickThrough, m_model.settings.hotkeys.clickThrough);
}

} // namespace yawa
