#include "platform/main_window.h"
#include "platform/app_paths.h"
#include "platform/win_util.h"
#include "ui/chrome.h"
#include "ui/overlay_view.h"
#include "ui/dashboard_view.h"
#include "ui/layout.h"
#include "ui/sensors_view.h"
#include "ui/settings_view.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>

namespace yawa {
namespace {

constexpr wchar_t kClassName[] = L"YawaFpsHubMain";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTickMessage = WM_APP + 2;
constexpr UINT kExportMessage = WM_APP + 3;
constexpr UINT kTrayIconId = 1;
constexpr UINT kHotkeyOverlay = 1;
constexpr UINT kHotkeyPosition = 2;
constexpr UINT kHotkeyWindow = 3;

constexpr int kHotkeyToggleOverlay = 1;
constexpr int kHotkeyCyclePosition = 2;

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
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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

    m_model.settingsPath = settingsPath();
    m_theme = makeTheme(m_model.settings.ui.accent);

    if (!m_sensors.initialize(log, error)) {
        return false;
    }

    const GpuRegistry& registry = m_sensors.gpus();
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
        m_overlay.setSettings(m_model.settings.overlay);
    }

    m_model.settingsPath = settingsPath();
    m_model.logPath = logPath();
    m_model.sessionsPath = sessionsDirectory();

    createTrayIcon();
    registerHotkeys();

    // Первый запуск показывает окно сразу: приложение живёт в трее только после того,
    // как пользователь сам свернул его или включил «сворачивать в трей».
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

void MainWindow::setOverlayVisible(bool visible)
{
    if (visible != m_overlay.isVisible()) {
        m_overlay.toggle();
    }
    m_model.overlayVisible = m_overlay.isVisible();
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
        // Ниже этого размера карточки начинают наезжать друг на друга.
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = static_cast<LONG>(980.0f * m_dpiScale);
        info->ptMinTrackSize.y = static_cast<LONG>(680.0f * m_dpiScale);
        return 0;
    }

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
        const LRESULT base = DefWindowProcW(m_window, message, wParam, lParam);
        if (base != HTCLIENT) {
            return base;
        }

        POINT cursor{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(m_window, &cursor);
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
            if (hit.action == ActionKind::SetScale || hit.action == ActionKind::SetOpacity) {
                m_dragAction = hit.action;
                m_dragRect = hit.rect;
                SetCapture(m_window);
                applySlider(hit.action, hit.rect, x);
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
        if (inTitleBar || wasInTitleBar) {
            InvalidateRect(m_window, nullptr, FALSE);
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

    case WM_MOUSELEAVE:
        m_trackingMouse = false;
        m_model.cursorInside = false;
        InvalidateRect(m_window, nullptr, FALSE);
        return 0;

    case WM_MOUSEWHEEL: {
        if (m_model.view != View::Sensors) {
            break;
        }
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        m_model.sensorScroll = std::clamp(m_model.sensorScroll - delta / WHEEL_DELTA * 3, 0, 4096);
        InvalidateRect(m_window, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP:
        if (m_dragAction == ActionKind::SetScale || m_dragAction == ActionKind::SetOpacity) {
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
        if (wParam == kHotkeyOverlay) {
            m_overlay.toggle();
        } else if (wParam == kHotkeyPosition) {
            const int next = (static_cast<int>(m_model.settings.overlay.position) + 1) % 4;
            m_model.settings.overlay.position = static_cast<OverlayPosition>(next);
            m_overlay.setSettings(m_model.settings.overlay);
            saveSettings();
            InvalidateRect(m_window, nullptr, FALSE);
        } else if (wParam == kHotkeyWindow) {
            toggleVisibilityFromTray();
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
            m_overlay.toggle();
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
    return Rect{ kRailWidth + 20.0f, top, m_renderer.width() - 20.0f, bottom };
}

std::vector<UiHit> MainWindow::buildHits()
{
    std::vector<UiHit> hits;

    const Rect area = contentArea();
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
    }
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
    m_hits = buildHits();
    drawRail(m_renderer, m_model, m_theme, m_hits);
    drawTitleBar(m_renderer, m_model, m_theme, m_hits);
    drawTopBar(m_renderer, m_model, m_theme, m_hits);
    drawStatusBar(m_renderer, m_model, m_theme);

    m_renderer.end();
}

void MainWindow::handleClick(float x, float y)
{
    if (m_model.editingLabel >= 0) {
        m_model.editingLabel = -1;
    }
    m_model.sensorSearchActive = false;

    for (const UiHit& hit : m_hits) {
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

void MainWindow::applySlider(ActionKind action, const Rect& rect, float x)
{
    if (rect.width() <= 0.0f) {
        return;
    }

    const float fraction = std::clamp((x - rect.left) / rect.width(), 0.0f, 1.0f);
    UiHit hit{ rect, action, 0 };

    if (action == ActionKind::SetScale) {
        // Шаг 5 %: значения остаются ровными и хорошо читаются в ini.
        hit.value = 70 + static_cast<int>(std::lround(fraction * 90.0f / 5.0f)) * 5;
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
        m_model.view = static_cast<View>(hit.value);
        break;

    case ActionKind::SetTargetHz:
        m_model.settings.capture.targetHz = std::clamp(hit.value, 30, 480);
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
        m_model.settings.overlay.style = static_cast<OverlayStyle>(hit.value);
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

    case ActionKind::CycleGpu: {
        const std::size_t count = m_model.gpuNames.size();
        if (count == 0) {
            break;
        }
        const std::size_t next = hit.value > 0 ? static_cast<std::size_t>(hit.value) : (m_model.activeGpu + 1) % count;
        m_sensors.selectGpu(next);
        m_model.activeGpu = m_sensors.activeGpu();
        const GpuAdapter* adapter = m_sensors.gpus().byIndex(m_model.activeGpu);
        if (adapter != nullptr) {
            m_model.settings.gpu.hasSelection = true;
            m_model.settings.gpu.luidLow = adapter->luidLow;
            m_model.settings.gpu.luidHigh = adapter->luidHigh;
        }
        saveSettings();
        break;
    }

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
        m_overlay.toggle();
        m_model.overlayVisible = m_overlay.isVisible();
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
                m_model.availability = m_sensors.availability();
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
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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
    RegisterHotKey(m_window, kHotkeyOverlay, 0, VK_F10);
    RegisterHotKey(m_window, kHotkeyPosition, 0, VK_F11);
    RegisterHotKey(m_window, kHotkeyWindow, 0, VK_F12);
}

} // namespace yawa
