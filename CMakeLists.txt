cmake_minimum_required(VERSION 3.24)

project(ackord
    VERSION 0.1.0
    LANGUAGES C
)

option(ACKORD_BUILD_TESTS "Build Ackord tests" ${PROJECT_IS_TOP_LEVEL})
option(ACKORD_BUILD_EXAMPLES "Build Ackord examples" ${PROJECT_IS_TOP_LEVEL})
option(ACKORD_INSTALL "Install Ackord" ${PROJECT_IS_TOP_LEVEL})

include(GNUInstallDirs)

function(ackord_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
        )
    endif()
endfunction()

add_library(ackord STATIC src/ackord.c)
add_library(ackord::ackord ALIAS ackord)

target_compile_features(ackord PUBLIC c_std_11)
set_target_properties(ackord PROPERTIES C_EXTENSIONS OFF)

target_include_directories(ackord
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

ackord_warnings(ackord)

if(ACKORD_BUILD_TESTS)
    enable_testing()

    add_executable(ackord_tests tests/ackord_tests.c)
    target_link_libraries(ackord_tests PRIVATE ackord::ackord)
    ackord_warnings(ackord_tests)
    add_test(NAME ackord_tests COMMAND ackord_tests)

    add_executable(ackord_session_tests tests/ackord_session_tests.c)
    target_link_libraries(ackord_session_tests PRIVATE ackord::ackord)
    ackord_warnings(ackord_session_tests)
    add_test(NAME ackord_session_tests COMMAND ackord_session_tests)

    if(CMAKE_NM AND NOT MSVC)
        add_test(
            NAME ackord_runtime_imports
            COMMAND "${CMAKE_COMMAND}"
                "-DNM=${CMAKE_NM}"
                "-DLIBRARY=$<TARGET_FILE:ackord>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_imports.cmake"
        )
    endif()
endif()

if(ACKORD_BUILD_EXAMPLES)
    add_executable(ackord_counter examples/counter.c)
    target_link_libraries(ackord_counter PRIVATE ackord::ackord)
    ackord_warnings(ackord_counter)

    if(ACKORD_BUILD_TESTS)
        add_test(NAME ackord_counter COMMAND ackord_counter)
    endif()
endif()

if(ACKORD_INSTALL)
    include(CMakePackageConfigHelpers)

    configure_package_config_file(
        cmake/ackordConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/ackordConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ackord"
    )

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/ackordConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion
    )

    install(TARGETS ackord
        EXPORT ackordTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )

    install(DIRECTORY include/
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )

    install(EXPORT ackordTargets
        NAMESPACE ackord::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ackord"
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ackordConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ackordConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ackord"
    )
endif()
