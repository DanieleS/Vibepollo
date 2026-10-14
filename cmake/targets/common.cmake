# common target definitions
# this file will also load platform specific macros

if(APPLE AND NOT SUNSHINE_BUILD_HOMEBREW)
    add_executable(sunshine MACOSX_BUNDLE ${SUNSHINE_TARGET_FILES})
else()
    add_executable(sunshine ${SUNSHINE_TARGET_FILES})
endif()
foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(sunshine ${dep})  # compile these before sunshine
endforeach()

include(${CMAKE_MODULE_PATH}/targets/web.cmake)
add_dependencies(sunshine web_ui)

# platform specific target definitions
if(WIN32)
    include(${CMAKE_MODULE_PATH}/targets/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/targets/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/targets/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/targets/linux.cmake)
    endif()
endif()

target_link_libraries(sunshine ${SUNSHINE_EXTERNAL_LIBRARIES} ${EXTRA_LIBS})
if(TARGET sunshine_libvirtualdisplay_uapi)
    target_link_libraries(sunshine sunshine_libvirtualdisplay_uapi)
endif()
target_compile_definitions(sunshine PUBLIC ${SUNSHINE_DEFINITIONS})

# The per-build version definitions (cmake/prep/build_version.cmake) go to the sources that name
# them, found by scanning, so a new user of the macros needs nothing here. A header using them
# would leave its includers without them: that fails to compile, loudly.
get_target_property(SUNSHINE_TARGET_SOURCES sunshine SOURCES)
foreach(SUNSHINE_TARGET_SOURCE IN LISTS SUNSHINE_TARGET_SOURCES)
    if(SUNSHINE_TARGET_SOURCE MATCHES "\\.(c|cc|cpp|cxx|m|mm)$")
        get_filename_component(SUNSHINE_TARGET_SOURCE_PATH "${SUNSHINE_TARGET_SOURCE}" ABSOLUTE
                BASE_DIR "${CMAKE_SOURCE_DIR}")
        if(EXISTS "${SUNSHINE_TARGET_SOURCE_PATH}")
            file(STRINGS "${SUNSHINE_TARGET_SOURCE_PATH}" SUNSHINE_TARGET_SOURCE_USES_VERSION
                    REGEX "PROJECT_VERSION|PROJECT_RELEASE_DATE|RC_VERSION_" LIMIT_COUNT 1)
            if(SUNSHINE_TARGET_SOURCE_USES_VERSION)
                set_property(SOURCE "${SUNSHINE_TARGET_SOURCE_PATH}" TARGET_DIRECTORY sunshine
                        APPEND PROPERTY COMPILE_DEFINITIONS ${SUNSHINE_VERSION_DEFINITIONS})
            endif()
        endif()
    endif()
endforeach()

# Logging integration flags are provided via SUNSHINE_DEFINITIONS to avoid duplicates
set_target_properties(sunshine PROPERTIES CXX_STANDARD 23)
if(NOT SUNSHINE_BUILD_STEAMOS)
    set_target_properties(sunshine PROPERTIES
            VERSION ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR})
endif()

# CLion complains about unknown flags after running cmake, and cannot add symbols to the index for cuda files
if(CUDA_INHERIT_COMPILE_OPTIONS)
    foreach(flag IN LISTS SUNSHINE_COMPILE_OPTIONS)
        list(APPEND SUNSHINE_COMPILE_OPTIONS_CUDA "$<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=${flag}>")
    endforeach()
endif()

target_compile_options(sunshine PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${SUNSHINE_COMPILE_OPTIONS_CUDA};-std=c++17>)  # cmake-lint: disable=C0301

# docs
if(BUILD_DOCS)
    add_subdirectory(third-party/doxyconfig docs)
endif()

set(TEST_DIR "")

# src/upnp
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/upnp.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES COMPILE_FLAGS -Wno-pedantic)

# GNU/MinGW needs bigobj for confighttp.cpp (exceeds COFF section limit)
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties("${CMAKE_SOURCE_DIR}/src/confighttp.cpp"
            DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
            PROPERTIES COMPILE_FLAGS "-Wa,-mbig-obj")
endif()

# third-party/nanors
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/rswrapper.c"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES COMPILE_FLAGS "-ftree-vectorize -funroll-loops")

# third-party/ViGEmClient
set(VIGEM_COMPILE_FLAGS "")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unknown-pragmas ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-misleading-indentation ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-class-memaccess ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-function ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-variable ")
set_source_files_properties("${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/src/ViGEmClient.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES
        COMPILE_DEFINITIONS "UNICODE=1;ERROR_INVALID_DEVICE_OBJECT_PARAMETER=650"
        COMPILE_FLAGS ${VIGEM_COMPILE_FLAGS})

# src/nvhttp
string(TOUPPER "x${CMAKE_BUILD_TYPE}" BUILD_TYPE)
if("${BUILD_TYPE}" STREQUAL "XDEBUG")
    if(WIN32)
        set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
                DIRECTORY "${CMAKE_SOURCE_DIR}"
                PROPERTIES COMPILE_FLAGS -O2)
    endif()
else()
    add_definitions(-DNDEBUG)
endif()
