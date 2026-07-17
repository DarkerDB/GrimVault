cmake_minimum_required (VERSION 3.28)

# Versioning: derive from git describe when available, fall back to 0.0.0.
include (cmake/Version.cmake)
grimvault_detect_version (GRIMVAULT_VERSION)

project (grimvault
   VERSION     ${GRIMVAULT_VERSION}
   LANGUAGES   CXX
   DESCRIPTION "GrimVault native client"
)

if (NOT WIN32)
   message (FATAL_ERROR "GrimVault is Windows-only. Configure with the windows-msvc preset.")
endif ()

set (CMAKE_CXX_STANDARD          23)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS         OFF)

list (APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include (Modules)
include (Signing)
include (EmbedSql)
include (Env)
include (AppcastUrl)

# ---- Global compile flags ----

add_compile_definitions (
   NOMINMAX
   WIN32_LEAN_AND_MEAN
   WINRT_LEAN_AND_MEAN
   UNICODE
   _UNICODE
)

if (MSVC)
   add_compile_options (/W4 /permissive- /Zc:__cplusplus /bigobj /await /EHsc /utf-8)
   add_link_options (/GUARD:CF /CETCOMPAT)
endif ()

# ---- Dependencies (vcpkg manifest) ----

find_package (Qt6 6.6 REQUIRED COMPONENTS
   Core Gui Widgets Quick QuickWidgets Qml Network Charts Sql Concurrent
)

# LinguistTools comes from the vcpkg `qttools` port (not `qtbase`). Treat as
# optional so dev builds don't require the heavier qttools compile; translations
# get baked only when it's available.
find_package (Qt6LinguistTools CONFIG QUIET)
if (Qt6LinguistTools_FOUND)
   set (GRIMVAULT_HAS_LINGUIST_TOOLS ON)
   message (STATUS "Qt6LinguistTools found: translations will be built")
else ()
   set (GRIMVAULT_HAS_LINGUIST_TOOLS OFF)
   message (STATUS "Qt6LinguistTools not found: skipping .ts -> .qm step (add `qttools` to vcpkg.json to enable)")
endif ()

qt_standard_project_setup (REQUIRES 6.6)

find_package (SQLiteCpp  CONFIG REQUIRED)
find_package (SQLite3           REQUIRED)
find_package (spdlog     CONFIG REQUIRED)
find_package (fmt        CONFIG REQUIRED)
find_package (nlohmann_json CONFIG REQUIRED)

find_package (OpenCV     CONFIG REQUIRED)

# WinSparkle is installed by vcpkg without a CMake config file. Hand-wire
# it: include path + import library. The DLL is staged into the install
# layout by windeployqt's runtime-deps step (TARGET_RUNTIME_DLLS).
find_path    (WINSPARKLE_INCLUDE_DIR winsparkle/winsparkle.h REQUIRED)
find_library (WINSPARKLE_LIBRARY     NAMES WinSparkle         REQUIRED)
add_library  (winsparkle::winsparkle UNKNOWN IMPORTED)
set_target_properties (winsparkle::winsparkle PROPERTIES
   IMPORTED_LOCATION             "${WINSPARKLE_LIBRARY}"
   INTERFACE_INCLUDE_DIRECTORIES "${WINSPARKLE_INCLUDE_DIR}"
)

find_package (CURL       CONFIG REQUIRED)
find_package (OpenSSL           REQUIRED)

# ---- Generated version header ----

grimvault_write_version_header (
   "${CMAKE_BINARY_DIR}/generated/gv/core/version.h"
   "${GRIMVAULT_VERSION}"
)

grimvault_write_appcast_header (
   "${CMAKE_BINARY_DIR}/generated/gv/update/appcast_url.h"
)

grimvault_write_env_header (
   "${CMAKE_BINARY_DIR}/generated/gv/core/env.h"
)

# ---- Module subdirectories ----

grimvault_add_module (core)
target_link_libraries (gv_core PUBLIC
   fmt::fmt
   spdlog::spdlog
)

grimvault_add_module (db)
target_link_libraries (gv_db PUBLIC
   gv::core
   SQLiteCpp
   SQLite3::SQLite3
   nlohmann_json::nlohmann_json
)
grimvault_embed_sql (gv_db
   "${CMAKE_SOURCE_DIR}/db/migrations"
   "${CMAKE_BINARY_DIR}/generated/db_embed/migrations"
)
target_include_directories (gv_db PRIVATE
   "${CMAKE_BINARY_DIR}/generated/db_embed"
)

grimvault_add_module (capture)
target_link_libraries (gv_capture
   PUBLIC
      gv::core
   PRIVATE
      d3d11
      dxgi
      dxguid
      dwmapi
      windowsapp
      Gdi32
      User32
      Shcore
)
find_path (STB_INCLUDE_DIR stb_image.h REQUIRED)
target_include_directories (gv_capture PRIVATE "${STB_INCLUDE_DIR}")

grimvault_add_module (vision)
target_link_libraries (gv_vision PUBLIC
   gv::core
   gv::capture
   opencv_core
   opencv_dnn
   opencv_imgproc
)

grimvault_add_module (ocr)
target_link_libraries (gv_ocr PUBLIC
   gv::core
   gv::capture
   gv::vision
   opencv_core
   opencv_dnn
   opencv_imgcodecs
   opencv_imgproc
   nlohmann_json::nlohmann_json
)

grimvault_add_module (update)
set_target_properties (gv_update PROPERTIES AUTOMOC ON)
target_link_libraries (gv_update
   PUBLIC
      gv::core
      Qt6::Core
   PRIVATE
      winsparkle::winsparkle
)

grimvault_add_module (auth)
set_target_properties (gv_auth PROPERTIES AUTORCC ON)
target_sources (gv_auth PRIVATE
   "${CMAKE_SOURCE_DIR}/src/auth/auth.qrc"
)
target_link_libraries (gv_auth
   PUBLIC
      gv::core
      nlohmann_json::nlohmann_json
      Qt6::Core
   PRIVATE
      CURL::libcurl
      OpenSSL::Crypto
      Qt6::Gui
)
if (WIN32)
   target_link_libraries (gv_auth PRIVATE Ws2_32 Advapi32)
endif ()

grimvault_add_module (api)
target_link_libraries (gv_api
   PUBLIC
      gv::core
      gv::auth
      gv::db
      nlohmann_json::nlohmann_json
   PRIVATE
      CURL::libcurl
)

grimvault_add_module (cli)
target_link_libraries (gv_cli
   PUBLIC
      gv::core
      gv::auth
      gv::api
      gv::db
      Qt6::Core
   PRIVATE
      nlohmann_json::nlohmann_json
)

grimvault_add_module (app)
set_target_properties (gv_app PROPERTIES AUTOMOC ON)
target_link_libraries (gv_app
   PUBLIC
      gv::core
      gv::db
      gv::ocr
      gv::api
      gv::ui
      Qt6::Core
)

grimvault_add_module (ui)
set_target_properties (gv_ui PROPERTIES
   AUTOMOC ON
   AUTOUIC ON
   AUTORCC ON
)
target_sources (gv_ui PRIVATE
   "${CMAKE_SOURCE_DIR}/qml/qml.qrc"
)
target_link_libraries (gv_ui PUBLIC
   gv::core
   gv::api
   nlohmann_json::nlohmann_json
   Qt6::Core
   Qt6::Gui
   Qt6::Widgets
   Qt6::Quick
   Qt6::Qml
)
if (WIN32)
   # gv/ui/screen.cpp: GetDpiForMonitor (Shcore), SetWindowPos (User32).
   # gv/ui/webview_host.cpp: WebView2 loader + DirectComposition (dcomp).
   find_package (unofficial-webview2 CONFIG REQUIRED)
   find_package (wil                 CONFIG REQUIRED)
   target_link_libraries (gv_ui PRIVATE
      unofficial::webview2::webview2
      WIL::WIL
      dcomp
      Shcore
      User32
   )
endif ()

# ---- Application entry ----

qt_add_executable (grimvault WIN32
   src/main.cpp
)

# Embed our app.manifest. Adding it as a target source is the canonical
# CMake path: CMake detects the .manifest extension and routes it through
# the linker as an additional manifest input, deduping against the default
# manifest resource it also generates. Our manifest is asInvoker (drops the
# old requireAdministrator) and adds DPI awareness + Win10/11 supportedOS
# on top of the linker defaults.
if (MSVC)
   target_sources (grimvault PRIVATE "${CMAKE_SOURCE_DIR}/app.manifest")
endif ()

# Exe shell icon + VERSIONINFO (Explorer, taskbar, Task Manager). The .ico
# is committed; regenerate from Icon-324x356.png via tools/build/png_to_ico.py
# if the art changes.
if (WIN32)
   set (GRIMVAULT_ICON_ICO "${CMAKE_SOURCE_DIR}/assets/images/Icon-324x356.ico")
   configure_file (
      "${CMAKE_SOURCE_DIR}/grimvault.rc.in"
      "${CMAKE_BINARY_DIR}/generated/grimvault.rc"
      @ONLY
   )
   target_sources (grimvault PRIVATE "${CMAKE_BINARY_DIR}/generated/grimvault.rc")
endif ()

target_include_directories (grimvault PRIVATE
   "${CMAKE_SOURCE_DIR}/include"
   "${CMAKE_BINARY_DIR}/generated"
)

target_link_libraries (grimvault PRIVATE
   gv::core
   gv::auth
   gv::db
   gv::capture
   gv::vision
   gv::ocr
   gv::api
   gv::cli
   gv::update
   gv::ui
   gv::app
   Qt6::Core
   Qt6::Gui
   Qt6::Widgets
   Shcore
   User32
   Advapi32
   Dbghelp
)

# ---- Translations ----
#
# Build .qm files from translations/*.ts. They're registered as Qt resources
# so QTranslator can load them at runtime by locale code.
# Only run when Qt6LinguistTools is available (vcpkg `qttools` port).
if (GRIMVAULT_HAS_LINGUIST_TOOLS)
   qt_add_translations (grimvault
      TS_FILES
         "${CMAKE_SOURCE_DIR}/translations/grimvault_en.ts"
         "${CMAKE_SOURCE_DIR}/translations/grimvault_de.ts"
         "${CMAKE_SOURCE_DIR}/translations/grimvault_es.ts"
         "${CMAKE_SOURCE_DIR}/translations/grimvault_fr.ts"
         "${CMAKE_SOURCE_DIR}/translations/grimvault_ja.ts"
         "${CMAKE_SOURCE_DIR}/translations/grimvault_ko.ts"
         "${CMAKE_SOURCE_DIR}/translations/grimvault_pt-BR.ts"
         "${CMAKE_SOURCE_DIR}/translations/grimvault_ru.ts"
         "${CMAKE_SOURCE_DIR}/translations/grimvault_zh-Hans.ts"
         "${CMAKE_SOURCE_DIR}/translations/grimvault_zh-Hant.ts"
      RESOURCE_PREFIX "/i18n"
   )
endif ()

set_target_properties (grimvault PROPERTIES
   AUTOMOC ON
)

set_target_properties (grimvault PROPERTIES
   WIN32_EXECUTABLE TRUE
   OUTPUT_NAME      grimvault
)

# vcpkg's applocal.ps1 stages Qt6Core(d).dll and co., but not Qt plugins
# (platforms/, imageformats/, etc.). Without the platforms plugin Qt fails
# to start. Copy them ourselves from vcpkg's installed Qt6/plugins tree.
grimvault_stage_qt_plugins (grimvault)

# Models (tooltip.onnx, paddle/) resolve from the exe dir unless
# GRIMVAULT_DEV_RESOURCES overrides, so stage them next to the binary the
# same way the installed layout does. Without this, running the build
# outside dev-run.ps1 starts with a dead tooltip detector.
add_custom_command (TARGET grimvault POST_BUILD
   COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
           "${CMAKE_SOURCE_DIR}/models"
           "$<TARGET_FILE_DIR:grimvault>/models"
   COMMENT "Staging models for grimvault"
   VERBATIM
)

# Augment page + vendored ddb-tooltips dist, served to WebView2 via the
# grimvault.assets virtual host. Same dev/install duality as models/.
add_custom_command (TARGET grimvault POST_BUILD
   COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
           "${CMAKE_SOURCE_DIR}/web"
           "$<TARGET_FILE_DIR:grimvault>/web"
   COMMENT "Staging web assets for grimvault"
   VERBATIM
)

# ---- Tests ----

if (BUILD_TESTING)
   enable_testing ()
   add_subdirectory (tests)
endif ()

# ---- Install + Package ----

include (cmake/Install.cmake)
