include (FetchContent)

FetchContent_Declare (grimvault_onnxruntime
   URL https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.24.4
   URL_HASH SHA256=57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681
   DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare (grimvault_directml
   URL https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.4
   URL_HASH SHA256=4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9
   DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable (grimvault_onnxruntime grimvault_directml)

set (GRIMVAULT_ONNXRUNTIME_RUNTIME
   "${grimvault_onnxruntime_SOURCE_DIR}/runtimes/win-x64/native/onnxruntime.dll")
set (GRIMVAULT_ONNXRUNTIME_PROVIDERS_RUNTIME
   "${grimvault_onnxruntime_SOURCE_DIR}/runtimes/win-x64/native/onnxruntime_providers_shared.dll")
set (GRIMVAULT_DIRECTML_RUNTIME
   "${grimvault_directml_SOURCE_DIR}/bin/x64-win/DirectML.dll")

add_library (onnxruntime::onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties (onnxruntime::onnxruntime PROPERTIES
   IMPORTED_IMPLIB "${grimvault_onnxruntime_SOURCE_DIR}/runtimes/win-x64/native/onnxruntime.lib"
   IMPORTED_LOCATION "${GRIMVAULT_ONNXRUNTIME_RUNTIME}"
   INTERFACE_INCLUDE_DIRECTORIES "${grimvault_onnxruntime_SOURCE_DIR}/build/native/include"
)

add_library (directml::directml SHARED IMPORTED GLOBAL)
set_target_properties (directml::directml PROPERTIES
   IMPORTED_IMPLIB "${grimvault_directml_SOURCE_DIR}/bin/x64-win/DirectML.lib"
   IMPORTED_LOCATION "${GRIMVAULT_DIRECTML_RUNTIME}"
   INTERFACE_INCLUDE_DIRECTORIES "${grimvault_directml_SOURCE_DIR}/include"
)

set (GRIMVAULT_ONNXRUNTIME_LICENSE "${grimvault_onnxruntime_SOURCE_DIR}/LICENSE")
set (GRIMVAULT_DIRECTML_LICENSE "${grimvault_directml_SOURCE_DIR}/LICENSE.txt")
