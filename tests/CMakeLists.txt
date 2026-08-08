find_package (GTest CONFIG REQUIRED)

# Unit tests: small, fast, hermetic. No capture, no Qt event loop, no network.
file (GLOB_RECURSE unit_sources CONFIGURE_DEPENDS "unit/*.cpp")

if (unit_sources)
   add_executable (grimvault_unit_tests ${unit_sources})

   target_link_libraries (grimvault_unit_tests PRIVATE
      gv::core
      gv::db
      gv::capture
      gv::vision
      gv::ocr
      nlohmann_json::nlohmann_json
      # Qt6::Core only — placement_test works on QRect/QSize/QPoint value
      # types. It deliberately does NOT link gv::ui: the point of extracting
      # gv/ui/placement.h was to test the card geometry without a WebView2
      # runtime or an event loop.
      Qt6::Core
      GTest::gtest
      GTest::gtest_main
   )

   target_compile_definitions (grimvault_unit_tests PRIVATE
      GRIMVAULT_TEST_MODELS_DIR="${CMAKE_SOURCE_DIR}/models"
      GRIMVAULT_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
   )

   set_target_properties (grimvault_unit_tests PROPERTIES
      CXX_STANDARD 23
      CXX_STANDARD_REQUIRED ON
   )

   add_test (NAME grimvault_unit_tests COMMAND grimvault_unit_tests)
   set_tests_properties (grimvault_unit_tests PROPERTIES LABELS "unit")
endif ()

# E2E tests: pipeline against PNG fixtures. Built when GRIMVAULT_BUILD_E2E=ON.
# Item canonicalization happens server-side now (see darkerdb_client.lookup_tooltip);
# the e2e tests that exercised the local locres pipeline have been removed.
if (GRIMVAULT_BUILD_E2E)
   file (GLOB_RECURSE e2e_sources CONFIGURE_DEPENDS "e2e/*.cpp")

   if (e2e_sources)
      add_executable (grimvault_e2e_tests ${e2e_sources})

      target_link_libraries (grimvault_e2e_tests PRIVATE
         gv::core
         gv::db
         gv::api
         gv::capture
         gv::vision
         gv::ocr
         opencv_imgcodecs
         opencv_imgproc
         GTest::gtest
         GTest::gtest_main
      )

      target_compile_definitions (grimvault_e2e_tests PRIVATE
         GRIMVAULT_TEST_MODELS_DIR="${CMAKE_SOURCE_DIR}/models"
         GRIMVAULT_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
      )

      set_target_properties (grimvault_e2e_tests PROPERTIES
         CXX_STANDARD 23
         CXX_STANDARD_REQUIRED ON
      )

      add_test (NAME grimvault_e2e_tests COMMAND grimvault_e2e_tests)
      set_tests_properties (grimvault_e2e_tests PROPERTIES LABELS "e2e")
   endif ()
endif ()
