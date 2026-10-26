add_executable(metrics_tests
    test_main.cpp
    test_statistics.cpp
    test_units.cpp
    test_metric_store.cpp
    test_system_sampler.cpp
    test_hardware_telemetry.cpp
    test_frame_tracker.cpp
    test_settings.cpp
    test_gpu_registry.cpp
    test_bottleneck.cpp
)

target_link_libraries(metrics_tests PRIVATE yawa_metrics)
target_include_directories(metrics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(metrics_tests PRIVATE cxx_std_20)

add_test(NAME metrics_tests COMMAND metrics_tests)
