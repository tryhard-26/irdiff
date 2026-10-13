cmake_minimum_required(VERSION 3.20)
project(irdiff VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(IRDIFF_STATIC_LLVM "Link LLVM component archives instead of the shared library" OFF)
option(IRDIFF_FULLY_STATIC "Also link libstdc++/libgcc statically (Linux)" OFF)
option(IRDIFF_COVERAGE "Instrument for source-based coverage (clang only)" OFF)
option(IRDIFF_WERROR "Treat warnings as errors" OFF)
option(IRDIFF_BUILD_TESTS "Build the test suite" ON)
set(IRDIFF_TARGETS "all" CACHE STRING "LLVM targets to initialize: 'all' or a ;-list such as X86;AArch64")

find_package(LLVM REQUIRED CONFIG)
if(LLVM_VERSION_MAJOR LESS 19)
  message(FATAL_ERROR "irdiff requires LLVM 19 or newer (found ${LLVM_PACKAGE_VERSION})")
endif()
if(LLVM_VERSION_MAJOR GREATER 23)
  message(WARNING "LLVM ${LLVM_PACKAGE_VERSION} is newer than the tested range (19-23)")
endif()
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

if(IRDIFF_TARGETS STREQUAL "all")
  set(irdiff_targets ${LLVM_TARGETS_TO_BUILD})
else()
  set(irdiff_targets ${IRDIFF_TARGETS})
endif()
set(irdiff_target_init "")
foreach(t ${irdiff_targets})
  string(APPEND irdiff_target_init "X(${t})\n")
endforeach()
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/gen/Targets.def CONTENT "${irdiff_target_init}")

execute_process(COMMAND git describe --tags --always --dirty
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE irdiff_git
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
configure_file(cmake/Version.h.in ${CMAKE_BINARY_DIR}/gen/Version.h @ONLY)

add_library(irdiff_core STATIC
  src/Model.cpp
  src/Input.cpp
  src/Canon.cpp
  src/Match.cpp
  src/Differ.cpp
  src/Analyses.cpp
  src/Pipeline.cpp
  src/Report.cpp)
target_include_directories(irdiff_core PUBLIC src ${CMAKE_BINARY_DIR}/gen)
target_include_directories(irdiff_core SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
separate_arguments(llvm_defs NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(irdiff_core PUBLIC ${llvm_defs})
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(irdiff_core PUBLIC -fno-rtti)
endif()

if(IRDIFF_STATIC_LLVM AND TARGET zstd::libzstd_shared)
  get_target_property(zstd_shared zstd::libzstd_shared IMPORTED_LOCATION)
  if(NOT zstd_shared)
    get_target_property(zstd_shared zstd::libzstd_shared IMPORTED_LOCATION_RELEASE)
  endif()
  get_filename_component(zstd_dir "${zstd_shared}" DIRECTORY)
  find_library(IRDIFF_ZSTD_STATIC NAMES libzstd.a HINTS "${zstd_dir}" NO_DEFAULT_PATH)
  if(IRDIFF_ZSTD_STATIC)
    message(STATUS "Linking zstd statically: ${IRDIFF_ZSTD_STATIC}")
    set_target_properties(zstd::libzstd_shared PROPERTIES
      IMPORTED_LOCATION "${IRDIFF_ZSTD_STATIC}"
      IMPORTED_LOCATION_RELEASE "${IRDIFF_ZSTD_STATIC}")
  endif()
endif()

if(IRDIFF_STATIC_LLVM OR NOT LLVM_LINK_LLVM_DYLIB)
  set(irdiff_components core irreader bitreader bitwriter analysis passes support
      remarks target targetparser transformutils scalaropts ipo vectorize codegen)
  foreach(t ${irdiff_targets})
    list(APPEND irdiff_components ${t}CodeGen ${t}Desc ${t}Info)
  endforeach()
  llvm_map_components_to_libnames(irdiff_llvm_libs ${irdiff_components})
else()
  set(irdiff_llvm_libs LLVM)
endif()
target_link_libraries(irdiff_core PUBLIC ${irdiff_llvm_libs})

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(irdiff_core PUBLIC -Wall -Wextra -Wno-unused-parameter)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(irdiff_core PUBLIC -Wno-psabi)
  endif()
  if(IRDIFF_WERROR)
    target_compile_options(irdiff_core PUBLIC -Werror)
  endif()
endif()
if(IRDIFF_COVERAGE)
  target_compile_options(irdiff_core PUBLIC -fprofile-instr-generate -fcoverage-mapping)
  target_link_options(irdiff_core PUBLIC -fprofile-instr-generate)
endif()

add_executable(irdiff src/main.cpp)
target_link_libraries(irdiff PRIVATE irdiff_core)
if(IRDIFF_FULLY_STATIC AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(irdiff PRIVATE -static-libstdc++ -static-libgcc -Wl,--as-needed)
endif()
if(IRDIFF_STATIC_LLVM AND APPLE)
  target_link_options(irdiff PRIVATE -Wl,-dead_strip_dylibs -Wl,-dead_strip)
endif()

include(GNUInstallDirs)
install(TARGETS irdiff RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES docs/irdiff.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

if(IRDIFF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
