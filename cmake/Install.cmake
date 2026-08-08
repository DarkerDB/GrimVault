# Install + CPack configuration for the NSIS Windows installer.
#
# Layout in the installed tree:
#    <prefix>/                       (default: %LOCALAPPDATA%\Programs\GrimVault)
#    ├── grimvault.exe
#    ├── *.dll                       (Qt + vcpkg runtime DLLs, via deploy script)
#    ├── plugins/                    (Qt platforms, imageformats, etc.)
#    ├── qml/                        (compiled QML modules)
#    ├── models/                     (paddle/<family>/, tooltip.onnx)
#    ├── i18n/                       (en/ de/ es/ fr/ ja/ ko/ pt-BR/ ru/ zh-Hans/ zh-Hant)
#    ├── assets/                     (fonts, images)
#    ├── db/schema.sql               (reference; runtime schema lives in user dir)
#    └── LICENSE
#
# Per-user data goes in %APPDATA%\GrimVault\ (db, logs, settings.ini.migrated).

# ---- Install the executable ----

install (TARGETS grimvault
   RUNTIME DESTINATION .
   COMPONENT             application
)

# Qt runtime: generated install-time script invokes windeployqt with the
# correct flags so platform plugins, QML modules, and Qt DLLs land alongside
# the binary. Requires Qt 6.3+. NO_UNSUPPORTED_PLATFORM_ERROR keeps the
# configure step quiet on non-Windows hosts (where this whole tree is unused).
qt_generate_deploy_app_script (
   TARGET                       grimvault
   OUTPUT_SCRIPT                qt_deploy_script
   NO_UNSUPPORTED_PLATFORM_ERROR
   DEPLOY_TOOL_OPTIONS
      --no-translations
      --no-system-d3d-compiler
      --no-opengl-sw
      --qmldir "${CMAKE_SOURCE_DIR}/qml"
)

install (SCRIPT  "${qt_deploy_script}"  COMPONENT application)

# Non-Qt runtime DLLs (OpenCV, ONNX, libcurl, etc.) discovered from imported
# targets. TARGET_RUNTIME_DLLS resolves at install time.
install (FILES       "$<TARGET_RUNTIME_DLLS:grimvault>"
         DESTINATION .
         COMPONENT   application)

# ---- Bundled data ----

install (DIRECTORY  "${CMAKE_SOURCE_DIR}/models/"
         DESTINATION models
         COMPONENT   models
         FILES_MATCHING
            PATTERN "*.onnx"
            PATTERN "*.txt"
            PATTERN "*.json")

install (DIRECTORY  "${CMAKE_SOURCE_DIR}/i18n/"
         DESTINATION i18n
         COMPONENT   i18n
         FILES_MATCHING
            PATTERN "*.json"
            PATTERN "*.locres")

install (DIRECTORY  "${CMAKE_SOURCE_DIR}/assets/"
         DESTINATION assets
         COMPONENT   assets)

install (DIRECTORY  "${CMAKE_SOURCE_DIR}/db/"
         DESTINATION db
         COMPONENT   schema
         FILES_MATCHING
            PATTERN "*.sql")

# Augment page + vendored ddb-tooltips dist (WebView2 virtual-host root).
install (DIRECTORY  "${CMAKE_SOURCE_DIR}/web/"
         DESTINATION web
         COMPONENT   assets)

install (FILES      "${CMAKE_SOURCE_DIR}/LICENSE"
         DESTINATION .
         COMPONENT   application)

# ---- CPack: NSIS Windows installer ----

set (CPACK_PACKAGE_NAME           "GrimVault")
set (CPACK_PACKAGE_VENDOR         "DarkerDB")
set (CPACK_PACKAGE_VERSION        "${PROJECT_VERSION}")
set (CPACK_PACKAGE_VERSION_MAJOR  "${PROJECT_VERSION_MAJOR}")
set (CPACK_PACKAGE_VERSION_MINOR  "${PROJECT_VERSION_MINOR}")
set (CPACK_PACKAGE_VERSION_PATCH  "${PROJECT_VERSION_PATCH}")
# Env-aware installer filename so dev / qa / prod matrix builds don't collide
# on `gh release upload --clobber`. Each arm produces a uniquely-named .exe.
set (CPACK_PACKAGE_FILE_NAME      "GrimVault-Setup-${GRIMVAULT_ENV}-${PROJECT_VERSION}")
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "GrimVault native client")
set (CPACK_PACKAGE_INSTALL_DIRECTORY   "GrimVault")
set (CPACK_RESOURCE_FILE_LICENSE       "${CMAKE_SOURCE_DIR}/LICENSE")

set (CPACK_GENERATOR  "NSIS")

