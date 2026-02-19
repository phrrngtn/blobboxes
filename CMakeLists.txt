cmake_minimum_required(VERSION 3.20)
project(bboxes LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── PDFium (pre-built) ───────────────────────────────────────────────
set(PDFIUM_VERSION "chromium/7690")
if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
  set(PDFIUM_PLATFORM "mac-arm64")
elseif(APPLE)
  set(PDFIUM_PLATFORM "mac-x64")
elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(PDFIUM_PLATFORM "win-x64")
elseif(WIN32)
  set(PDFIUM_PLATFORM "win-x86")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  set(PDFIUM_PLATFORM "linux-arm64")
else()
  set(PDFIUM_PLATFORM "linux-x64")
endif()
FetchContent_Declare(pdfium
  URL "https://github.com/bblanchon/pdfium-binaries/releases/download/${PDFIUM_VERSION}/pdfium-${PDFIUM_PLATFORM}.tgz"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(pdfium)
set(PDFium_DIR "${pdfium_SOURCE_DIR}" CACHE PATH "" FORCE)
find_package(PDFium REQUIRED)

# ── nlohmann/json ────────────────────────────────────────────────────
FetchContent_Declare(nlohmann_json
  URL "https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# ── Fix pdfium dylib install name on macOS ───────────────────────────
if(APPLE)
  set(_pdfium_dylib "${pdfium_SOURCE_DIR}/lib/libpdfium.dylib")
  execute_process(COMMAND install_name_tool -id "@rpath/libpdfium.dylib" "${_pdfium_dylib}"
                  ERROR_QUIET)
endif()

# ── Optional backends ────────────────────────────────────────────────
option(BUILD_XLSX_BACKEND "Build XLSX backend (requires xlnt)" OFF)
option(BUILD_TEXT_BACKEND "Build text file backend" OFF)
option(BUILD_DOCX_BACKEND "Build DOCX backend (requires pugixml + miniz)" OFF)

# ── XLSX: xlnt via FetchContent ──────────────────────────────────────
if(BUILD_XLSX_BACKEND)
  FetchContent_Declare(xlnt
    GIT_REPOSITORY https://github.com/xlnt-community/xlnt.git
    GIT_TAG v1.6.1
    GIT_SHALLOW TRUE)
  set(XLNT_SHARED OFF CACHE BOOL "" FORCE)
  set(TESTS OFF CACHE BOOL "" FORCE)
  set(SAMPLES OFF CACHE BOOL "" FORCE)
  set(BENCHMARKS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(xlnt)
endif()

# ── DOCX: pugixml + miniz via FetchContent ──────────────────────────
if(BUILD_DOCX_BACKEND)
  FetchContent_Declare(pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG v1.14
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(pugixml)

  FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.0.2
    GIT_SHALLOW TRUE)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
  FetchContent_MakeAvailable(miniz)
  unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)
endif()

# ── SQLite (for SQLite extension) ─────────────────────────────────────
if(BUILD_SQLITE_EXTENSION)
  FetchContent_Declare(sqlite3
    URL "https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(sqlite3)

  # Build sqlite3 amalgamation as a static library
  if(NOT TARGET sqlite3_lib)
    add_library(sqlite3_lib STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
    target_include_directories(sqlite3_lib PUBLIC ${sqlite3_SOURCE_DIR})
    target_compile_definitions(sqlite3_lib PRIVATE SQLITE_CORE)
  endif()
endif()

# ── Sources used by all targets ──────────────────────────────────────
set(BBOXES_SOURCES src/bboxes_core.cpp src/bboxes_pdf.cpp)
set(BBOXES_INCLUDES include)
set(BBOXES_LIBS pdfium nlohmann_json::nlohmann_json)
set(BBOXES_DEFS "")

if(BUILD_XLSX_BACKEND)
  list(APPEND BBOXES_SOURCES src/bboxes_xlsx.cpp)
  list(APPEND BBOXES_LIBS xlnt)
  list(APPEND BBOXES_DEFS BBOXES_HAS_XLSX)
endif()

if(BUILD_TEXT_BACKEND)
  list(APPEND BBOXES_SOURCES src/bboxes_text.cpp)
  list(APPEND BBOXES_DEFS BBOXES_HAS_TEXT)
endif()

if(BUILD_DOCX_BACKEND)
  list(APPEND BBOXES_SOURCES src/bboxes_docx.cpp)
  list(APPEND BBOXES_LIBS pugixml miniz)
  list(APPEND BBOXES_DEFS BBOXES_HAS_DOCX)
endif()

# Helper: copy libpdfium next to a target and set RPATH
function(bboxes_runtime target rpath_origin)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${pdfium_SOURCE_DIR}/lib/libpdfium${CMAKE_SHARED_LIBRARY_SUFFIX}"
      "$<TARGET_FILE_DIR:${target}>/")
  set_target_properties(${target} PROPERTIES
    BUILD_RPATH "${rpath_origin}" INSTALL_RPATH "${rpath_origin}")
endfunction()

# Helper: apply backend defines + includes to a target
function(bboxes_backend_setup target)
  target_compile_definitions(${target} PRIVATE ${BBOXES_DEFS})
endfunction()

# ── Shared library ───────────────────────────────────────────────────
add_library(bboxes SHARED ${BBOXES_SOURCES})
target_include_directories(bboxes PUBLIC ${BBOXES_INCLUDES})
target_link_libraries(bboxes PRIVATE ${BBOXES_LIBS})
bboxes_backend_setup(bboxes)
bboxes_runtime(bboxes "$<IF:$<PLATFORM_ID:Darwin>,@loader_path,$ORIGIN>")

# ── C++ example ──────────────────────────────────────────────────────
add_executable(bboxes_example example/main.cpp ${BBOXES_SOURCES})
target_include_directories(bboxes_example PRIVATE ${BBOXES_INCLUDES})
target_link_libraries(bboxes_example PRIVATE ${BBOXES_LIBS})
bboxes_backend_setup(bboxes_example)
bboxes_runtime(bboxes_example "@executable_path")

# ── Python bindings (nanobind) ───────────────────────────────────────
option(BUILD_PYTHON_BINDINGS "Build nanobind Python module" OFF)
if(BUILD_PYTHON_BINDINGS)
  find_package(Python 3.8 REQUIRED COMPONENTS Interpreter Development.Module)
  execute_process(COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE NB_DIR)
  list(APPEND CMAKE_PREFIX_PATH "${NB_DIR}")
  find_package(nanobind CONFIG REQUIRED)

  nanobind_add_module(blobboxes_ext NB_STATIC python/bindings.cpp ${BBOXES_SOURCES})
  target_include_directories(blobboxes_ext PRIVATE ${BBOXES_INCLUDES})
  target_link_libraries(blobboxes_ext PRIVATE ${BBOXES_LIBS})
  bboxes_backend_setup(blobboxes_ext)
  set_target_properties(blobboxes_ext PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/python/blobboxes")
  bboxes_runtime(blobboxes_ext "$<IF:$<PLATFORM_ID:Darwin>,@loader_path,$ORIGIN>")
endif()

# ── DuckDB extension ────────────────────────────────────────────────
option(BUILD_DUCKDB_EXTENSION "Build DuckDB loadable extension" OFF)
if(BUILD_DUCKDB_EXTENSION)
  add_library(bboxes_duckdb MODULE duckdb_ext/src/bboxes_ext.cpp ${BBOXES_SOURCES})
  target_include_directories(bboxes_duckdb PRIVATE
    ${BBOXES_INCLUDES} duckdb_ext/duckdb_capi)
  target_link_libraries(bboxes_duckdb PRIVATE ${BBOXES_LIBS})
  bboxes_backend_setup(bboxes_duckdb)
  set_target_properties(bboxes_duckdb PROPERTIES
    OUTPUT_NAME "bboxes"
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/duckdb")
  bboxes_runtime(bboxes_duckdb "$<IF:$<PLATFORM_ID:Darwin>,@loader_path,$ORIGIN>")
endif()

# ── SQLite extension ────────────────────────────────────────────────
option(BUILD_SQLITE_EXTENSION "Build SQLite loadable extension" OFF)
if(BUILD_SQLITE_EXTENSION)
  add_library(bboxes_sqlite MODULE sqlite_ext/src/bboxes_sqlite.cpp ${BBOXES_SOURCES})
  target_include_directories(bboxes_sqlite PRIVATE ${BBOXES_INCLUDES} ${sqlite3_SOURCE_DIR})
  target_link_libraries(bboxes_sqlite PRIVATE ${BBOXES_LIBS})
  bboxes_backend_setup(bboxes_sqlite)
  set_target_properties(bboxes_sqlite PROPERTIES
    OUTPUT_NAME "bboxes"
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/sqlite")
  if(APPLE)
    target_link_options(bboxes_sqlite PRIVATE -undefined dynamic_lookup)
    add_custom_command(TARGET bboxes_sqlite POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E create_symlink
        bboxes.so "$<TARGET_FILE_DIR:bboxes_sqlite>/bboxes.dylib")
  endif()
  bboxes_runtime(bboxes_sqlite "$<IF:$<PLATFORM_ID:Darwin>,@loader_path,$ORIGIN>")
endif()
