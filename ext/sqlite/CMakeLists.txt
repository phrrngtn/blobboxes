cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0148)
  cmake_policy(SET CMP0148 OLD)
endif()
project(blobboxes_sqlite LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Sources are symlinked into this directory from the parent project
set(BB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

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

# Fix pdfium dylib install name on macOS
if(APPLE)
  set(_pdfium_dylib "${pdfium_SOURCE_DIR}/lib/libpdfium.dylib")
  execute_process(COMMAND install_name_tool -id "@rpath/libpdfium.dylib" "${_pdfium_dylib}"
                  ERROR_QUIET)
endif()

# ── nlohmann/json ────────────────────────────────────────────────────
FetchContent_Declare(nlohmann_json
  URL "https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# ── hash-library (MD5) ───────────────────────────────────────────────
FetchContent_Declare(hash_library
  GIT_REPOSITORY https://github.com/stbrumme/hash-library.git
  GIT_TAG        hash_library_v8
  GIT_SHALLOW    TRUE)
FetchContent_Populate(hash_library)
add_library(hash_library STATIC "${hash_library_SOURCE_DIR}/md5.cpp")
target_include_directories(hash_library PUBLIC "${hash_library_SOURCE_DIR}")
if(APPLE)
  file(WRITE "${CMAKE_BINARY_DIR}/endian_shim/endian.h" "#include <machine/endian.h>\n")
  target_include_directories(hash_library PRIVATE "${CMAKE_BINARY_DIR}/endian_shim")
endif()

# ── SQLite amalgamation ──────────────────────────────────────────────
FetchContent_Declare(sqlite3
  URL "https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite3)

# ── Sources ──────────────────────────────────────────────────────────
set(BBOXES_SOURCES ${BB_ROOT}/src/bboxes_core.cpp ${BB_ROOT}/src/bboxes_pdf.cpp)
set(BBOXES_INCLUDES ${BB_ROOT}/include)
set(BBOXES_LIBS pdfium nlohmann_json::nlohmann_json hash_library)

# Helper: copy libpdfium next to a target and set RPATH
function(bboxes_runtime target rpath_origin)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${pdfium_SOURCE_DIR}/lib/libpdfium${CMAKE_SHARED_LIBRARY_SUFFIX}"
      "$<TARGET_FILE_DIR:${target}>/")
  set_target_properties(${target} PROPERTIES
    BUILD_RPATH "${rpath_origin}" INSTALL_RPATH "${rpath_origin}")
endfunction()

# ── SQLite extension ─────────────────────────────────────────────────
add_library(bboxes_sqlite MODULE
  ${BB_ROOT}/sqlite_ext/src/bboxes_sqlite.cpp ${BBOXES_SOURCES})
target_include_directories(bboxes_sqlite PRIVATE
  ${BBOXES_INCLUDES} ${sqlite3_SOURCE_DIR})
target_link_libraries(bboxes_sqlite PRIVATE ${BBOXES_LIBS})
set_target_properties(bboxes_sqlite PROPERTIES
  OUTPUT_NAME "bboxes"
  PREFIX "")
if(APPLE)
  target_link_options(bboxes_sqlite PRIVATE -undefined dynamic_lookup)
endif()
bboxes_runtime(bboxes_sqlite "$<IF:$<PLATFORM_ID:Darwin>,@loader_path,$ORIGIN>")

# Install the extension and libpdfium into the Python package directory
install(TARGETS bboxes_sqlite LIBRARY DESTINATION blobboxes_sqlite)
install(FILES "${pdfium_SOURCE_DIR}/lib/libpdfium${CMAKE_SHARED_LIBRARY_SUFFIX}"
  DESTINATION blobboxes_sqlite)
if(APPLE)
  install(CODE "
    execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink
      bboxes.so \"\${CMAKE_INSTALL_PREFIX}/blobboxes_sqlite/bboxes.dylib\")
  ")
endif()
