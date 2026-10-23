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
    void saveSettings();

    void createTrayIcon();
    void destroyTrayIcon();
    void showTrayMenu();
    void registerHotkeys();
    void toggleVisibilityFromTray();

    Rect contentArea() const;
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
    std::string m_labelBackup;

    std::thread m_worker;
    std::condition_variable m_wake;
    std::mutex m_wakeMutex;
    std::mutex m_modelMutex;
    std::atomic<bool> m_stopRequested{ false };
    bool m_running = false;
    bool m_trayCreated = false;
    float m_dpiScale = 1.0f;
    std::wstring m_overlayShotDir;
};

} // namespace yawa
