#pragma once

#include "metrics/metric_store.h"
#include "metrics/statistics.h"
#include "platform/sensors_win.h"
#include "render/d2d.h"
#include "ui/settings.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yawa {

enum class View : std::uint8_t { Dashboard, Sensors, Overlay, Settings };

// Что именно нажали в интерфейсе. Окно разбирает эти события и меняет состояние,
// сами экраны остаются «только рисующими».
enum class ActionKind : std::uint8_t {
    None,
    SwitchView,
    SetInterval,
    SetTargetHz,
    SetStyle,
    SetPosition,
    SetLayout,
    SetAccent,
    SetDensity,
    CycleGpu,
    ToggleOverlay,
    TogglePause,
    ToggleEtw,
    ToggleIdlePause,
    ToggleDiskNet,
    SelectLabel,
    ResetLabels,
    ToggleCompact,
    ToggleTray,
    OpenSettingsFolder,
    ResetSettings,
    SetScale,
    SetOpacity,
    ToggleMetric,
    SetSensorFilter,
    OffsetX,
    OffsetY,
};

struct UiHit {
    Rect rect{};
    ActionKind action = ActionKind::None;
    int value = 0;
};

// Всё, что нужно экранам для отрисовки. Снимок собирается в потоке окна,
// поэтому экраны не трогают ни датчики, ни файлы.
struct AppModel {
    MetricStore store;
    Settings settings;
    FpsSummary fps;
    SensorAvailability availability;
    std::vector<std::wstring> gpuNames;
    std::size_t activeGpu = 0;
    std::uint64_t elapsedMs = 0;
    double selfCpu = 0.0;
    double selfRamGb = 0.0;
    std::uint32_t selfThreads = 0;
    bool etwRunning = false;
    bool paused = false;
    bool overlayVisible = false;
    View view = View::Dashboard;
    int editingLabel = -1;
    // -1 — все группы, иначе номер группы датчиков из списка экрана «Датчики».
    int sensorFilter = -1;
    std::wstring settingsPath;
    std::wstring foregroundApp;
};

constexpr std::size_t kFavoriteHistoryPoints = 240;

} // namespace yawa
