#pragma once

#include "metrics/frame_tracker.h"
#include "metrics/system_sampler.h"
#include "platform/etw_frames.h"
#include "platform/log.h"
#include "platform/overlay_window.h"
#include "platform/sensors_win.h"
#include "render/d2d.h"
#include "render/theme.h"
#include "ui/app_model.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace yawa {

class MainWindow {
public:
    MainWindow() = default;
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool create(HINSTANCE instance, Log& log, std::string& error);
    void setInitialView(View view) { m_model.view = view; }

    // CI снимает экраны при фиксированном размере: иначе размер окна зависит от
    // разрешения раннера и сравнивать со скриншотом прототипа нечем.
    void setInitialSize(int width, int height);
    // Стартовый стиль оверлея: нужен CI, чтобы снять все пять стилей одного экрана.
    // Вызывается после create(), поэтому применяем сразу, а не через флаг.
    void setInitialStyle(OverlayStyle style)
    {
        m_model.settings.overlay.style = style;
        m_overlay.setSettings(m_model.settings.overlay);
    }
    void setOverlayVisible(bool visible);
    void exportOverlayStyles();
    // smokeSeconds > 0 — самопроверка для CI: окно живёт заданное время и закрывается само.
    int run(int smokeSeconds = 0, const std::wstring& overlayShotDir = std::wstring());

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void startSampling();
    void stopSampling();
    // Перезапуск нужен, когда меняется состав сбора (ETW включают или выключают).
    void restartSampling();
    void samplingLoop();
    void onTick();
    void paint();
    void handleClick(float x, float y);
    void dragSlider(float x);
    void applySlider(ActionKind action, const Rect& rect, float x);
    void applyAction(const UiHit& hit);
    // Ввод подписи: символы приходят из WM_CHAR/WM_IME_CHAR уже в UTF-16.
    void editLabelChar(wchar_t symbol);
    // Поиск по датчикам: та же логика, что у подписей, но строка живёт в модели.
    void editSensorQuery(wchar_t symbol);
    void saveSettings();
    // Закрытие по «Выход» в трее должно завершать процесс, а не прятать окно в трей.
    bool m_quitRequested = false;

    void createTrayIcon();
    void destroyTrayIcon();
    void showTrayMenu();
    void registerHotkeys();
    void toggleVisibilityFromTray();
    // Единая точка включения/выключения оверлея: логирует причину, если показать не удалось.
    void toggleOverlay();
    void selectGpu(std::size_t index);

    Rect contentArea() const;
    // Экран рисуется целиком в «демо-высоте», а окно показывает его часть со сдвигом.
    float contentDesignHeight() const;
    void scrollContent(float delta);
    void drawScrollBar(const Rect& viewport);
    std::vector<UiHit> buildHits();
    void paintView();

    HWND m_window = nullptr;
    HINSTANCE m_instance = nullptr;
    Log* m_log = nullptr;

    Renderer m_renderer;
    OverlayWindow m_overlay;
    SystemSensors m_sensors;
    EtwFrameSession m_etw;

    AppModel m_model;
    Theme m_theme = makeTheme(Accent::Mint);
    std::vector<UiHit> m_hits;
    ActionKind m_dragAction = ActionKind::None;
    Rect m_dragRect;
    float m_dragStartX = 0.0f;
    float m_dragStartY = 0.0f;
    int m_dragOffsetX = 0;
    int m_dragOffsetY = 0;
    std::string m_labelBackup;

    std::thread m_worker;
    std::condition_variable m_wake;
    std::mutex m_wakeMutex;
    std::mutex m_modelMutex;
    std::atomic<bool> m_stopRequested{ false };
    bool m_running = false;
    bool m_trayCreated = false;
    bool m_trackingMouse = false;
    int m_hoverIndex = -1;
    float m_dpiScale = 1.0f;
    int m_initialWidth = 0;
    int m_initialHeight = 0;
    std::wstring m_overlayShotDir;
};

} // namespace yawa
