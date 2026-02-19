cmake_minimum_required(VERSION 3.20)
project(pdf_bounding_boxes LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── PDFium (pre-built binaries) ─────────────────────────────────────
set(PDFIUM_VERSION "chromium/7690")
if(APPLE)
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    set(PDFIUM_PLATFORM "mac-arm64")
  else()
    set(PDFIUM_PLATFORM "mac-x64")
  endif()
elseif(WIN32)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(PDFIUM_PLATFORM "win-x64")
  else()
    set(PDFIUM_PLATFORM "win-x86")
  endif()
else()
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set(PDFIUM_PLATFORM "linux-arm64")
  else()
    set(PDFIUM_PLATFORM "linux-x64")
  endif()
endif()

FetchContent_Declare(pdfium
  URL "https://github.com/bblanchon/pdfium-binaries/releases/download/${PDFIUM_VERSION}/pdfium-${PDFIUM_PLATFORM}.tgz"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(pdfium)

# The tarball ships PDFiumConfig.cmake at its root
set(PDFium_DIR "${pdfium_SOURCE_DIR}" CACHE PATH "" FORCE)
set(PDFIUM_LIB_DIR "${pdfium_SOURCE_DIR}/lib" CACHE PATH "" FORCE)
find_package(PDFium REQUIRED)

# ── nlohmann/json (header-only) ─────────────────────────────────────
FetchContent_Declare(nlohmann_json
  URL "https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ── library ─────────────────────────────────────────────────────────
add_library(pdf_bboxes src/pdf_bboxes.cpp)
target_include_directories(pdf_bboxes PUBLIC include)
target_link_libraries(pdf_bboxes
  PRIVATE pdfium
  PRIVATE nlohmann_json::nlohmann_json
)

# ── example ─────────────────────────────────────────────────────────
add_executable(pdf_bboxes_example example/main.cpp)
target_link_libraries(pdf_bboxes_example PRIVATE pdf_bboxes)

# The pre-built pdfium dylib has install name "./libpdfium.dylib" (cwd-relative).
# Fix it to use @rpath so executables work from any directory.
if(APPLE)
  set(_pdfium_dylib "${pdfium_SOURCE_DIR}/lib/libpdfium.dylib")
  add_custom_command(TARGET pdf_bboxes POST_BUILD
    COMMAND install_name_tool -id "@rpath/libpdfium.dylib" "${_pdfium_dylib}" || true
    COMMENT "Fixing pdfium dylib install name"
  )
endif()

# Copy libpdfium next to binaries and set rpath
add_custom_command(TARGET pdf_bboxes_example POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${pdfium_SOURCE_DIR}/lib/$<IF:$<PLATFORM_ID:Windows>,pdfium.dll,libpdfium${CMAKE_SHARED_LIBRARY_SUFFIX}>"
    "$<TARGET_FILE_DIR:pdf_bboxes_example>/"
)
set_target_properties(pdf_bboxes_example PROPERTIES
  BUILD_RPATH "@executable_path"
  INSTALL_RPATH "@executable_path"
)

# ── Python bindings (nanobind) ───────────────────────────────────────
option(BUILD_PYTHON_BINDINGS "Build nanobind Python module" OFF)

if(BUILD_PYTHON_BINDINGS)
  find_package(Python 3.8 REQUIRED COMPONENTS Interpreter Development.Module)
  execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE NB_DIR
  )
  list(APPEND CMAKE_PREFIX_PATH "${NB_DIR}")
  find_package(nanobind CONFIG REQUIRED)

  nanobind_add_module(pdf_bboxes_ext NB_STATIC python/bindings.cpp)
  target_link_libraries(pdf_bboxes_ext PRIVATE pdf_bboxes)

  # Place the .so in python/pdf_bboxes/ for editable installs
  set_target_properties(pdf_bboxes_ext PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/python/pdf_bboxes"
  )

  # Copy libpdfium next to the extension and set RPATH
  add_custom_command(TARGET pdf_bboxes_ext POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${pdfium_SOURCE_DIR}/lib/$<IF:$<PLATFORM_ID:Windows>,pdfium.dll,libpdfium${CMAKE_SHARED_LIBRARY_SUFFIX}>"
      "$<TARGET_FILE_DIR:pdf_bboxes_ext>/"
  )
  if(APPLE)
    set_target_properties(pdf_bboxes_ext PROPERTIES
      BUILD_RPATH "@loader_path"
      INSTALL_RPATH "@loader_path"
    )
  else()
    set_target_properties(pdf_bboxes_ext PROPERTIES
      BUILD_RPATH "$ORIGIN"
      INSTALL_RPATH "$ORIGIN"
    )
  endif()
endif()
