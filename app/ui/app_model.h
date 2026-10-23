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
    ClearLabel,
    ResetOverlayPlacement,
    DragOverlay,
    ApplyLabelPreset,
    OffsetX,
    OffsetY,
    WindowMinimize,
    WindowMaximize,
    WindowClose,
    FocusSensorSearch,
    ToggleOverlayOnly,
    ToggleOverlayBackground,
    ToggleOverlayBorder,
    ToggleOverlayShadow,
    SetOverlayRadius,
    SetOverlayFont,
    ToggleOverlayBold,
    ToggleOverlayItalic,
    ChangeOverlayOutline,
    SelectGpu,
    ToggleGpuMenu,
    CaptureHotkey,
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
    std::uint32_t cpuCores = 0;
    bool etwRunning = false;
    bool paused = false;
    bool overlayVisible = false;
    // Текст последней ошибки оверлея: показывается в статус-баре вместо подсказок.
    std::wstring overlayError;
    View view = View::Dashboard;
    int editingLabel = -1;
    // -1 — все группы, иначе номер группы датчиков из списка экрана «Датчики».
    int sensorFilter = -1;
    // Пути и имя программы хранятся в UTF-8, как и остальные строки настроек.
    std::string settingsPath;
    std::string logPath;
    std::string sessionsPath;
    std::wstring foregroundApp;
    // Строка поиска на экране датчиков и фильтр «только в оверлее».
    std::string sensorQuery;
    bool sensorSearchActive = false;
    bool overlayOnly = false;
    // Редактор горячих клавиш: -1 — не редактируем, иначе номер действия (0…2).
    int hotkeyEditing = -1;
    // Выпадающий список выбора GPU на обзоре.
    bool gpuMenuOpen = false;
    int sensorScroll = 0;
    // Состояние окна для своего заголовка: перетаскивание и кнопки рисуются вручную.
    bool maximized = false;
    float cursorX = -100.0f;
    float cursorY = -100.0f;
    bool cursorInside = false;
};

constexpr std::size_t kFavoriteHistoryPoints = 240;

// Наведение считается по координатам курсора, которые окно обновляет на WM_MOUSEMOVE.
// Так экран остаётся «только рисующим» и не заводит собственного состояния.
inline bool isHovered(const AppModel& model, const Rect& rect)
{
    return model.cursorInside && rect.contains(model.cursorX, model.cursorY);
}

} // namespace yawa
