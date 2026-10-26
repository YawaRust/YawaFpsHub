add_executable(YawaFpsHub WIN32
    main.cpp
    metrics/frame_tracker.cpp
    metrics/gpu_registry.cpp
    metrics/hardware_info.cpp
    metrics/bottleneck.cpp
    metrics/metric_store.cpp
    metrics/statistics.cpp
    metrics/system_sampler.cpp
    metrics/overlay_rows.cpp
    metrics/units.cpp
    platform/app_paths.cpp
    platform/display_info.cpp
    platform/dxgi_adapters.cpp
    platform/hardware_info_win.cpp
    platform/etw_frames.cpp
    platform/log.cpp
    platform/main_window.cpp
    platform/nvml_sensors.cpp
    platform/overlay_window.cpp
    platform/pdh_query.cpp
    platform/sensors_win.cpp
    platform/win_util.cpp
    render/d2d.cpp
    render/overlay_painter.cpp
    render/theme.cpp
    ui/bottleneck_view.cpp
    ui/chrome.cpp
    ui/dashboard_view.cpp
    ui/layout.cpp
    ui/overlay_view.cpp
    ui/sensors_view.cpp
    ui/settings.cpp
    ui/settings_view.cpp
    ui/system_view.cpp
)

# Ресурсы (иконка и свойства файла) собирает только MSVC: windres в CI не используется,
# а локальная проверка ядра собирается без приложения.
if(MSVC)
    set(YAWA_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/yawa.ico")
    configure_file(resources/resource.rc.in "${CMAKE_CURRENT_BINARY_DIR}/resource.rc" @ONLY)
    target_sources(YawaFpsHub PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/resource.rc")
endif()

target_include_directories(YawaFpsHub PRIVATE ${CMAKE_SOURCE_DIR}/app)
target_link_libraries(YawaFpsHub PRIVATE d2d1 dwrite dxgi dwmapi pdh psapi shell32 user32 gdi32 advapi32 ole32
    oleaut32 wbemuuid windowscodecs)
# Один exe без зависимостей от установщика Visual C++: рантайм линкуем статически.
# Без этого YawaFpsHub.exe не запускается на чистой Windows, если там нет VC++ Redistributable.
if(MSVC)
    set_property(TARGET YawaFpsHub PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

target_compile_definitions(YawaFpsHub PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    YAWA_VERSION_STRING="${PROJECT_VERSION}")
target_compile_features(YawaFpsHub PRIVATE cxx_std_20)

set_target_properties(YawaFpsHub PROPERTIES OUTPUT_NAME "YawaFpsHub")
