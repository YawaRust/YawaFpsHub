#include "platform/main_window.h"
#include "platform/app_paths.h"
#include "ui/chrome.h"
#include "ui/dashboard_view.h"
#include "ui/layout.h"
#include "ui/sensors_view.h"
#include "ui/settings_view.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>

namespace yawa {
namespace {

constexpr wchar_t kClassName[] = L"YawaFpsHubMain";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTickMessage = WM_APP + 2;
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

    m_window = CreateWindowExW(0, kClassName, L"YawaFpsHub", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        1280, 800, nullptr, nullptr, instance, this);

    if (m_window == nullptr) {
        error = "не удалось создать главное окно";
        return false;
    }

    // Тёмный системный заголовок: приложение не рисует рамку само, но и светлая полоса
    // рядом с тёмным интерфейсом выглядит чужой.
    const BOOL darkTitle = TRUE;
    DwmSetWindowAttribute(m_window, 20, &darkTitle, sizeof(darkTitle));

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
        m_overlay.setSettings(m_model.settings.overlay);
    }

    createTrayIcon();
    registerHotkeys();

    m_running = true;
    startSampling();
    return true;
}

int MainWindow::run()
{
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
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
        return 0;

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
        handleClick(x, y);
        return 0;
    }

    case WM_CHAR: {
        if (m_model.editingLabel >= 0) {
            const auto index = static_cast<std::size_t>(m_model.editingLabel);
            LabelField field = static_cast<LabelField>(index);
            std::string value = labelValue(m_model.settings.overlay, field);

            if (wParam == VK_BACK && !value.empty()) {
                value.pop_back();
                setLabel(m_model.settings.overlay, field, value.empty() ? " " : value);
            } else if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
                m_model.editingLabel = -1;
                saveSettings();
            } else if (wParam >= 32 && wParam < 127 && value.size() < 10) {
                value.push_back(static_cast<char>(wParam));
                setLabel(m_model.settings.overlay, field, value);
            }

            m_overlay.setSettings(m_model.settings.overlay);
            InvalidateRect(m_window, nullptr, FALSE);
        }
        return 0;
    }

    case kTickMessage:
        onTick();
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
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
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
            PostMessageW(m_window, WM_CLOSE, 0, 0);
            break;
        default:
            break;
        }
        return 0;
    }

    case WM_CLOSE:
        if (m_model.settings.ui.closeToTray) {
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
    const float top = kTitleBarHeight + kTabBarHeight + 12.0f;
    const float bottom = m_renderer.height() - kStatusBarHeight - 12.0f;
    return Rect{ 18.0f, top, m_renderer.width() - 18.0f, bottom };
}

std::vector<UiHit> MainWindow::buildHits()
{
    std::vector<UiHit> hits;
    drawTabs(m_renderer, m_model, m_theme, hits);

    const Rect area = contentArea();
    switch (m_model.view) {
    case View::Dashboard:
        drawDashboard(m_renderer, m_model, m_theme, area, hits);
        break;
    case View::Sensors:
        drawSensors(m_renderer, m_model, m_theme, area, hits);
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
    m_renderer.clear(m_theme.background);

    drawTitleBar(m_renderer, m_model, m_theme);
    m_hits = buildHits();
    drawStatusBar(m_renderer, m_model, m_theme);

    m_renderer.end();
}

void MainWindow::handleClick(float x, float y)
{
    if (m_model.editingLabel >= 0) {
        m_model.editingLabel = -1;
    }

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

void MainWindow::applyAction(const UiHit& hit)
{
    switch (hit.action) {
    case ActionKind::SwitchView:
        m_model.view = static_cast<View>(hit.value);
        break;

    case ActionKind::SetInterval:
        if (hit.value >= 30) {
            m_model.settings.capture.targetHz = hit.value;
        }
        if (hit.value == 500 || hit.value == 1000 || hit.value == 2000) {
            m_model.settings.capture.intervalMs = hit.value;
            std::lock_guard<std::mutex> guard(m_wakeMutex);
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

    case ActionKind::SelectLabel:
        m_model.editingLabel = hit.value;
        break;

    case ActionKind::ResetLabels:
        m_model.settings.overlay = OverlaySettings{};
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ToggleCompact:
        m_model.settings.ui.compactDensity = !m_model.settings.ui.compactDensity;
        saveSettings();
        break;

    case ActionKind::OpenSettingsFolder:
        revealInExplorer(executableDirectory());
        break;

    case ActionKind::ResetSettings:
        m_model.settings = defaultSettings();
        m_theme = makeTheme(m_model.settings.ui.accent);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ToggleOverlay:
        m_overlay.toggle();
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
        {
            std::lock_guard<std::mutex> guard(m_modelMutex);
            interval = m_model.settings.capture.intervalMs;
        }
        {
            std::unique_lock<std::mutex> lock(m_wakeMutex);
            m_wake.wait_for(lock, std::chrono::milliseconds(interval),
                [this]() { return m_stopRequested.load(); });
        }

        if (m_stopRequested) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - previous).count();
        previous = now;

        m_etw.drain(tracker);

        yawa::SystemSnapshot snapshot;
        m_sensors.sample(snapshot);

        const yawa::SystemSampler::Deltas deltas = sampler.compute(snapshot, elapsed);
        const yawa::DerivedValues values = yawa::deriveValues(snapshot, deltas);
        const yawa::FpsSummary fps = tracker.summary(static_cast<double>(m_model.settings.capture.targetHz));

        {
            std::lock_guard<std::mutex> guard(m_modelMutex);
            yawa::applySnapshot(m_model.store, snapshot, values, fps, static_cast<std::uint8_t>(
                                                                        m_model.settings.capture.smoothingPercent));
            m_model.fps = fps;
            m_model.availability = m_sensors.availability();
            m_model.etwRunning = m_etw.isRunning();
        }

        PostMessageW(m_window, kTickMessage, 0, 0);
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
        m_overlay.setContent(m_model.store, m_model.fps,
            m_model.activeGpu < m_model.gpuNames.size() ? m_model.gpuNames[m_model.activeGpu] : L"");
    }

    m_overlay.repaint();

    InvalidateRect(m_window, nullptr, FALSE);
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
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, m_window, nullptr);
    DestroyMenu(menu);
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
