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

   string (TOUPPER "${CMAKE_BUILD_TYPE}" qt_build_type)
   get_target_property (qt_core_location Qt6::Core "IMPORTED_LOCATION_${qt_build_type}")
   if (NOT qt_core_location)
      get_target_property (qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
   endif ()
   if (NOT qt_core_location)
      get_target_property (qt_core_location Qt6::Core IMPORTED_LOCATION)
   endif ()
   if (NOT qt_core_location)
      message (FATAL_ERROR "Cannot resolve the Qt runtime directory")
   endif ()

   get_filename_component (qt_bin_dir "${qt_core_location}" DIRECTORY)
   get_filename_component (qt_prefix "${qt_bin_dir}" DIRECTORY)

   # Official Qt binaries use <prefix>/plugins and <prefix>/qml. vcpkg uses
   # <prefix>/Qt6/plugins and <prefix>/Qt6/qml.
   set (qt_plugins_dir "${qt_prefix}/plugins")
   set (qt_qml_dir "${qt_prefix}/qml")
   if (NOT EXISTS "${qt_plugins_dir}")
      set (qt_plugins_dir "${qt_prefix}/Qt6/plugins")
   endif ()
   if (NOT EXISTS "${qt_qml_dir}")
      set (qt_qml_dir "${qt_prefix}/Qt6/qml")
   endif ()

   add_custom_command (TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_directory
              "${qt_plugins_dir}"
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
              "${qt_qml_dir}"
              "$<TARGET_FILE_DIR:${target}>"
      COMMENT "Staging Qt QML modules for ${target}"
      VERBATIM
      COMMAND_EXPAND_LISTS
   )

   # vcpkg's applocal deploy copies only the Qt DLLs the exe links directly.
   # QML plugins pull in extra Qt libraries (e.g. QtQuick/Layouts/*.dll needs
   # Qt6QuickLayouts.dll) that nothing links at build time, so the plugin
   # fails to load at runtime with "module could not be found". Stage every
   # Qt6Quick*.dll so any staged QML module finds its runtime.
   file (GLOB qml_runtime "${qt_bin_dir}/Qt6Quick*.dll")

   if (qml_runtime)
      add_custom_command (TARGET ${target} POST_BUILD
         COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                 ${qml_runtime}
                 "$<TARGET_FILE_DIR:${target}>"
         COMMENT "Staging Qt QML runtime DLLs for ${target}"
         VERBATIM
         COMMAND_EXPAND_LISTS
      )
   endif ()
endfunction ()

# Keep the old name as an alias for any callers still using it.
function (grimvault_windeployqt target)
   grimvault_stage_qt_plugins (${target})
endfunction ()
