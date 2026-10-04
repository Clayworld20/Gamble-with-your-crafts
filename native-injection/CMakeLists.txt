# ============================================================================
#  CMakeLists.txt — сборка нативной части моста GWYF ↔ Minecraft.
#
#  ЦЕЛИ
#  ----
#    GWYF_HookEngine        (dll) — внедряется в процесс игры;
#    gwyf_native_bridge     (dll) — грузится JVM Minecraft через Fabric-мод;
#    gwyfbridge             (exe) — инжектор и диагностика из командной строки;
#    gwyfbridge_selftest    (exe) — самотест (в том числе на Linux);
#    fake_mono_host         (dll) — синтетический хост для интеграционного прогона.
#
#  СБОРКА
#  ------
#    Windows (Visual Studio):
#      cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#      cmake --build build --config Release
#    Linux (только переносимая часть: самотест без Windows-специфики):
#      cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#      cmake --build build && ctest --test-dir build
#
#  JNI: если в системе нет JDK, нативная библиотека для Minecraft пропускается
#  с предупреждением (остальные цели собираются). Достаточно указать
#  -DJAVA_HOME=<путь> или -DJNI_INCLUDE_DIR / -DJNI_LIBRARY.
# ============================================================================
cmake_minimum_required(VERSION 3.20)

project(gwyfbridge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(GWYF_BUILD_TESTS "Собирать самотест и синтетический хост" ON)
option(GWYF_WARNINGS_AS_ERRORS "Считать предупреждения ошибками в нашем коде" ON)

# ── Общие источники ─────────────────────────────────────────────────────────

# Переносимое ядро: собирается и на Linux (самотест), и на Windows.
set(GWYF_CORE_SOURCES
    src/Common.cpp
    src/IpcProtocol.cpp
    src/McWireFormat.cpp
    src/TargetProfile.cpp
    src/VoxelWorld.cpp
)

# Windows-специфика: разделяемая память, Mono, паттерны, MinHook, движок игры.
set(GWYF_WINDOWS_SOURCES
    src/IpcBridge.cpp
    src/PatternScan.cpp
    src/MonoRuntime.cpp
    src/HookEngine.cpp
    src/VoxelTo3DWorld.cpp
    src/GWYF_HookEngine.cpp
)

set(GWYF_MINHOOK_SOURCES
    third_party/minhook/src/hook.c
    third_party/minhook/src/buffer.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c
)

# ── Флаги ───────────────────────────────────────────────────────────────────

function(gwyf_set_common_flags target)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/src/hde
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /MP /wd4100)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
        if(GWYF_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
        if(GWYF_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# MinHook — сторонний код: собираем с меньшим уровнем предупреждений.
function(gwyf_set_third_party_flags target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /wd4100 /wd4152 /wd4201)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -w)
    endif()
endfunction()

# ── Windows-часть: движок, мост, инжектор, самотест ─────────────────────────

if(WIN32)
    # Ядро как объектная библиотека: переиспользуется всеми целями.
    add_library(gwyf_core STATIC ${GWYF_CORE_SOURCES})
    gwyf_set_common_flags(gwyf_core)

    add_library(gwyf_minhook STATIC ${GWYF_MINHOOK_SOURCES})
    gwyf_set_third_party_flags(gwyf_minhook)

    # 1. Движок, который живёт внутри игры.
    add_library(GWYF_HookEngine SHARED ${GWYF_WINDOWS_SOURCES})
    gwyf_set_common_flags(GWYF_HookEngine)
    target_link_libraries(GWYF_HookEngine PRIVATE gwyf_core gwyf_minhook)
    set_target_properties(GWYF_HookEngine PROPERTIES PREFIX "" OUTPUT_NAME "GWYF_HookEngine")
    # Экспорт GWYF_* виден инжектору; __declspec(dllexport) в исходнике уже стоит,
    # поэтому дополнительный .def не нужен.

    # 2. Нативная библиотека для JVM Minecraft (JNI).
    find_package(JNI QUIET)
    if(JNI_FOUND)
        add_library(gwyf_native_bridge SHARED src/Native_JNI_Bridge.cpp src/IpcBridge.cpp)
        gwyf_set_common_flags(gwyf_native_bridge)
        target_include_directories(gwyf_native_bridge PRIVATE ${JNI_INCLUDE_DIRS})
        target_link_libraries(gwyf_native_bridge PRIVATE gwyf_core ${JNI_LIBRARIES})
        set_target_properties(gwyf_native_bridge PROPERTIES OUTPUT_NAME "gwyf_native_bridge")
    else()
        message(WARNING
            "JDK не найден — цель gwyf_native_bridge пропущена. "
            "Укажите -DJAVA_HOME=<путь к JDK> или -DJNI_INCLUDE_DIR=<...>/include -DJNI_LIBRARY=<...>.")
    endif()

    # 3. Инжектор и диагностика.
    add_executable(gwyfbridge tools/cli/GwyfBridgeCli.cpp)
    gwyf_set_common_flags(gwyfbridge)
    target_link_libraries(gwyfbridge PRIVATE gwyf_core)
    set_target_properties(gwyfbridge PROPERTIES OUTPUT_NAME "gwyfbridge")

    if(GWYF_BUILD_TESTS)
        # 4. Синтетический хост: поддельный Mono + фабрика объектов.
        add_library(fake_mono_host SHARED tools/fakehost/fake_mono_host.cpp)
        gwyf_set_common_flags(fake_mono_host)
        set_target_properties(fake_mono_host PROPERTIES PREFIX "")

        # 5. Самотест: всё ядро + Windows-часть.
        add_executable(gwyfbridge_selftest tools/selftest/selftest.cpp ${GWYF_WINDOWS_SOURCES})
        gwyf_set_common_flags(gwyfbridge_selftest)
        target_link_libraries(gwyfbridge_selftest PRIVATE gwyf_core gwyf_minhook)

        enable_testing()
        add_test(NAME portable COMMAND gwyfbridge_selftest --portable)
        add_test(NAME all COMMAND gwyfbridge_selftest)
        # Интеграционный прогон: движок внутри процесса с поддельным Mono.
        add_test(NAME integration
                 COMMAND gwyfbridge_selftest --integration
                         "$<TARGET_FILE_DIR:gwyfbridge_selftest>/GWYF_HookEngine.dll")
        set_tests_properties(integration PROPERTIES
            ENVIRONMENT "PATH=$<TARGET_FILE_DIR:fake_mono_host>;$ENV{PATH}")
    endif()

else()
    # ── Переносимая часть: самотест без Windows-специфики ───────────────────
    add_executable(gwyfbridge_selftest tools/selftest/selftest.cpp ${GWYF_CORE_SOURCES})
    gwyf_set_common_flags(gwyfbridge_selftest)

    enable_testing()
    add_test(NAME portable COMMAND gwyfbridge_selftest --portable)
    add_test(NAME layout-hash COMMAND gwyfbridge_selftest --print-layout-hash)
endif()
