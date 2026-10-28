#include "platform/main_window.h"
#include "metrics/overlay_metric.h"
#include "metrics/report_frame.h"
#include "platform/app_paths.h"
#include "platform/changelog_win.h"
#include "platform/display_info.h"
#include "platform/hardware_info_win.h"
#include "platform/overlay_monitor.h"
#include "platform/pawnio_package.h"
#include "platform/report_file_win.h"
#include "platform/report_preview_win.h"
#include "platform/report_viewer_win.h"
#include "platform/win_util.h"
#include "render/overlay_painter.h"
#include "report/package.h"
#include "ui/bottleneck_view.h"
#include "ui/canvas_transform.h"
#include "ui/chrome.h"
#include "ui/dashboard_view.h"
#include "ui/layout.h"
#include "ui/overlay_view.h"
#include "ui/sensors_view.h"
#include "ui/settings_view.h"
#include "ui/system_view.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <memory>

namespace yawa {
namespace {

    constexpr wchar_t kClassName[] = L"YawaFpsHubMain";
    constexpr UINT kTrayMessage = WM_APP + 1;
    constexpr UINT kTickMessage = WM_APP + 2;
    constexpr UINT kExportMessage = WM_APP + 3;
    constexpr UINT kSensorElevationMessage = WM_APP + 4;
    constexpr UINT kHardwareInfoMessage = WM_APP + 5;
    constexpr UINT kSensorInitMessage = WM_APP + 6;

    struct HardwareReport
    {
        HardwareIdentity hardware;
        std::string error;
        bool success = false;
    };

    struct SensorInitReport
    {
        std::optional<AmdDdr4Timings> memoryTimings;
        std::vector<std::wstring> gpuNames;
        GpuSelection gpu;
        std::size_t activeGpu = 0;
    };
    constexpr UINT kTrayIconId = 1;
    // DWMWA_BORDER_COLOR = 34, DWMWA_COLOR_NONE = 0xFFFFFFFE. На Windows 10 атрибут
    // неизвестен, вызов просто вернёт ошибку — там рамку убирает растянутая рамка DWM.
    constexpr DWORD kDwmBorderColor = 34;
    constexpr COLORREF kDwmColorNone = 0xFFFFFFFE;

    bool chooseOverlayRgba(HWND owner, std::optional<std::uint32_t>& rgba)
    {
        static COLORREF custom[16] { };
        const std::uint32_t current = rgba.value_or(0xFFFFFFFFu);
        CHOOSECOLORW picker { };
        picker.lStructSize = sizeof(picker);
        picker.hwndOwner = owner;
        picker.lpCustColors = custom;
        picker.rgbResult = RGB((current >> 24) & 0xFFu, (current >> 16) & 0xFFu, (current >> 8) & 0xFFu);
        picker.Flags = CC_RGBINIT | CC_FULLOPEN;
        if (!ChooseColorW(&picker)) {
            return false;
        }
        rgba = (static_cast<std::uint32_t>(GetRValue(picker.rgbResult)) << 24)
            | (static_cast<std::uint32_t>(GetGValue(picker.rgbResult)) << 16)
            | (static_cast<std::uint32_t>(GetBValue(picker.rgbResult)) << 8) | (current & 0xFFu);
        return true;
    }

    void removeDwmBorder(HWND window)
    {
        DwmSetWindowAttribute(window, kDwmBorderColor, &kDwmColorNone, sizeof(kDwmColorNone));
    }
    // Идентификаторы WM_HOTKEY: RegisterHotKey принимает int, поэтому и константы int.
    constexpr int kHotkeyOverlay = 1;
    constexpr int kHotkeyPosition = 2;
    constexpr int kHotkeyWindow = 3;
    constexpr int kHotkeyClickThrough = 4;
    constexpr int kHotkeyEmergencyExit = 5;
    constexpr int kHotkeyMenuScene = 6;

    std::string gameOverlayPath(std::string_view gameKey)
    {
        return dataDirectory() + "\\game-profiles\\" + std::string(gameKey) + ".ini";
    }

    bool writeGameOverlay(std::string_view gameKey, const OverlaySettings& overlay)
    {
        ensureDirectory(dataDirectory() + "\\game-profiles");
        Settings profile = defaultSettings();
        profile.overlay = overlay;
        copyOverlayPlacement(profile.overlay, defaultSettings().overlay);
        return writeTextFile(gameOverlayPath(gameKey), serializeSettings(profile));
    }

    std::vector<std::pair<std::string, RECT>> overlayMonitors()
    {
        std::vector<std::pair<std::string, RECT>> result;
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL {
                auto& monitors = *reinterpret_cast<std::vector<std::pair<std::string, RECT>>*>(context);
                MONITORINFOEXW info { };
                info.cbSize = sizeof(info);
                if (GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info))) {
                    monitors.emplace_back(monitorIdentity(info.szDevice), info.rcMonitor);
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&result));
        return result;
    }

} // namespace

MainWindow::~MainWindow()
{
    // Некоторые драйверы не завершают отменённый IOCTL (в т. ч. SMART/NVMe), а
    // shutdown() ждёт поток чтения. После 15 секунд даём приложению завершить себя,
    // не оставляя пользователя с невидимым зависшим процессом до перезагрузки.
    auto stopped = std::make_shared<std::atomic<bool>>(false);
    std::thread([stopped] {
        Sleep(15000);
        if (!stopped->load()) {
            TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
        }
    }).detach();
    stopSampling();
    destroyTrayIcon();
    m_overlayEditor.close();
    m_overlay.destroy();
    m_sensors.shutdown();
    m_renderer.shutdown();
    stopped->store(true);
}

