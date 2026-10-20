cmake_minimum_required(VERSION 3.16)
# CEF's CMake helpers probe both C and C++ compiler flags.
project(cefnest-helper LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT CEF_ROOT AND DEFINED ENV{CEF_ROOT})
  set(CEF_ROOT "$ENV{CEF_ROOT}")
endif()
if(NOT CEF_ROOT)
  message(FATAL_ERROR
    "CEF_ROOT is not set. Pass -DCEF_ROOT=/path/to/cef_binary_... "
    "or export CEF_ROOT.")
endif()
get_filename_component(CEF_ROOT "${CEF_ROOT}" ABSOLUTE)

list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)

# Put the executable and all CEF runtime files in build/<CONFIG>/.
SET_CEF_TARGET_OUT_DIR()
file(MAKE_DIRECTORY "${CEF_TARGET_OUT_DIR}")

ADD_LOGICAL_TARGET(libcef_lib "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
add_subdirectory("${CEF_ROOT}/libcef_dll" libcef_dll_wrapper)

add_executable(cefnest-helper src/main.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(cefnest-helper)
target_link_libraries(cefnest-helper PRIVATE
  libcef_lib
  libcef_dll_wrapper
  ${CEF_STANDARD_LIBS}
)

# libcef.so is copied next to the executable by COPY_FILES below. Use $ORIGIN
# so the helper runs without LD_LIBRARY_PATH.
set_target_properties(cefnest-helper PROPERTIES
  BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "$ORIGIN"
)

COPY_FILES(cefnest-helper "${CEF_BINARY_FILES}"
           "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
COPY_RESOURCES(cefnest-helper "${CEF_RESOURCE_FILES}" "Resources"
               "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")
