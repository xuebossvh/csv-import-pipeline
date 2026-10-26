cmake_minimum_required(VERSION 3.16)
project(csv_import_pipeline VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -------------------- options --------------------
option(ENABLE_ASAN "Build with AddressSanitizer (requires the matching compiler runtime)" OFF)
option(BUILD_TESTS "Build local unit tests (requires tests and bundled GoogleTest)" OFF)
option(BUILD_BENCH "Build benchmarks" ON)

# -------------------- library --------------------
set(CSVP_SOURCES
    src/parser.cpp
    src/validator.cpp
    src/pipeline.cpp
)

add_library(csvp STATIC ${CSVP_SOURCES})
target_include_directories(csvp PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(csvp PUBLIC Threads::Threads)

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
        string(REGEX REPLACE "/RTC[1su]" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REPLACE "/ZI" "/Zi" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        target_compile_options(csvp PUBLIC /fsanitize=address)
        target_link_options(csvp INTERFACE /INCREMENTAL:NO)
    endif()
else()
    target_compile_options(csvp PUBLIC -Wall -Wextra -pedantic)
    if(ENABLE_ASAN)
        target_compile_options(csvp PUBLIC -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(csvp PUBLIC -fsanitize=address)
    endif()
endif()

# -------------------- tests --------------------
function(csvp_copy_asan_runtime target)
    if(MSVC AND ENABLE_ASAN)
        get_filename_component(_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        file(GLOB _asan_runtime "${_compiler_dir}/clang_rt.asan_dynamic-*.dll")
        if(NOT _asan_runtime)
            message(FATAL_ERROR "ASan runtime DLL was not found beside ${CMAKE_CXX_COMPILER}")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_asan_runtime} "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

if(BUILD_TESTS)
    enable_testing()

    # Use bundled googletest (pre-downloaded to third_party/)
    # For offline environments, comment out add_subdirectory and use:
    # find_package(GTest REQUIRED)

    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(third_party/googletest)

    # MSVC's STL container annotations must agree across every linked library.
    if(ENABLE_ASAN)
        foreach(_test_library gtest gtest_main gmock gmock_main)
            if(TARGET ${_test_library})
                if(MSVC)
                    target_compile_options(${_test_library} PRIVATE /fsanitize=address)
                else()
                    target_compile_options(${_test_library} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
                endif()
            endif()
        endforeach()
    endif()

    set(TEST_SOURCES
        tests/test_parser.cpp
        tests/test_validator.cpp
        tests/test_pipeline.cpp
        tests/test_concurrent.cpp
    )

    add_executable(csvp_tests ${TEST_SOURCES})
    target_link_libraries(csvp_tests PRIVATE csvp GTest::gtest GTest::gtest_main)
    target_compile_definitions(csvp_tests PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/")

    if(MSVC)
        target_compile_options(csvp_tests PRIVATE /utf-8 /permissive- /EHsc /W4)
    endif()

    csvp_copy_asan_runtime(csvp_tests)
    include(GoogleTest)
    gtest_discover_tests(csvp_tests)
endif()

# -------------------- benchmark --------------------
if(BUILD_BENCH)
    add_executable(csvp_bench bench/bench_import.cpp)
    target_link_libraries(csvp_bench PRIVATE csvp)
    if(WIN32)
        target_link_libraries(csvp_bench PRIVATE shell32)
    endif()
    csvp_copy_asan_runtime(csvp_bench)


    # 默认运行参数：csvp_bench sample_utf8.csv 3 output.txt
    # 直接使用源码目录下的 data，避免把大文件拷到编译目录。
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/sample_utf8.csv")
        set_target_properties(csvp_bench PROPERTIES
            VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data"
            VS_DEBUGGER_COMMAND_ARGUMENTS "sample_utf8.csv 3 output.txt"
            DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data"
        )
    endif()

    # 优化级别跟随构建配置（CMAKE_CXX_FLAGS_RELEASE 已含 /O2，Debug 为 /Od），
    # 不要在这里显式加 /O2，否则与 Debug 默认的 /RTC1 冲突：
    #   "error D8016: /O2 和 /RTC1 命令行选项不兼容"
    if(MSVC)
        target_compile_options(csvp_bench PRIVATE /utf-8 /permissive- /EHsc /W4)
    else()
        target_compile_options(csvp_bench PRIVATE)
    endif()
endif()