bool MainWindow::create(HINSTANCE instance, Log& log, std::string& error)
{
    m_instance = instance;
    m_log = &log;

    WNDCLASSEXW windowClass { };
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
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
        1280, 800, nullptr, nullptr, instance, this);

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
    const MARGINS glass { -1, -1, -1, -1 };
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

    m_baseOverlay = m_model.settings.overlay;

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
        m_overlay.setMoveHandler([this](int x, int y, bool finished) {
            {
                std::lock_guard<std::mutex> guard(m_modelMutex);
                auto& overlay = m_model.settings.overlay;
                if (overlay.anchorMode == OverlayAnchorMode::Free) {
                    overlay.originX = x;
                    overlay.originY = y;
                } else {
                    overlay.offsetX = x;
                    overlay.offsetY = y;
                }
            }
            if (finished) {
                if (m_model.settings.overlay.anchorMode == OverlayAnchorMode::Free) {
                    rememberOverlayMonitor(m_model.settings.overlay, overlayVirtualScreen());
                    m_overlay.setSettings(m_model.settings.overlay);
                }
                saveSettings();
            }
            InvalidateRect(m_window, nullptr, FALSE);
        });
        m_overlay.setDirectEditHandler([this](int scale, int padding, int rowGap, bool finished) {
            {
                std::lock_guard<std::mutex> guard(m_modelMutex);
                auto& overlay = m_model.settings.overlay;
                overlay.scalePercent = scale;
                overlay.paddingPx = padding;
                overlay.rowGapPx = rowGap;
            }
            if (finished) {
                saveSettings();
            }
            m_overlayEditor.refresh();
            InvalidateRect(m_window, nullptr, FALSE);
        });
        m_overlay.setLayerEditHandler([this](std::size_t index, int x, int y, bool finished) {
            if (index >= m_model.settings.overlay.rtssLayerEdits.size()) {
                return;
            }
            {
                std::lock_guard<std::mutex> guard(m_modelMutex);
                auto& edit = m_model.settings.overlay.rtssLayerEdits[index];
                edit.x = x;
                edit.y = y;
            }
            if (finished) {
                saveSettings();
            }
            m_overlayEditor.refresh();
            InvalidateRect(m_window, nullptr, FALSE);
        });
        m_overlay.setRowEditHandler([this](MetricId id, OverlayRowPosition position, bool finished) {
            {
                std::lock_guard<std::mutex> guard(m_modelMutex);
                m_model.settings.overlay.rowPositions[static_cast<std::size_t>(id)] = position;
            }
            if (finished) {
                saveSettings();
            }
            m_overlayEditor.refresh();
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
    RECT client { };
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

void MainWindow::scheduleSensorElevation()
{
    PostMessageW(m_window, kSensorElevationMessage, 0, 0);
}

void MainWindow::openEditorForSmoke()
{
    applyAction(UiHit { { }, ActionKind::OpenOverlayEditor, 0 });
}

void MainWindow::exportOverlayStyles()
{
    if (m_overlayShotDir.empty()) {
        return;
    }

    static constexpr std::array<std::wstring_view, 5> kStyles { L"glass", L"neon", L"capsule", L"terminal",
        L"minimal" };

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
    m_model.menuScene = false;
    m_sceneWasVisible = false;
    m_overlay.setMenuSceneOpacity(std::nullopt);
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

void MainWindow::refreshMenuScene()
{
    const bool menu = m_model.menuScene;
    const int opacity = m_model.settings.overlay.menuOpacityPercent;
    m_overlay.setMenuSceneOpacity(menu ? std::optional<int>(opacity) : std::nullopt);
    if (menu && opacity == 0 && m_overlay.isVisible()) {
        m_overlay.hide();
    } else if (m_sceneWasVisible && (!menu || opacity > 0) && !m_overlay.isVisible()) {
        std::string error;
        if (!m_overlay.show(&error) && m_log != nullptr) {
            m_log->warning("смена сцены оверлея: " + error);
        }
    }
    m_model.overlayVisible = m_overlay.isVisible();
    InvalidateRect(m_window, nullptr, FALSE);
    m_overlayEditor.refresh();
}

void MainWindow::toggleMenuScene()
{
    if (!m_model.menuScene) {
        m_sceneWasVisible = m_overlay.isVisible();
    }
    m_model.menuScene = !m_model.menuScene;
    refreshMenuScene();
    if (!m_model.menuScene) {
        m_sceneWasVisible = false;
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

bool MainWindow::smokeImportRtss(std::string_view package, std::string& error)
{
    OverlaySettings imported = m_model.settings.overlay;
    const bool parsed = package.starts_with("0CDU") ? importRtssExchange(package, imported, error)
                                                    : importRtssOverlay(package, imported, error);
    if (!parsed) {
        return false;
    }
    imported.opacityPercent = 100;
    m_overlay.setSettings(imported);
    if (!m_overlay.show(&error)) {
        return false;
    }
    if (!m_overlay.hasVisiblePixels()) {
        error = "импорт RTSS создал прозрачный кадр";
        return false;
    }
    if (imported.rtssVisual) {
        const auto& layers = imported.rtssVisual->layers;
        if (std::any_of(layers.begin(), layers.end(), [](const RtssVisualLayer& layer) {
                return layer.text.starts_with("<G=Frametime,");
            })) {
            MetricStore samples;
            for (int index = 0; index < 32; ++index) {
                samples.set(MetricId::FrameTime, 8.0 + index % 9);
            }
            m_overlay.setContent(samples, FpsSummary{}, L"");
            m_overlay.repaint();
            if (!m_overlay.hasVisiblePixels()) {
                error = "RTSS-график с локальными данными не отрисован";
                return false;
            }
        }
        OverlaySettings hidden = imported;
        for (std::size_t index = 0; index < hidden.rtssVisual->layers.size(); ++index) {
            hidden.rtssLayerEdits[index].visible = false;
        }
        m_overlay.setSettings(hidden);
        m_overlay.repaint();
        const bool blankWhenHidden = !m_overlay.hasVisiblePixels();
        m_overlay.setSettings(imported);
        m_overlay.repaint();
        if (!blankWhenHidden || !m_overlay.hasVisiblePixels()) {
            error = "не прошла проверка видимости RTSS-слоёв";
            return false;
        }
    }
    m_model.settings.overlay = std::move(imported);
    m_model.overlayVisible = true;
    return true;
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

    MSG message { };
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
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(m_window, &cursor);

        // Края тянем сами: у окна без системного заголовка DefWindowProc не отдаёт
        // HTLEFT/HTBOTTOM, и за края изменить размер было нельзя.
        if (IsZoomed(m_window) == FALSE) {
            const LONG border = static_cast<LONG>(8.0f * m_dpiScale);
            RECT client { };
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
        PAINTSTRUCT paint { };
        BeginPaint(m_window, &paint);
        this->paint();
        EndPaint(m_window, &paint);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case kSensorInitMessage: {
        std::unique_ptr<SensorInitReport> report(reinterpret_cast<SensorInitReport*>(lParam));
        {
            std::lock_guard<std::mutex> guard(m_modelMutex);
            m_model.gpuNames = std::move(report->gpuNames);
            m_model.memoryTimings = report->memoryTimings;
            m_model.activeGpu = report->activeGpu;
            // Сохраняем только восстановленный из конфигурации LUID, не переписывая
            // выбор, который пользователь успел сделать после начала опроса.
            if (!m_model.settings.gpu.hasSelection) {
                m_model.settings.gpu = report->gpu;
            }
        }
        InvalidateRect(m_window, nullptr, FALSE);
        return 0;
    }

    case kHardwareInfoMessage: {
        std::unique_ptr<HardwareReport> report(reinterpret_cast<HardwareReport*>(lParam));
        if (report->success) {
            const CpuIdentity& cpu = report->hardware.cpu;
            if (m_log != nullptr) {
                m_log->info("железо: " + (cpu.name.empty() ? std::string("CPU не определён") : cpu.name)
                    + ", ядер " + std::to_string(cpu.physicalCores) + "/"
                    + std::to_string(cpu.logicalProcessors) + ", модулей "
                    + std::to_string(report->hardware.memory.modules.size()));
            }
            std::lock_guard<std::mutex> guard(m_modelMutex);
            m_model.hardware = std::move(report->hardware);
        } else if (m_log != nullptr) {
            m_log->warning("сведения о системе: " + report->error);
        }
        InvalidateRect(m_window, nullptr, FALSE);
        return 0;
    }

    case kSensorElevationMessage:
        if (wParam == 0) {
            requestSensorElevation(m_window, kSensorElevationMessage);
        } else if (wParam == 1) {
            m_quitRequested = true;
            PostMessageW(m_window, WM_CLOSE, 0, 0);
        } else if (m_log != nullptr) {
            m_log->warning(
                "повышение прав для датчиков не завершено: " + win32ErrorText(static_cast<DWORD>(lParam)));
        }
        return 0;

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
            RECT client { };
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
            if (hit.action == ActionKind::ReorderMetric) {
                m_dragAction = ActionKind::ReorderMetric;
                m_dragMetric = static_cast<MetricId>(hit.value);
                SetCapture(m_window);
                return 0;
            }
            if (hit.action == ActionKind::DragOverlay) {
                m_dragAction = ActionKind::DragOverlay;
                m_dragStartX = x;
                m_dragStartY = y;
                m_dragOffsetX = m_model.settings.overlay.anchorMode == OverlayAnchorMode::Free
                    ? m_model.settings.overlay.originX
                    : m_model.settings.overlay.offsetX;
                m_dragOffsetY = m_model.settings.overlay.anchorMode == OverlayAnchorMode::Free
                    ? m_model.settings.overlay.originY
                    : m_model.settings.overlay.offsetY;
                m_dragRect = hit.rect;
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
                m_model.contentScroll
                    = std::clamp(m_model.contentScroll + (forward ? step : -step), 0.0f, limit);
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
            TRACKMOUSEEVENT track { };
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
            const float limit = m_draggingSensors
                ? m_model.sensorScrollMax
                : std::max(contentDesignHeight() - contentArea().height(), 0.0f);
            // Геометрия полосы берётся из сохранённой дорожки и текущего вылета бегунка.
            const Rect& track = m_scrollTrack;
            const float viewportHeight
                = m_draggingSensors ? m_model.sensorViewportHeight : contentArea().height();
            const float designHeight
                = m_draggingSensors ? std::max(viewportHeight + limit, 1.0f) : contentDesignHeight();
            const float trackHeight = track.height();
            const float thumbHeight
                = std::max(m_draggingSensors ? 24.0f : 40.0f, trackHeight * viewportHeight / designHeight);
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

        if (m_dragAction == ActionKind::ReorderMetric) {
            for (const UiHit& hit : m_hits) {
                if (hit.action == ActionKind::ReorderMetric && hit.value != static_cast<int>(m_dragMetric)
                    && hit.rect.contains(cursorX, cursorY)) {
                    moveOverlayMetric(
                        m_model.settings.overlay, m_dragMetric, static_cast<MetricId>(hit.value));
                    m_overlay.setSettings(m_model.settings.overlay);
                    InvalidateRect(m_window, nullptr, FALSE);
                    break;
                }
            }
            return 0;
        }

        if (m_dragAction == ActionKind::DragOverlay) {
            const float x = static_cast<float>(GET_X_LPARAM(lParam)) / m_dpiScale;
            const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / m_dpiScale;
            auto& overlay = m_model.settings.overlay;
            if (overlay.anchorMode == OverlayAnchorMode::Free) {
                overlay.originX = std::clamp(m_dragOffsetX
                        + scaledOverlayOffset(static_cast<int>(std::lround(x - m_dragStartX)),
                            m_dragRect.width(), GetSystemMetrics(SM_CXVIRTUALSCREEN)),
                    0, 32768);
                overlay.originY = std::clamp(m_dragOffsetY
                        + scaledOverlayOffset(static_cast<int>(std::lround(y - m_dragStartY)),
                            m_dragRect.height(), GetSystemMetrics(SM_CYVIRTUALSCREEN)),
                    0, 32768);
            } else {
                overlay.offsetX = std::clamp(
                    m_dragOffsetX + static_cast<int>(std::lround(x - m_dragStartX)), -8192, 8192);
                overlay.offsetY = std::clamp(
                    m_dragOffsetY + static_cast<int>(std::lround(y - m_dragStartY)), -8192, 8192);
            }
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
            POINT cursor { };
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
            if (m_dragAction == ActionKind::DragOverlay
                && m_model.settings.overlay.anchorMode == OverlayAnchorMode::Free) {
                rememberOverlayMonitor(m_model.settings.overlay, overlayVirtualScreen());
                m_overlay.setSettings(m_model.settings.overlay);
            }
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
            m_model.settings.overlay.anchorMode = OverlayAnchorMode::Corner;
            m_overlay.setSettings(m_model.settings.overlay);
            saveSettings();
            InvalidateRect(m_window, nullptr, FALSE);
        } else if (wParam == static_cast<WPARAM>(kHotkeyWindow)) {
            toggleVisibilityFromTray();
        } else if (wParam == static_cast<WPARAM>(kHotkeyEmergencyExit)) {
            m_quitRequested = true;
            PostMessageW(m_window, WM_CLOSE, 0, 0);
        } else if (wParam == static_cast<WPARAM>(kHotkeyMenuScene)) {
            toggleMenuScene();
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
    return Rect { kRailWidth + 30.0f, top, m_renderer.width() - 22.0f, bottom };
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
        return 430.0f + kGap + 158.0f + kGap;
    case View::Settings:
        return kSettingsHeight;
    case View::Bottleneck:
        return balanceHeight(m_model.balance.advice.size(), contentArea().width() < kDashboardNarrowWidth);
    case View::System:
        return systemHeight(contentArea().width() < kDashboardNarrowWidth);
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
    const Rect track { viewport.right + 4.0f, trackTop, viewport.right + 11.0f, trackBottom };
    const Rect thumb { track.left, thumbTop, track.right, thumbTop + thumbHeight };
    const bool hovered = isHovered(m_model, track) || m_draggingScroll;
    m_renderer.fillRoundRect(track, 3.0f, m_theme.line.withAlpha(hovered ? 0.55f : 0.28f));
    m_renderer.fillRoundRect(
        thumb, 3.0f, m_theme.lineStrong.withAlpha(hovered || m_draggingScroll ? 1.0f : 0.8f));

    hits.push_back(UiHit { Rect { track.left - 2.0f, thumbTop, track.right + 2.0f, thumbTop + thumbHeight },
        ActionKind::ScrollContentThumb, 0 });
    hits.push_back(UiHit { track, ActionKind::ScrollContentTrack, 0 });
    m_scrollTrack = track;
}

std::vector<UiHit> MainWindow::buildHits()
{
    std::vector<UiHit> hits;

    const Rect viewport = contentArea();
    // Экран рисуется в демо-высоте и прокручивается: на маленьком окне видно меньше
    // карточек, но их размеры те же, что в прототипе.
    m_model.contentHeight = std::max(viewport.height(), contentDesignHeight());
    const Rect area { viewport.left, viewport.top, viewport.right, viewport.top + m_model.contentHeight };
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

    {
        std::lock_guard<std::mutex> guard(m_modelMutex);
        m_model.activeGpu = index;
        const GpuAdapter* adapter = m_sensors.gpus().byIndex(index);
        if (adapter != nullptr) {
            m_model.settings.gpu.hasSelection = true;
            m_model.settings.gpu.luidLow = adapter->luidLow;
            m_model.settings.gpu.luidHigh = adapter->luidHigh;
        }
        m_requestedGpu = index;
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
    UiHit hit { rect, action, 0 };

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
        applyOverlayRecipe(m_model.settings.overlay, static_cast<OverlayRecipe>(hit.value));
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        InvalidateRect(m_window, nullptr, FALSE);
        break;

    case ActionKind::CaptureHotkey:
        m_model.hotkeyEditing = std::clamp(hit.value, 0, 3);
        break;

    case ActionKind::ToggleOverlayBackground:
        m_model.settings.overlay.surfaceActive = false;
        m_model.settings.overlay.showBackground = !m_model.settings.overlay.showBackground;
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ToggleOverlayBorder:
        m_model.settings.overlay.surfaceActive = false;
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
        m_model.settings.overlay.surfaceActive = false;
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
        m_model.settings.overlay.anchorMode = OverlayAnchorMode::Corner;
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ToggleFollowGameMonitor:
        m_model.settings.overlay.followGameMonitor = !m_model.settings.overlay.followGameMonitor;
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::CycleOverlayMonitor: {
        const auto monitors = overlayMonitors();
        if (monitors.empty()) {
            break;
        }
        auto& placement = m_model.settings.overlay;
        std::size_t next = 0;
        for (std::size_t index = 0; index < monitors.size(); ++index) {
            if (monitors[index].first == placement.savedMonitorId) {
                next = (index + 1) % monitors.size();
                break;
            }
        }
        const RECT& chosen = monitors[next].second;
        const int localX = placement.anchorMode == OverlayAnchorMode::Free ? placement.monitorOriginX : 16;
        const int localY = placement.anchorMode == OverlayAnchorMode::Free ? placement.monitorOriginY : 16;
        placement.anchorMode = OverlayAnchorMode::Free;
        placement.followGameMonitor = false;
        placement.savedMonitorId = monitors[next].first;
        placement.monitorOriginX = std::clamp(localX, 0, static_cast<int>(chosen.right - chosen.left - 1));
        placement.monitorOriginY = std::clamp(localY, 0, static_cast<int>(chosen.bottom - chosen.top - 1));
        const RECT virtualScreen = overlayVirtualScreen();
        placement.originX = originFromMonitor(placement.monitorOriginX, chosen.left, virtualScreen.left);
        placement.originY = originFromMonitor(placement.monitorOriginY, chosen.top, virtualScreen.top);
        m_overlay.setSettings(placement);
        saveSettings();
        break;
    }

    case ActionKind::CycleSparkline: {
        auto& overlay = m_model.settings.overlay;
        if (!overlay.showSparkline) {
            overlay.showSparkline = true;
            overlay.sparklineMetric = MetricId::Fps;
        } else if (overlay.sparklineMetric == MetricId::Fps) {
            overlay.sparklineMetric = MetricId::CpuLoad;
        } else if (overlay.sparklineMetric == MetricId::CpuLoad) {
            overlay.sparklineMetric = MetricId::GpuLoad;
        } else if (overlay.sparklineMetric == MetricId::GpuLoad) {
            overlay.sparklineMetric = MetricId::FrameTime;
        } else {
            overlay.showSparkline = false;
        }
        m_overlay.setSettings(overlay);
        saveSettings();
        break;
    }

    case ActionKind::ToggleMenuScene:
        toggleMenuScene();
        break;

    case ActionKind::ToggleGameProfile: {
        const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                .count());
        if (m_recentGameKey.empty() || m_recentGameAtMs == 0 || nowMs < m_recentGameAtMs
            || nowMs - m_recentGameAtMs > 300000) {
            break;
        }
        if (m_profileActive && m_activeGameKey == m_recentGameKey) {
            const std::string path = gameOverlayPath(m_activeGameKey);
            if (DeleteFileW(widen(path).c_str()) == FALSE) {
                MessageBoxW(
                    m_window, L"Не удалось удалить профиль игры.", L"YawaFpsHub", MB_OK | MB_ICONERROR);
                break;
            }
            m_profileActive = false;
            m_model.hasGameProfile = false;
            m_model.settings.overlay = m_baseOverlay;
        } else {
            if (!writeGameOverlay(m_recentGameKey, m_model.settings.overlay)) {
                MessageBoxW(m_window,
                    L"Не удалось сохранить профиль игры. Проверьте доступ к каталогу данных.", L"YawaFpsHub",
                    MB_OK | MB_ICONERROR);
                break;
            }
            m_activeGameKey = m_recentGameKey;
            m_profileActive = true;
            m_model.hasGameProfile = true;
        }
        m_overlay.setSettings(m_model.settings.overlay);
        if (m_model.menuScene) {
            refreshMenuScene();
        }
        saveSettings();
        m_overlayEditor.refresh();
        break;
    }

    case ActionKind::SetLayout:
        m_model.settings.overlay.layout = static_cast<OverlayLayout>(hit.value);
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetOverlaySurface:
        setOverlaySurface(m_model.settings.overlay, static_cast<OverlaySurface>(hit.value));
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
        const LabelField field
            = static_cast<LabelField>(std::clamp(hit.value, 0, static_cast<int>(kLabelCount) - 1));
        m_labelBackup = labelValue(m_model.settings.overlay, field);
        // Поле очищается сразу: иначе новое название пришлось бы стирать посимвольно.
        // Esc возвращает прежнюю подпись.
        setLabel(m_model.settings.overlay, field, std::string { });
        m_model.editingLabel = hit.value;
        m_overlay.setSettings(m_model.settings.overlay);
        break;
    }

    case ActionKind::ClearLabel: {
        const LabelField field
            = static_cast<LabelField>(std::clamp(hit.value, 0, static_cast<int>(kLabelCount) - 1));
        setLabel(m_model.settings.overlay, field, std::string { });
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;
    }

    case ActionKind::ApplyLabelPreset:
        m_model.settings.overlay.labels = hit.value == 0
            ? std::array<std::string, kLabelCount> { "FPS", "CPU", "GPU", "RAM", "VRAM" }
            : hit.value == 1 ? std::array<std::string, kLabelCount> { "КАДРЫ", "ЦП", "ГП", "ОЗУ", "ВИДЕО" }
                             : std::array<std::string, kLabelCount> { "", "", "", "", "" };
        m_model.editingLabel = -1;
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ResetOverlayPlacement:
        m_model.settings.overlay.anchorMode = OverlayAnchorMode::Corner;
        m_model.settings.overlay.offsetX = 0;
        m_model.settings.overlay.offsetY = 0;
        m_model.settings.overlay.scalePercent = 100;
        m_model.settings.overlay.opacityPercent = 72;
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ResetLabels:
        // Сбрасываем только подписи: остальные настройки оверлея не трогаем.
        m_model.settings.overlay.labels = OverlaySettings { }.labels;
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
        revealInExplorer(dataDirectory());
        break;

    case ActionKind::ShowChangelog:
        if (!openEmbeddedChangelog(m_window)) {
            MessageBoxW(
                m_window, L"Встроенный список изменений недоступен.", L"YawaFpsHub", MB_OK | MB_ICONWARNING);
        }
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

    case ActionKind::SetMenuOpacity:
        m_model.settings.overlay.menuOpacityPercent = std::clamp(hit.value, 0, 100);
        if (m_model.menuScene) {
            refreshMenuScene();
        }
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
        if (hit.value == kOverlayColorCount) {
            if (!chooseOverlayRgba(GetActiveWindow(), m_model.settings.overlay.valueRgba))
                break;
        } else {
            m_model.settings.overlay.valueColor = std::clamp(hit.value, -1, kOverlayColorCount - 1);
            m_model.settings.overlay.valueRgba.reset();
        }
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetLabelColor:
        if (hit.value == kOverlayColorCount) {
            if (!chooseOverlayRgba(GetActiveWindow(), m_model.settings.overlay.labelRgba))
                break;
        } else {
            m_model.settings.overlay.labelColor = std::clamp(hit.value, -1, kOverlayColorCount - 1);
            m_model.settings.overlay.labelRgba.reset();
        }
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetAccentColor:
        if (hit.value == kOverlayColorCount) {
            if (!chooseOverlayRgba(GetActiveWindow(), m_model.settings.overlay.accentRgba))
                break;
        } else {
            m_model.settings.overlay.accentColor = std::clamp(hit.value, -1, kOverlayColorCount - 1);
            m_model.settings.overlay.accentRgba.reset();
        }
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::SetRowColor: {
        const int field = hit.value / 8;
        const int color = hit.value % 8 - 1;
        if (field >= 0 && field < static_cast<int>(kLabelCount)) {
            auto& overlay = m_model.settings.overlay;
            const auto index = static_cast<std::size_t>(field);
            if (color == kOverlayColorCount) {
                if (!chooseOverlayRgba(GetActiveWindow(), overlay.rowRgba[index]))
                    break;
            } else {
                overlay.rowColors[index] = std::clamp(color, -1, kOverlayColorCount - 1);
                overlay.rowRgba[index].reset();
            }
            m_overlay.setSettings(overlay);
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

    case ActionKind::EnableCpuSensors:
        scheduleSensorElevation();
        break;

    case ActionKind::ToggleSensor:
        if (hit.value >= 0 && hit.value < static_cast<int>(kMetricCount)) {
            auto& selected = m_model.settings.overlay.metrics[static_cast<std::size_t>(hit.value)];
            bool active = selected;
            for (MetricId detail : kPairedMetricDetails) {
                if (displayMetric(detail) == static_cast<MetricId>(hit.value)) {
                    active |= m_model.settings.overlay.metrics[static_cast<std::size_t>(detail)];
                }
            }
            selected = !active;
            // У старых профилей «всего» было отдельным датчиком: глаз пары
            // отключает и старую вторую строку, иначе она самопроизвольно вернётся.
            for (MetricId detail : kPairedMetricDetails) {
                if (displayMetric(detail) == static_cast<MetricId>(hit.value)) {
                    m_model.settings.overlay.metrics[static_cast<std::size_t>(detail)] = false;
                }
            }
            if (m_model.settings.overlay.pixelLayout) {
                enableOverlayPixelLayout(m_model.settings.overlay);
            }
            m_overlay.setSettings(m_model.settings.overlay);
            m_overlayEditor.refresh();
            saveSettings();
        }
        break;

    case ActionKind::ToggleMetric:
        if (hit.value >= 0 && hit.value < static_cast<int>(kLabelCount)) {
            auto& visible = m_model.settings.overlay.metrics[static_cast<std::size_t>(
                kOverlayPrimaryMetrics[static_cast<std::size_t>(hit.value)])];
            visible = !visible;
            m_overlay.setSettings(m_model.settings.overlay);
            saveSettings();
        }
        break;

    case ActionKind::OffsetX:
        if (m_model.settings.overlay.anchorMode == OverlayAnchorMode::Free) {
            auto& x = m_model.settings.overlay.originX;
            x = std::clamp(x + hit.value, 0, 32768);
        } else {
            auto& x = m_model.settings.overlay.offsetX;
            x = std::clamp(x + hit.value, -8192, 8192);
        }
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::OffsetY:
        if (m_model.settings.overlay.anchorMode == OverlayAnchorMode::Free) {
            auto& y = m_model.settings.overlay.originY;
            y = std::clamp(y + hit.value, 0, 32768);
        } else {
            auto& y = m_model.settings.overlay.offsetY;
            y = std::clamp(y + hit.value, -8192, 8192);
        }
        m_overlay.setSettings(m_model.settings.overlay);
        saveSettings();
        break;

    case ActionKind::ToggleOverlay:
        toggleOverlay();
        break;

    case ActionKind::TogglePause: {
        std::lock_guard<std::mutex> guard(m_modelMutex);
        m_model.paused = !m_model.paused;
        if (m_model.paused) {
            m_model.findings.clear();
            m_model.diagnosisInput = { };
            m_recentGamePid = 0;
            m_recentGameWindow = nullptr;
        }
        break;
    }

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

    case ActionKind::CreateDiagnosticReport: {
        GameDiagnosisInput input;
        HWND gameWindow = nullptr;
        const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                .count());
        {
            std::lock_guard<std::mutex> guard(m_modelMutex);
            input = m_model.diagnosisInput;
            if (!m_model.paused && m_recentGameWindow != nullptr && IsWindow(m_recentGameWindow) != FALSE
                && recentGameSelection(nowMs, m_recentGameAtMs, m_recentGamePid)) {
                if (input.gameProcessId == 0)
                    input.gameProcessId = m_recentGamePid;
                if (input.gameProcessId == m_recentGamePid)
                    gameWindow = m_recentGameWindow;
            }
        }
        input.nowMs = nowMs;
        std::string error;
        if (!showReportCardPreview(m_window, input, gameWindow, error)) {
            if (m_log)
                m_log->warning("диагностическая карточка: " + error);
            MessageBoxW(m_window, widen(error).c_str(), L"YawaFpsHub", MB_OK | MB_ICONERROR);
        }
        break;
    }

    case ActionKind::OpenDiagnosticReport: {
        wchar_t filename[MAX_PATH] { };
        OPENFILENAMEW dialog { };
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window;
        dialog.lpstrFilter = L"Пакет Yawa (*.yawa)\0*.yawa\0\0";
        dialog.lpstrFile = filename;
        dialog.nMaxFile = MAX_PATH;
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog) == FALSE)
            break;
        std::string archive;
        std::string error;
        ReportPackageContents contents;
        if (!readLocalReport(filename, archive, error) || !readReportPackage(archive, contents, error)
            || !showReportPackageViewer(m_window, contents, error)) {
            if (m_log)
                m_log->warning("просмотр .yawa: " + error);
            MessageBoxW(m_window, widen(error).c_str(), L"Пакет .yawa", MB_OK | MB_ICONERROR);
        }
        break;
    }

    case ActionKind::ImportOverlayRecipe: {
        wchar_t filename[MAX_PATH] { };
        OPENFILENAMEW dialog { };
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = GetActiveWindow() != nullptr ? GetActiveWindow() : m_window;
        dialog.lpstrFilter
            = L"Рецепт JSON (*.json)\0*.json\0RTSS-макет (*.ovl)\0*.ovl\0RTSS-пакет (*.ovx)\0*.ovx\0\0";
        dialog.lpstrFile = filename;
        dialog.nMaxFile = MAX_PATH;
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog) == FALSE)
            break;

        const wchar_t* suffix = std::wcsrchr(filename, L'.');
        const bool exchange = suffix != nullptr && _wcsicmp(suffix, L".ovx") == 0;
        const bool rtss = exchange || (suffix != nullptr && _wcsicmp(suffix, L".ovl") == 0);
        std::string content;
        std::string error;
        const HANDLE file = CreateFileW(
            filename, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = "Не удалось открыть файл оверлея: " + win32ErrorText(GetLastError());
        } else {
            LARGE_INTEGER length { };
            if (GetFileSizeEx(file, &length) == FALSE || length.QuadPart < 0) {
                error = "Не удалось определить размер файла оверлея";
            } else if (length.QuadPart > (exchange ? 8 * 1024 * 1024 : rtss ? 1024 * 1024 : 16384)) {
                error = "Файл оверлея слишком большой";
            } else {
                content.resize(static_cast<std::size_t>(length.QuadPart));
                DWORD received = 0;
                if (ReadFile(file, content.data(), static_cast<DWORD>(content.size()), &received, nullptr)
                        == FALSE
                    || received != static_cast<DWORD>(content.size())) {
                    error = "Не удалось прочитать файл оверлея";
                }
            }
            CloseHandle(file);
        }
        OverlaySettings imported = m_model.settings.overlay;
        if (error.empty()) {
            if (exchange) {
                importRtssExchange(content, imported, error);
            } else if (rtss) {
                importRtssOverlay(content, imported, error);
            } else {
                importOverlayRecipeJson(content, imported, error);
            }
        }
        if (!error.empty()) {
            if (m_log)
                m_log->warning("импорт оверлея: " + error);
            MessageBoxW(dialog.hwndOwner, widen(error).c_str(), L"YawaFpsHub", MB_OK | MB_ICONERROR);
            break;
        }
        if (rtss) {
            const bool hasRows = std::any_of(
                imported.metrics.begin(), imported.metrics.end(), [](bool selected) { return selected; });
            const bool hasVisual = imported.rtssVisual
                && std::any_of(imported.rtssVisual->layers.begin(), imported.rtssVisual->layers.end(),
                    [](const RtssVisualLayer& layer) { return !layer.text.empty() || layer.hasBackground; });
            if (!hasRows && !hasVisual) {
                MessageBoxW(dialog.hwndOwner, L"Нет слоёв для отображения. Текущий оверлей не изменён.",
                    L"Шаблон RTSS не перенесён", MB_OK | MB_ICONWARNING);
                break;
            }
            const std::wstring notice = L"Это предварительная отрисовка, не копия RTSS 1:1. "
                + std::to_wstring(imported.rtssOmittedLayers)
                + L" слоёв без связанной метрики. Фон, обычный текст и координаты "
                  L"переносятся; графики, сложные теги и картинки ещё не воспроизводятся. "
                  L"Недоступные значения помечены «нет данных». Исходный пакет остаётся "
                  L"для обратного экспорта.\n\nОткрыть этот макет вместо текущего оверлея?";
            if (MessageBoxW(dialog.hwndOwner, notice.c_str(), L"Частичный импорт RTSS",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2)
                != IDYES) {
                break;
            }
        }
        const OverlaySettings previous = m_model.settings.overlay;
        const bool wasVisible = m_overlay.isVisible();
        if (rtss && imported.opacityPercent == 0) {
            imported.opacityPercent = 100;
        }
        m_model.settings.overlay = std::move(imported);
        m_overlay.setSettings(m_model.settings.overlay);
        std::string renderError;
        if (!wasVisible && !m_model.menuScene) {
            m_overlay.show(&renderError);
        }
        if (renderError.empty() && m_overlay.isVisible() && m_model.settings.overlay.opacityPercent > 0
            && !m_overlay.hasVisiblePixels()) {
            renderError = "Импортированный макет не дал видимых пикселей";
        }
        if (!renderError.empty()) {
            m_model.settings.overlay = previous;
            m_overlay.setSettings(previous);
            if (!wasVisible) {
                m_overlay.hide();
            }
            m_model.overlayVisible = wasVisible;
            if (m_log) {
                m_log->warning("импорт оверлея: " + renderError);
            }
            MessageBoxW(dialog.hwndOwner, widen(renderError).c_str(), L"Оверлей оставлен без изменений",
                MB_OK | MB_ICONWARNING);
            break;
        }
        m_model.overlayVisible = m_overlay.isVisible();
        saveSettings();
        m_overlayEditor.refresh();
        InvalidateRect(m_window, nullptr, FALSE);
        break;
    }

    case ActionKind::ExportOverlayRecipe: {
        wchar_t filename[MAX_PATH] = L"yawa-overlay-recipe";
        OPENFILENAMEW dialog { };
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = GetActiveWindow() != nullptr ? GetActiveWindow() : m_window;
        dialog.lpstrFilter = L"Рецепт JSON (*.json)\0*.json\0RTSS-макет (*.ovl)\0*.ovl\0Исходный RTSS-пакет "
                             L"(*.ovx)\0*.ovx\0\0";
        dialog.lpstrFile = filename;
        dialog.nMaxFile = MAX_PATH;
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (GetSaveFileNameW(&dialog) == FALSE) {
            break;
        }
        std::wstring path(filename);
        const std::size_t dot = path.find_last_of(L'.');
        const std::size_t slash = path.find_last_of(L"\\/");
        const bool hasExtension = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash);
        const bool exchange
            = hasExtension ? _wcsicmp(path.c_str() + dot, L".ovx") == 0 : dialog.nFilterIndex == 3;
        const bool rtss
            = hasExtension ? _wcsicmp(path.c_str() + dot, L".ovl") == 0 : dialog.nFilterIndex == 2;
        if (hasExtension && !rtss && !exchange && _wcsicmp(path.c_str() + dot, L".json") != 0) {
            MessageBoxW(dialog.hwndOwner, L"Поддерживается экспорт .json, .ovl и исходного .ovx.",
                L"YawaFpsHub", MB_OK | MB_ICONWARNING);
            break;
        }
        if (exchange && !m_model.settings.overlay.rtssExchange) {
            MessageBoxW(dialog.hwndOwner,
                L"Нет импортированного .ovx для обратного экспорта. "
                L"Создание нового пакета .ovx в Yawa не поддерживается.",
                L"YawaFpsHub", MB_OK | MB_ICONWARNING);
            break;
        }
        if (!hasExtension) {
            path += exchange ? L".ovx" : rtss ? L".ovl" : L".json";
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES
                && MessageBoxW(dialog.hwndOwner, L"Файл уже существует. Заменить?", L"YawaFpsHub",
                       MB_YESNO | MB_ICONWARNING)
                    != IDYES) {
                break;
            }
        }
        bool exportOriginal = false;
        if (rtss && m_model.settings.overlay.rtssTemplate) {
            const int choice = MessageBoxW(dialog.hwndOwner,
                L"Да — экспортировать исходный RTSS .ovl без изменений (включая незнакомые слои).\n"
                L"Нет — создать новый .ovl только из текущих показателей Yawa.\n"
                L"Отмена — не сохранять.\n\n"
                L"Правки в мастерской не изменяют исходный файл. Внешние картинки остаются "
                L"зависимыми от своих путей.",
                L"Как экспортировать RTSS?", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (choice == IDCANCEL) {
                break;
            }
            exportOriginal = choice == IDYES;
        }
        const std::string content = exchange ? *m_model.settings.overlay.rtssExchange
            : exportOriginal                 ? *m_model.settings.overlay.rtssTemplate
            : rtss                           ? serializeRtssOverlay(m_model.settings.overlay)
                                             : serializeOverlayRecipeJson(m_model.settings.overlay);
        if (content.empty()) {
            MessageBoxW(dialog.hwndOwner, L"Нет показателей, которые можно экспортировать в RTSS.",
                L"YawaFpsHub", MB_OK | MB_ICONWARNING);
            break;
        }
        if (!writeTextFile(narrow(path), content)) {
            if (m_log != nullptr) {
                m_log->warning("не удалось сохранить оверлей " + narrow(path));
            }
            MessageBoxW(
                dialog.hwndOwner, L"Не удалось сохранить файл оверлея.", L"YawaFpsHub", MB_OK | MB_ICONERROR);
        } else if (exchange) {
            MessageBoxW(dialog.hwndOwner,
                L"Исходный пакет .ovx записан без изменений, включая вложенные "
                L"файлы. Правки мастерской Yawa в нём не отражены.",
                L"Экспорт RTSS", MB_OK | MB_ICONINFORMATION);
        } else if (rtss) {
            MessageBoxW(dialog.hwndOwner,
                exportOriginal
                    ? L"Исходный .ovl записан без изменений. Вложенные в импортированный .ovx изображения "
                      L"в этот файл не включены: для полного пакета экспортируйте исходный .ovx."
                    : L"Созданы только показатели RTSS HAL и FPS. Форма, картинки и внешние датчики "
                      L"не добавлены. Проверьте шаблон в RTSS OverlayEditor.",
                L"Экспорт RTSS", MB_OK | MB_ICONINFORMATION);
        }
        break;
    }

    case ActionKind::OpenOverlayEditor:
        if (!m_overlayEditor.open(
                m_window, m_instance, m_model, m_theme,
                [this](const UiHit& control, float x, bool finished) {
                    if (isSliderAction(control.action)) {
                        applySlider(control.action, control.rect, x);
                        if (finished) {
                            saveSettings();
                        }
                    } else {
                        applyAction(control);
                    }
                    if (control.action == ActionKind::ToggleMetric && m_model.settings.overlay.pixelLayout) {
                        enableOverlayPixelLayout(m_model.settings.overlay);
                        m_overlay.setSettings(m_model.settings.overlay);
                    }
                    m_overlayEditor.refresh();
                    InvalidateRect(m_window, nullptr, FALSE);
                },
                [this](wchar_t symbol) {
                    editLabelChar(symbol);
                    m_overlayEditor.refresh();
                },
                [this](bool finished) {
                    m_overlay.setSettings(m_model.settings.overlay);
                    if (m_model.menuScene) {
                        refreshMenuScene();
                    }
                    if (finished) {
                        saveSettings();
                    }
                    InvalidateRect(m_window, nullptr, FALSE);
                })
            && m_log) {
            m_log->warning("не удалось открыть редактор оверлея");
        }
        break;

    case ActionKind::ReorderMetric:
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
    if (m_profileActive && !m_activeGameKey.empty()) {
        copyOverlayPlacement(m_baseOverlay, m_model.settings.overlay);
        if (!writeGameOverlay(m_activeGameKey, m_model.settings.overlay) && m_log != nullptr) {
            m_log->warning("не удалось записать профиль " + gameOverlayPath(m_activeGameKey));
        }
    } else {
        m_baseOverlay = m_model.settings.overlay;
    }
    Settings global = m_model.settings;
    global.overlay = m_baseOverlay;
    if (!writeTextFile(settingsPath(), serializeSettings(global)) && m_log != nullptr) {
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
    {
        std::lock_guard<std::mutex> guard(m_modelMutex);
        m_model.etwRunning = false;
        m_model.findings.clear();
        m_model.diagnosisInput = { };
        m_recentGamePid = 0;
        m_recentGameWindow = nullptr;
        m_model.store.reset();
    }
    m_etwRestartRequested = true;
    m_wake.notify_all();
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
    static constexpr std::array<std::wstring_view, 14> kSystemNames { L"dwm.exe", L"explorer.exe",
        L"csrss.exe", L"winlogon.exe", L"wininit.exe", L"services.exe", L"lsass.exe", L"smss.exe",
        L"searchhost.exe", L"startmenuexperiencehost.exe", L"textinputhost.exe", L"shellexperiencehost.exe",
        L"applicationframehost.exe", L"lockapp.exe" };

    bool system = true;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process != nullptr) {
        wchar_t path[512] { };
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
    if (m_log != nullptr) {
        m_log->info("инициализация датчиков в фоне");
    }
    std::string sensorError;
    if (!m_sensors.initialize(*m_log, sensorError) && m_log != nullptr) {
        m_log->warning("инициализация датчиков: " + sensorError);
    }
    if (m_stopRequested) {
        return;
    }

    const GpuRegistry& registry = m_sensors.gpus();
    auto ready = std::make_unique<SensorInitReport>();
    ready->memoryTimings = m_sensors.memoryTimings();
    for (std::size_t index = 0; index < registry.count(); ++index) {
        if (const GpuAdapter* adapter = registry.byIndex(index)) {
            ready->gpuNames.push_back(adapter->name);
        }
    }
    GpuSelection configured;
    {
        std::lock_guard<std::mutex> guard(m_modelMutex);
        configured = m_model.settings.gpu;
    }
    if (configured.hasSelection && registry.count() > 0) {
        m_sensors.selectGpu(registry.resolve(configured.luidLow, configured.luidHigh));
    }
    ready->activeGpu = m_sensors.activeGpu();
    if (const GpuAdapter* adapter = registry.byIndex(ready->activeGpu)) {
        ready->gpu = GpuSelection { true, adapter->luidLow, adapter->luidHigh };
    }
    if (PostMessageW(m_window, kSensorInitMessage, 0, reinterpret_cast<LPARAM>(ready.get()))) {
        ready.release();
    }
    auto initialHardware = std::make_unique<HardwareReport>();
    initialHardware->success
        = collectHardwareInfo(initialHardware->hardware, registry, initialHardware->error, false);
    if (PostMessageW(m_window, kHardwareInfoMessage, 0, reinterpret_cast<LPARAM>(initialHardware.get()))) {
        initialHardware.release();
    }
    // Если WMI зависнет, паспорт CPU/SMBIOS/DXGI уже показан и окно продолжит работать.
    std::thread([window = m_window, adapters = GpuRegistry(registry)]() {
        auto report = std::make_unique<HardwareReport>();
        report->success = collectHardwareInfo(report->hardware, adapters, report->error);
        if (PostMessageW(window, kHardwareInfoMessage, 0, reinterpret_cast<LPARAM>(report.get()))) {
            report.release();
        }
    }).detach();

    yawa::FrameTracker tracker(EtwFrameSession::qpcFrequency());
    yawa::SystemSampler sampler;

    const auto startEtw = [this] {
        std::string error;
        if (!m_etw.start(error)) {
            m_etw.stop();
            if (m_log != nullptr) {
                m_log->warning("ETW: " + error);
            }
        }
    };
    bool etwEnabled = false;
    {
        std::lock_guard<std::mutex> guard(m_modelMutex);
        etwEnabled = m_model.settings.capture.etwEnabled;
    }
    if (etwEnabled) {
        startEtw();
    }

    auto previous = std::chrono::steady_clock::now();
    auto lastFrameSample = previous;

    while (!m_stopRequested) {
        if (m_etwRestartRequested.exchange(false)) {
            m_etw.stop();
            tracker.reset();
            sampler = yawa::SystemSampler { };
            previous = std::chrono::steady_clock::now();
            lastFrameSample = previous;
            bool enabled = false;
            {
                std::lock_guard<std::mutex> guard(m_modelMutex);
                enabled = m_model.settings.capture.etwEnabled;
            }
            if (enabled) {
                startEtw();
            }
        }
        int interval = 1000;
        int targetHz = 60;
        bool paused = false;
        {
            std::lock_guard<std::mutex> guard(m_modelMutex);
            interval = m_model.settings.capture.intervalMs;
            targetHz = m_model.settings.capture.targetHz;
            paused = m_model.paused;
        }

        if (!paused) {
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - previous).count();
            previous = now;

            if (m_etw.drain(tracker) != 0) {
                lastFrameSample = now;
            } else if (tracker.streamCount() != 0 && now - lastFrameSample > std::chrono::seconds(2)) {
                // Без новых Present-событий последний QPC не меняется: история иначе залипает навсегда.
                tracker.reset();
            }

            // Игра — это процесс активного окна, как в RTSS. Без этого на мониторе 144 Гц
            // композитор рабочего стола обгонял игру на 120 кадрах.
            DWORD foregroundPid = 0;
            const HWND foregroundWindow = GetForegroundWindow();
            if (foregroundWindow != nullptr) {
                GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
            }
            if (foregroundPid == GetCurrentProcessId()) {
                foregroundPid = 0;
            }
            tracker.setForegroundProcess(static_cast<std::uint32_t>(foregroundPid));

            std::array<std::uint32_t, yawa::FrameTracker::kMaxStreams> streamPids { };
            const std::size_t streamCount = tracker.streamProcesses(streamPids.data(), streamPids.size());
            std::array<std::uint32_t, yawa::FrameTracker::kMaxStreams> systemPids { };
            std::size_t systemCount = 0;
            for (std::size_t index = 0; index < streamCount; ++index) {
                if (isSystemProcessCached(streamPids[index])) {
                    systemPids[systemCount++] = streamPids[index];
                }
            }
            tracker.setSystemProcesses(systemPids.data(), systemCount);

            // Для оценки игры не смешиваем фоновые процессы и весь адаптер с её PID.
            const std::uint32_t gamePid = tracker.activeProcessId() == foregroundPid
                    && !tracker.isSystemProcess(foregroundPid) && tracker.hasActiveStream()
                ? static_cast<std::uint32_t>(foregroundPid)
                : 0;
            const std::size_t requested = m_requestedGpu.exchange(static_cast<std::size_t>(-1));
            if (requested != static_cast<std::size_t>(-1)) {
                m_sensors.selectGpu(requested);
            }
            yawa::SystemSnapshot snapshot;
            m_sensors.sample(snapshot, gamePid);

            const yawa::SystemSampler::Deltas deltas = sampler.compute(snapshot, elapsed);
            const yawa::DerivedValues values = yawa::deriveValues(snapshot, deltas);
            const yawa::FpsSummary fps = tracker.summary(static_cast<double>(targetHz));
            const bool etwRunning = m_etw.isRunning();
            GameDiagnosisInput diagnosis;
            diagnosis.gameProcessId = gamePid;
            diagnosis.sampledAtMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
            diagnosis.nowMs
                = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                        .count());
            diagnosis.capture
                = gamePid != 0 && etwRunning ? GameCaptureState::Running : GameCaptureState::Unknown;
            if (gamePid != 0 && fps.currentFps > 0.0)
                diagnosis.fps = fps.currentFps;
            if (gamePid != 0 && snapshot.gameCpuLoad >= 0.0)
                diagnosis.gameCpuLoad = snapshot.gameCpuLoad;
            if (gamePid != 0 && snapshot.gamePeakThreadLoad >= 0.0)
                diagnosis.gameThreadLoad = snapshot.gamePeakThreadLoad;
            // Этот счётчик PDH уже отфильтрован по PID, 3D-движку и LUID выбранной карты.
            if (gamePid != 0 && snapshot.gameGpuLoad >= 0.0) {
                diagnosis.gpuLoad = snapshot.gameGpuLoad;
                diagnosis.gpuMatchesGame = true;
            }
            auto findings = diagnoseGame(diagnosis);

            const std::wstring foreground = foregroundProcessName();

            {
                std::lock_guard<std::mutex> guard(m_modelMutex);
                yawa::applySnapshot(m_model.store, snapshot, values, fps,
                    static_cast<std::uint8_t>(m_model.settings.capture.smoothingPercent));
                m_model.fps = fps;
                m_model.framesPresented = tracker.activePresents();
                m_model.availability = m_sensors.availability();
                m_model.diskHealth = m_sensors.diskHealth();
                m_model.cpuCores = m_sensors.coreCount();
                m_model.cpuThreads = m_sensors.cpuThreads();
                m_model.cpuName = m_sensors.cpuName();
                // Узкое место пересчитываем на том же снимке, что и экраны: отдельного
                // опроса железа нет, и цифры на «Балансе» всегда совпадают с «Обзором».
                m_model.gameName = gamePid != 0 ? foreground : L"";
                m_model.gameProcessId = gamePid;
                if (gamePid != 0 && !m_model.paused) {
                    m_recentGamePid = gamePid;
                    m_recentGameWindow = foregroundWindow;
                    m_recentGameAtMs = diagnosis.sampledAtMs;
                }
                m_model.gameCpuLoad = gamePid != 0 ? snapshot.gameCpuLoad : -1.0;
                m_model.gamePeakThreadLoad = gamePid != 0 ? snapshot.gamePeakThreadLoad : -1.0;
                m_model.gameGpuLoad = gamePid != 0 ? snapshot.gameGpuLoad : -1.0;
                SystemLoad gameLoad = loadFromStore(m_model.store);
                gameLoad.cpuLoad = snapshot.gameCpuLoad;
                gameLoad.cpuPeakCore = snapshot.gamePeakThreadLoad;
                gameLoad.gpuLoad = snapshot.gameGpuLoad;
                m_model.balance = analyzeActiveGame(m_model.hardware, gameLoad, gamePid != 0);
                m_model.findings = m_model.paused ? std::vector<Finding> { } : std::move(findings);
                m_model.diagnosisInput = m_model.paused ? GameDiagnosisInput { } : diagnosis;
                m_model.etwRunning = etwRunning;
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
            [this]() { return m_stopRequested.load() || m_etwRestartRequested.load(); });
    }

    m_etw.stop();
}

void MainWindow::onTick()
{
    std::string gameKey;
    const auto profileNowMs
        = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                .count());
    {
        std::lock_guard<std::mutex> guard(m_modelMutex);
        if (m_model.gameProcessId != 0) {
            gameKey = gameProfileKey(m_model.gameName);
            if (!gameKey.empty()) {
                m_recentGameKey = gameKey;
                m_model.profileGameName = m_model.gameName;
            } else {
                m_recentGameKey.clear();
                m_model.profileGameName.clear();
            }
        } else if (m_recentGameAtMs == 0 || profileNowMs < m_recentGameAtMs
            || profileNowMs - m_recentGameAtMs > 300000) {
            m_recentGameKey.clear();
            m_model.profileGameName.clear();
        }
    }
    if (gameKey.empty() && m_overlayEditor.isOpen() && m_profileActive) {
        gameKey = m_activeGameKey;
    }
    if (gameKey != m_activeGameKey) {
        Settings saved;
        std::string content;
        std::string error;
        const bool hasProfile = !gameKey.empty() && readTextFile(gameOverlayPath(gameKey), content)
            && content.size() <= 20 * 1024 * 1024 && parseSettings(content, saved, error);
        OverlaySettings resolved;
        {
            std::lock_guard<std::mutex> guard(m_modelMutex);
            m_activeGameKey = gameKey;
            m_profileActive = hasProfile;
            m_model.hasGameProfile = hasProfile;
            m_model.settings.overlay
                = hasProfile ? overlayForGame(m_baseOverlay, saved.overlay) : m_baseOverlay;
            resolved = m_model.settings.overlay;
        }
        m_overlay.setSettings(resolved);
        if (m_model.menuScene) {
            refreshMenuScene();
        }
        m_overlayEditor.refresh();
    }
    static const auto started = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    std::uint32_t gamePid = 0;
    bool followGameMonitor = false;
    bool movedSavedMonitor = false;
    OverlaySettings resolvedPlacement;

    {
        std::lock_guard<std::mutex> guard(m_modelMutex);
        m_model.elapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count());
        m_model.overlayVisible = m_overlay.isVisible();
        auto& placement = m_model.settings.overlay;
        if (placement.anchorMode == OverlayAnchorMode::Free && !placement.savedMonitorId.empty()
            && m_dragAction != ActionKind::DragOverlay && !m_overlay.isDragging()
            && !m_overlayEditor.isDragging()) {
            RECT savedMonitor { };
            if (findOverlayMonitor(placement.savedMonitorId, savedMonitor)) {
                const RECT screen = overlayVirtualScreen();
                const int x = originFromMonitor(placement.monitorOriginX, savedMonitor.left, screen.left);
                const int y = originFromMonitor(placement.monitorOriginY, savedMonitor.top, screen.top);
                if (placement.originX != x || placement.originY != y) {
                    placement.originX = x;
                    placement.originY = y;
                    resolvedPlacement = placement;
                    movedSavedMonitor = true;
                }
            }
        }
        gamePid = m_model.gameProcessId;
        followGameMonitor = m_model.settings.overlay.followGameMonitor;
        m_overlay.setContent(m_model.store, m_model.fps,
            m_model.activeGpu < m_model.gpuNames.size() ? m_model.gpuNames[m_model.activeGpu] : L"");
    }

    if (movedSavedMonitor) {
        m_overlay.setSettings(resolvedPlacement);
    }
    RECT gameMonitor { };
    const RECT* target = nullptr;
    if (followGameMonitor && gamePid != 0) {
        const HWND foreground = GetForegroundWindow();
        DWORD foregroundPid = 0;
        if (foreground != nullptr && IsIconic(foreground) == FALSE) {
            GetWindowThreadProcessId(foreground, &foregroundPid);
        }
        if (foregroundPid == gamePid) {
            MONITORINFO monitor { sizeof(monitor) };
            const HMONITOR screen = MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL);
            if (screen != nullptr && GetMonitorInfoW(screen, &monitor)) {
                gameMonitor = monitor.rcMonitor;
                target = &gameMonitor;
            }
        }
    }
    m_overlay.setGameMonitor(target);
    m_overlay.repaint();
    m_overlayEditor.refresh();

    // Свёрнутое или скрытое в трей окно не перерисовываем: это экономит CPU в простое.
    if (IsWindowVisible(m_window) != FALSE && IsIconic(m_window) == FALSE) {
        InvalidateRect(m_window, nullptr, FALSE);
    }
}

void MainWindow::createTrayIcon()
{
    NOTIFYICONDATAW icon { };
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

    NOTIFYICONDATAW icon { };
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

    POINT cursor { };
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
    UnregisterHotKey(m_window, kHotkeyClickThrough);
    UnregisterHotKey(m_window, kHotkeyEmergencyExit);
    UnregisterHotKey(m_window, kHotkeyMenuScene);
    RegisterHotKey(m_window, kHotkeyEmergencyExit, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F10);
    if (RegisterHotKey(m_window, kHotkeyMenuScene, MOD_CONTROL | MOD_NOREPEAT, VK_F11) == FALSE
        && m_log != nullptr) {
        m_log->warning("хоткей Ctrl+F11 для сцены меню занят другой программой");
    }

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
