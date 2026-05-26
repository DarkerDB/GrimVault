# grimvault_add_module (<name>)
#
# Adds src/<name>/ as a static library target `gv_<name>` aliased to gv::<name>.
# Public headers live under include/gv/<name>/. Source files are collected from
# src/<name>/*.cpp at configure time.
#
# Usage:
#    grimvault_add_module (core)
#    target_link_libraries (gv_core PUBLIC fmt::fmt spdlog::spdlog)
#
function (grimvault_add_module name)
   set (module_src_dir "${CMAKE_SOURCE_DIR}/src/${name}")
   set (module_inc_dir "${CMAKE_SOURCE_DIR}/include/gv/${name}")

   file (GLOB_RECURSE module_sources CONFIGURE_DEPENDS
      "${module_src_dir}/*.cpp"
      "${module_src_dir}/*.h"
   )

   # Include the module's public headers in the target source list so AUTOMOC
   # scans them for Q_OBJECT classes. Without this, headers reached only via
   # <gv/...> include directives don't get moc'd.
   file (GLOB_RECURSE module_public_headers CONFIGURE_DEPENDS
      "${module_inc_dir}/*.h"
   )

   if (NOT module_sources)
      message (FATAL_ERROR "grimvault_add_module(${name}): no sources under ${module_src_dir}")
   endif ()

   add_library (gv_${name} STATIC ${module_sources} ${module_public_headers})
   add_library (gv::${name} ALIAS gv_${name})

   target_include_directories (gv_${name}
      PUBLIC
         "${CMAKE_SOURCE_DIR}/include"
         "${CMAKE_BINARY_DIR}/generated"
      PRIVATE
         "${module_src_dir}"
   )

   set_target_properties (gv_${name} PROPERTIES
      CXX_STANDARD 23
      CXX_STANDARD_REQUIRED ON
      POSITION_INDEPENDENT_CODE ON
   )
endfunction ()


# grimvault_windeployqt (<target>)
#
# Runs windeployqt after the target is built so Qt DLLs and plugins land
# alongside the executable. Only meaningful on Windows; no-op elsewhere.
#
function (grimvault_stage_qt_plugins target)
   if (NOT WIN32)
      return ()
   endif ()

   # vcpkg installs Qt plugins under:
   #    <prefix>/Qt6/plugins        (release)
   #    <prefix>/debug/Qt6/plugins  (debug)
   # At runtime Qt looks for them next to the executable in
   #    <exe_dir>/<category>/<plugin>.dll  (e.g. platforms/qwindowsd.dll)
   # so we copy the matching plugins tree post-build. windeployqt itself
   # trips on vcpkg's split bin/debug-bin layout, so we do this directly.
   set (vcpkg_root "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows")
   if (DEFINED VCPKG_INSTALLED_DIR)
      set (vcpkg_root "${VCPKG_INSTALLED_DIR}/x64-windows")
   endif ()

   add_custom_command (TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_directory
              "$<IF:$<CONFIG:Debug>,${vcpkg_root}/debug/Qt6/plugins,${vcpkg_root}/Qt6/plugins>"
              "$<TARGET_FILE_DIR:${target}>"
      COMMENT "Staging Qt plugins for ${target}"
      VERBATIM
      COMMAND_EXPAND_LISTS
   )

   # QML modules live under Qt6/qml/<Module>/{qmldir,*.qml,plugin.dll}. Qt's
   # engine looks for them next to the exe by default. plugins-staging above
   # only handles the C++ plugin tree; QML modules are a separate copy.
   # Without this, `import QtQuick.Window` (and any required-plugin module)
   # fails at runtime with "module not installed".
   add_custom_command (TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_directory
              "$<IF:$<CONFIG:Debug>,${vcpkg_root}/debug/Qt6/qml,${vcpkg_root}/Qt6/qml>"
              "$<TARGET_FILE_DIR:${target}>"
      COMMENT "Staging Qt QML modules for ${target}"
      VERBATIM
      COMMAND_EXPAND_LISTS
   )
endfunction ()

# Keep the old name as an alias for any callers still using it.
function (grimvault_windeployqt target)
   grimvault_stage_qt_plugins (${target})
endfunction ()
