add_executable(yawa-probe
    main.cpp
    ../../app/platform/log.cpp
    ../../app/platform/etw_frames.cpp
    ../../app/platform/dxgi_adapters.cpp
    ../../app/platform/nvml_sensors.cpp
    ../../app/platform/pdh_query.cpp
    ../../app/platform/sensors_win.cpp
    ../../app/platform/win_util.cpp
)

target_link_libraries(yawa-probe PRIVATE
    yawa_metrics
    advapi32
    dxgi
    pdh
    psapi
    # WMI-фолбэк температуры CPU живёт в sensors_win.cpp, поэтому probe линкует те же библиотеки.
    ole32
    oleaut32
    wbemuuid
)

target_include_directories(yawa-probe PRIVATE ${CMAKE_SOURCE_DIR}/app)
target_compile_definitions(yawa-probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_features(yawa-probe PRIVATE cxx_std_20)

set_target_properties(yawa-probe PROPERTIES OUTPUT_NAME "yawa-probe")
