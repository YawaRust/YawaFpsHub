cmake_minimum_required(VERSION 3.21)

# Номер релиза живёт в файле VERSION: его читают CMake, workflow релиза и
# tools/bump_version.py. Один файл — одна правка на выпуск.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" YAWA_VERSION_TEXT)
string(STRIP "${YAWA_VERSION_TEXT}" YAWA_VERSION_TEXT)

project(YawaFpsHub VERSION ${YAWA_VERSION_TEXT} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    if(YAWA_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(YAWA_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

# Графическое приложение появится на этапе 4 вместе с Direct2D-рендером.
option(YAWA_BUILD_APP "Собирать приложение YawaFpsHub (Windows)" ON)
# Диагностическая утилита: показывает, какие датчики реально доступны на машине.
option(YAWA_BUILD_PROBE "Собирать yawa-probe (Windows)" ON)

# Портируемое ядро метрик: собирается везде, включая Linux-агент тестов.
add_library(yawa_metrics STATIC
    app/metrics/overlay_rows.cpp
    app/metrics/units.cpp
    app/metrics/statistics.cpp
    app/metrics/metric_store.cpp
    app/metrics/system_sampler.cpp
    app/metrics/frame_tracker.cpp
    app/metrics/gpu_registry.cpp
    app/metrics/hardware_info.cpp
    app/metrics/bottleneck.cpp
    app/ui/settings.cpp
)

target_include_directories(yawa_metrics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_compile_features(yawa_metrics PUBLIC cxx_std_20)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(YAWA_BUILD_PROBE AND WIN32)
    add_subdirectory(tools/probe)
endif()

if(YAWA_BUILD_APP AND WIN32 AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/app/CMakeLists.txt)
    add_subdirectory(app)
endif()
