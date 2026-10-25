cmake_minimum_required(VERSION 3.16)
project(csv_import_pipeline VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -------------------- options --------------------
option(ENABLE_ASAN "Build with AddressSanitizer (MSVC x64 only)" OFF)
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_BENCH "Build benchmarks" ON)

# -------------------- library --------------------
set(CSVP_SOURCES
    src/parser.cpp
    src/validator.cpp
    src/pipeline.cpp
)

add_library(csvp STATIC ${CSVP_SOURCES})
target_include_directories(csvp PUBLIC include)

# MSVC-specific settings
if(MSVC)
    target_compile_options(csvp PUBLIC
        /utf-8          # source encoding
        /permissive-    # standards conformance
        /Zc:__cplusplus # correct __cplusplus value
        /EHsc           # C++ exception handling
        /W4             # warning level 4
    )
    if(ENABLE_ASAN)
        target_compile_options(csvp PUBLIC /fsanitize=address)
        target_link_options(csvp PUBLIC /fsanitize=address)
    endif()
else()
    target_compile_options(csvp PUBLIC -Wall -Wextra -pedantic)
    if(ENABLE_ASAN)
        target_compile_options(csvp PUBLIC -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(csvp PUBLIC -fsanitize=address)
    endif()
endif()

# -------------------- tests --------------------
if(BUILD_TESTS)
    enable_testing()

    # Use bundled googletest (pre-downloaded to third_party/)
    # For offline environments, comment out add_subdirectory and use:
    # find_package(GTest REQUIRED)

    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(third_party/googletest)

    set(TEST_SOURCES
        tests/test_parser.cpp
        tests/test_validator.cpp
        tests/test_pipeline.cpp
        tests/test_concurrent.cpp
    )

    add_executable(csvp_tests ${TEST_SOURCES})
    target_link_libraries(csvp_tests PRIVATE csvp GTest::gtest GTest::gtest_main)
    target_compile_definitions(csvp_tests PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/data/")

    if(MSVC)
        target_compile_options(csvp_tests PRIVATE /utf-8 /permissive- /EHsc /W4)
        if(ENABLE_ASAN)
            target_compile_options(csvp_tests PRIVATE /fsanitize=address)
            target_link_options(csvp_tests PRIVATE /fsanitize=address)
        endif()
    endif()

    include(GoogleTest)
    gtest_discover_tests(csvp_tests)
endif()

# -------------------- benchmark --------------------
if(BUILD_BENCH)
    add_executable(csvp_bench bench/bench_import.cpp)
    target_link_libraries(csvp_bench PRIVATE csvp)


    # 默认运行参数：csvp_bench sample_utf8.csv 3 output.txt
    # 直接使用源码目录下的 data，避免把大文件拷到编译目录。
    set_target_properties(csvp_bench PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data"
        VS_DEBUGGER_COMMAND_ARGUMENTS "sample_utf8.csv 3 output.txt"
        DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data"
    )

    # 优化级别跟随构建配置（CMAKE_CXX_FLAGS_RELEASE 已含 /O2，Debug 为 /Od），
    # 不要在这里显式加 /O2，否则与 Debug 默认的 /RTC1 冲突：
    #   "error D8016: /O2 和 /RTC1 命令行选项不兼容"
    if(MSVC)
        target_compile_options(csvp_bench PRIVATE /utf-8 /permissive- /EHsc /W4)
    else()
        target_compile_options(csvp_bench PRIVATE)
    endif()
endif()