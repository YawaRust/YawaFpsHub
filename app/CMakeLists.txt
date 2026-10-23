add_executable(YawaFpsHub WIN32
    main.cpp
    metrics/frame_tracker.cpp
    metrics/gpu_registry.cpp
    metrics/metric_store.cpp
    metrics/statistics.cpp
    metrics/system_sampler.cpp
    metrics/units.cpp
    platform/app_paths.cpp
    platform/dxgi_adapters.cpp
    platform/etw_frames.cpp
    platform/log.cpp
    platform/main_window.cpp
    platform/nvml_sensors.cpp
    platform/overlay_window.cpp
    platform/pdh_query.cpp
    platform/sensors_win.cpp
    platform/win_util.cpp
    render/d2d.cpp
    render/theme.cpp
    ui/chrome.cpp
    ui/dashboard_view.cpp
    ui/layout.cpp
    ui/sensors_view.cpp
    ui/settings.cpp
    ui/settings_view.cpp
)

target_include_directories(YawaFpsHub PRIVATE ${CMAKE_SOURCE_DIR}/app)
target_link_libraries(YawaFpsHub PRIVATE d2d1 dwrite dxgi dwmapi pdh psapi shell32 user32 gdi32 advapi32 ole32)
target_compile_definitions(YawaFpsHub PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_features(YawaFpsHub PRIVATE cxx_std_20)

set_target_properties(YawaFpsHub PROPERTIES OUTPUT_NAME "YawaFpsHub")
