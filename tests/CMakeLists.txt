add_executable(metrics_tests
    test_main.cpp
    test_statistics.cpp
    test_units.cpp
    test_metric_store.cpp
    test_system_sampler.cpp
    test_hardware_telemetry.cpp
    test_memory_timings.cpp
    test_spd.cpp
    test_frame_tracker.cpp
    test_settings.cpp
    test_gpu_registry.cpp
    test_bottleneck.cpp
    test_diagnosis.cpp
    test_report.cpp
)

target_link_libraries(metrics_tests PRIVATE yawa_metrics)
target_include_directories(metrics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(metrics_tests PRIVATE cxx_std_20)

add_test(NAME metrics_tests COMMAND metrics_tests)
add_test(NAME ui_labels COMMAND ${CMAKE_COMMAND} -DPROJECT_ROOT=${PROJECT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_ui_labels.cmake)

# Synthetic game pixels verify that Direct2D composes opaque masks into both
# preview pixels and the PNG. The DXGI game capture itself still needs real hardware.
if(WIN32)
    add_executable(report_card_win_tests
        ../app/platform/report_card_win_test.cpp
        ../app/platform/report_card_win.cpp
        ../app/platform/win_util.cpp)
    target_link_libraries(report_card_win_tests PRIVATE yawa_metrics d2d1 dwrite windowscodecs ole32)
    target_include_directories(report_card_win_tests PRIVATE ${PROJECT_SOURCE_DIR}/app)
    target_compile_definitions(report_card_win_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_test(NAME report_card_win_tests COMMAND report_card_win_tests)
endif()