set (CPACK_NSIS_PACKAGE_NAME              "GrimVault")
set (CPACK_NSIS_DISPLAY_NAME              "GrimVault")
set (CPACK_NSIS_HELP_LINK                 "https://darkerdb.com")
set (CPACK_NSIS_URL_INFO_ABOUT            "https://darkerdb.com")
set (CPACK_NSIS_CONTACT                   "support@darkerdb.com")
set (CPACK_NSIS_MODIFY_PATH               OFF)
set (CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set (CPACK_NSIS_MUI_FINISHPAGE_RUN        "grimvault.exe")

# Per-user install: no UAC prompt, no admin required, no SmartScreen
# "publisher unknown" gauntlet on every install. Matches app.manifest's
# asInvoker. The install location moves from C:\Program Files\GrimVault
# to %LOCALAPPDATA%\Programs\GrimVault — same convention used by VS Code,
# Discord, GitHub Desktop, etc.
set (CPACK_NSIS_REQUEST_EXECUTION_LEVEL   user)
set (CPACK_NSIS_INSTALL_ROOT              "$LOCALAPPDATA\\Programs")

# Start Menu + uninstaller shortcuts.
set (CPACK_PACKAGE_EXECUTABLES            "grimvault" "GrimVault")
set (CPACK_CREATE_DESKTOP_LINKS           "grimvault")

# WebView2 Evergreen runtime: preinstalled on Win11 and pushed to Win10 via
# Windows Update, but not guaranteed. Bundle the ~2 MB bootstrapper
# (https://go.microsoft.com/fwlink/p/?LinkId=2124703 →
# tools/build/MicrosoftEdgeWebView2Setup.exe, not committed) and run it
# silently when the EdgeUpdate client key is absent. Per-user install, so
# no UAC. Without the bootstrapper file the installer still builds; the app
# then falls back to the QML renderer on machines missing the runtime.
set (grimvault_wv2_bootstrapper "${CMAKE_SOURCE_DIR}/tools/build/MicrosoftEdgeWebView2Setup.exe")

if (EXISTS "${grimvault_wv2_bootstrapper}")
   string (REPLACE "/" "\\\\" grimvault_wv2_bootstrapper_nsis "${grimvault_wv2_bootstrapper}")
   set (CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
      ClearErrors
      ReadRegStr $0 HKLM 'SOFTWARE\\\\WOW6432Node\\\\Microsoft\\\\EdgeUpdate\\\\Clients\\\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}' 'pv'
      IfErrors 0 wv2_present
      ReadRegStr $0 HKCU 'Software\\\\Microsoft\\\\EdgeUpdate\\\\Clients\\\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}' 'pv'
      IfErrors 0 wv2_present
      DetailPrint 'Installing WebView2 runtime...'
      File '/oname=$TEMP\\\\MicrosoftEdgeWebView2Setup.exe' '${grimvault_wv2_bootstrapper_nsis}'
      ExecWait '\"$TEMP\\\\MicrosoftEdgeWebView2Setup.exe\" /silent /install'
      Delete '$TEMP\\\\MicrosoftEdgeWebView2Setup.exe'
      wv2_present:
   ")
else ()
   message (STATUS "WebView2 bootstrapper not present; installer will skip runtime check")
endif ()

# Components: bundle as a single .exe (not split-component install). Users
# never need to pick which paddle model family to install; we ship them all.
set (CPACK_COMPONENTS_ALL                 application models i18n assets schema)
set (CPACK_NSIS_MUI_HEADERIMAGE           "${CMAKE_SOURCE_DIR}/assets/images/Icon-324x356.png")

# Generate a multi-resolution .ico from the source PNG at configure time
# if it doesn't exist yet. Requires Python3 + Pillow; skips silently if
# either is unavailable (the EXISTS check below then leaves NSIS with its
# default icon, and a warning surfaces the gap).
set (grimvault_icon_png "${CMAKE_SOURCE_DIR}/assets/images/Icon-324x356.png")
set (grimvault_icon_ico "${CMAKE_SOURCE_DIR}/assets/images/Icon-324x356.ico")

if (NOT EXISTS "${grimvault_icon_ico}" AND EXISTS "${grimvault_icon_png}")
   find_package (Python3 QUIET COMPONENTS Interpreter)
   if (Python3_Interpreter_FOUND)
      execute_process (
         COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/build/png_to_ico.py"
            "${grimvault_icon_png}"
            "${grimvault_icon_ico}"
         RESULT_VARIABLE ico_rc
         OUTPUT_VARIABLE ico_out
         ERROR_VARIABLE  ico_err
      )
      if (ico_rc EQUAL 0)
         message (STATUS "Generated installer icon: ${grimvault_icon_ico}")
      else ()
         message (WARNING
            "png_to_ico.py failed (rc=${ico_rc}); installer will use default icon. "
            "Install Pillow (`pip install pillow`) and reconfigure to generate one. "
            "stderr: ${ico_err}")
      endif ()
   else ()
      message (WARNING
         "Python3 not found; cannot auto-generate installer .ico. "
         "Either install Python3+Pillow, or pre-generate ${grimvault_icon_ico}.")
   endif ()
endif ()

if (EXISTS "${grimvault_icon_ico}")
   set (CPACK_NSIS_INSTALLED_ICON_NAME    "grimvault.exe")
   set (CPACK_NSIS_MUI_ICON               "${grimvault_icon_ico}")
   set (CPACK_NSIS_MUI_UNIICON            "${grimvault_icon_ico}")
endif ()

include (CPack)
