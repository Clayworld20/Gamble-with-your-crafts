# =============================================================================
#  GwycBridge — нативный мост между казино и Minecraft.
#
#  Что собирается:
#    * gwyc_bridge      — динамическая библиотека (DLL / .so) с C-ABI из
#                         include/gwyc/plugin_abi.h; её и грузит игра;
#    * gwyc_core_tests  — тесты ядра: протокол, кольцо, RLE, воксели, сканер,
#                         хуки и сквозной прогон ABI (без Windows и без Minecraft).
#
#  Сборка (Linux/macOS):
#      cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#      cmake --build build -j
#      ctest --test-dir build --output-on-failure
#
#  Сборка (Windows, MSVC):
#      cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#      cmake --build build --config Release
#      ctest --test-dir build -C Release --output-on-failure
#
#  Сборка (Windows, MinGW-w64 / clang-cl):
#      cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
# =============================================================================
cmake_minimum_required(VERSION 3.20)

# C нужен для MinHook (движок трамплинов — это C, third_party/minhook/src/*.c).
# Без него CMake молча выбрасывает .c-файлы, и линковка падает на unresolved MH_*.
project(GwycBridge
        VERSION 1.0.0
        DESCRIPTION "Мост между казино Gamble With Your Crafts и Minecraft Java Edition"
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Тип сборки" FORCE)
endif()

# Артефакты кладём в один каталог: игре нужно знать, откуда грузить DLL.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(GWYC_BUILD_TESTS "Собрать тесты нативного ядра" ON)
option(GWYC_WITH_JNI "Включить прямые вызовы JVM через JNI (нужны заголовки jni.h)" OFF)
option(GWYC_WERROR "Считать предупреждения ошибками" OFF)

set(GWYC_CORE_SOURCES
    src/GWYF_HookEngine.cpp
    src/Native_JNI_Bridge.cpp
    src/VoxelTo3DWorld.cpp
    src/BridgeDll.cpp)

set(GWYC_PUBLIC_HEADERS
    include/gwyc/protocol.h
    include/gwyc/plugin_abi.h
    include/gwyc/RingBuffer.h
    include/gwyc/SharedChannel.h)

# ── Библиотека моста ────────────────────────────────────────────────────────

add_library(gwyc_bridge SHARED ${GWYC_CORE_SOURCES} ${GWYC_PUBLIC_HEADERS})

target_include_directories(gwyc_bridge
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(gwyc_bridge PRIVATE GWYC_BRIDGE_EXPORTS)

if(GWYC_WITH_JNI)
    target_compile_definitions(gwyc_bridge PRIVATE GWYC_WITH_JNI=1)
    if(APPLE)
        target_compile_options(gwyc_bridge PRIVATE "-I/usr/include")
        find_library(JVM_LIBRARY jvm HINTS /Library/Java/JavaVirtualMachines)
        if(JVM_LIBRARY)
            target_link_libraries(gwyc_bridge PRIVATE "${JVM_LIBRARY}")
        endif()
    elseif(UNIX)
        find_package(JNI QUIET)
        if(JNI_FOUND)
            target_include_directories(gwyc_bridge PRIVATE ${JNI_INCLUDE_DIRS})
            target_link_libraries(gwyc_bridge PRIVATE ${JNI_LIBRARIES})
        else()
            message(WARNING "GWYC_WITH_JNI=ON, но JNI не найден: собираю без прямого JNI-пути")
            target_compile_definitions(gwyc_bridge PRIVATE GWYC_WITH_JNI=0)
        endif()
    else()
        # Windows: jvm.dll загружается динамически (см. Native_JNI_Bridge.cpp),
        # поэтому линковать нечего — нужны только заголовки из JAVA_HOME.
        if(DEFINED ENV{JAVA_HOME})
            target_include_directories(gwyc_bridge PRIVATE "$ENV{JAVA_HOME}/include" "$ENV{JAVA_HOME}/include/win32")
        else()
            message(WARNING "JAVA_HOME не задан: включите его, чтобы собрать JNI-путь")
        endif()
    endif()
else()
    target_compile_definitions(gwyc_bridge PRIVATE GWYC_WITH_JNI=0)
endif()

if(WIN32)
    # MinHook — движок трамплинов. Собирается только под Windows: на других
    # платформах GWYF_HookEngine работает в теневом режиме (см. заголовок).
    # Диссектор берём ровно под разрядность, как в апстриме MinHook.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(GWYC_MINHOOK_HDE third_party/minhook/src/hde/hde64.c)
    else()
        set(GWYC_MINHOOK_HDE third_party/minhook/src/hde/hde32.c)
    endif()

    set(GWYC_MINHOOK_SOURCES
        third_party/minhook/src/buffer.c
        third_party/minhook/src/hook.c
        third_party/minhook/src/trampoline.c
        ${GWYC_MINHOOK_HDE})

    target_sources(gwyc_bridge PRIVATE ${GWYC_MINHOOK_SOURCES})

    target_include_directories(gwyc_bridge PRIVATE third_party/minhook/include)
    target_compile_definitions(gwyc_bridge PRIVATE _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)

    # MinHook — сторонний код (BSD-2-Clause, вендорен без изменений): свой /W4 /WX
    # к нему не применяем, иначе чужие предупреждения ломают нашу сборку.
    set_source_files_properties(${GWYC_MINHOOK_SOURCES}
        PROPERTIES COMPILE_OPTIONS "$<$<C_COMPILER_ID:MSVC>:/w>")

    if(MSVC)
        # /W4 /WX — для нашего кода. Заголовки стандартной библиотеки помечаем
        # внешними: их собственные предупреждения (напр. xutility) не должны
        # ломать сборку проекта.
        target_compile_options(gwyc_bridge PRIVATE /W4 /permissive- /utf-8 /EHsc /MP
                                               /external:anglebrackets /external:W0)
        if(GWYC_WERROR)
            target_compile_options(gwyc_bridge PRIVATE /WX)
        endif()
    else()
        target_compile_options(gwyc_bridge PRIVATE -Wall -Wextra)
    endif()
else()
    target_compile_options(gwyc_bridge PRIVATE -Wall -Wextra)
    if(GWYC_WERROR)
        target_compile_options(gwyc_bridge PRIVATE -Werror)
    endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(gwyc_bridge PRIVATE Threads::Threads)

set_target_properties(gwyc_bridge PROPERTIES
    OUTPUT_NAME "GWYFCraftsBridge"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    POSITION_INDEPENDENT_CODE ON)

# ── Тесты ядра ─────────────────────────────────────────────────────────────

if(GWYC_BUILD_TESTS)
    add_executable(gwyc_core_tests tests/core_tests.cpp)
    target_include_directories(gwyc_core_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src")

    # Ядро (включая движок хуков) собирается прямо в тест на всех платформах:
    # так тест работает с одним экземпляром MinHook и не зависит от экспорта
    # C++-символов из DLL — наружу торчит только C-ABI (plugin_abi.h).
    target_sources(gwyc_core_tests PRIVATE ${GWYC_CORE_SOURCES})
    target_compile_definitions(gwyc_core_tests PRIVATE GWYC_BRIDGE_STATIC)

    if(WIN32)
        target_sources(gwyc_core_tests PRIVATE ${GWYC_MINHOOK_SOURCES})
        target_include_directories(gwyc_core_tests PRIVATE third_party/minhook/include)
        target_compile_definitions(gwyc_core_tests PRIVATE _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()

    if(MSVC)
        target_compile_options(gwyc_core_tests PRIVATE /W4 /permissive- /utf-8 /EHsc
                                                   /external:anglebrackets /external:W0)
        if(GWYC_WERROR)
            target_compile_options(gwyc_core_tests PRIVATE /WX)
        endif()
    else()
        target_compile_options(gwyc_core_tests PRIVATE -Wall -Wextra)
        if(GWYC_WERROR)
            target_compile_options(gwyc_core_tests PRIVATE -Werror)
        endif()
    endif()

    target_link_libraries(gwyc_core_tests PRIVATE Threads::Threads)

    enable_testing()
    add_test(NAME gwyc_core_tests COMMAND gwyc_core_tests)
    set_tests_properties(gwyc_core_tests PROPERTIES TIMEOUT 300)
endif()
