import { execSync } from 'node:child_process';
import { readdir, copyFile, writeFileSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';

const DEBUG = process.env.NODE_ENV === 'development';
const BINDINGS = "binding.gyp";

const binding = {
  "targets": [
    {
      "target_name": "native",
      "product_dir": "<(module_root_dir)/src/native/.build",
      "sources": [ 
        "src/native/async.cpp",
        "src/native/logger.cpp",
        "src/native/main.cpp",
        "src/native/screen.cpp",
        "src/native/util.cpp"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').targets\"):node_addon_api"
      ],
      "include_dirs": [
        "<(module_root_dir)/node_modules/node-addon-api",
        "<(module_root_dir)/vcpkg_installed/x64-windows/include",
        "<(module_root_dir)/vcpkg_installed/x64-windows/include/opencv4"
      ],
      "configurations": {},
      "msvs_settings": {
        "VCCLCompilerTool": {
          "AdditionalOptions": [
            "/Zc:__cplusplus",
            "/bigobj",
            "/await",
            "/EHsc",
          ]
        }
      },
      "defines": [
        "WINRT_LEAN_AND_MEAN",
        "NOMINMAX",
        "WIN32_LEAN_AND_MEAN"
      ]
    }
  ]
}

if (DEBUG) {
  binding.targets [0].libraries = [
      "d3d11.lib",
      "d3dcompiler.lib",
      "DirectXTex.lib",
      "DirectXTK.lib",
      "dxgi.lib",
      "dxguid.lib",
      "leptonica-1.84.1d.lib",
      "opencv_core4d.lib",
      "opencv_dnn4d.lib",
      "opencv_highgui4d.lib",
      "opencv_imgcodecs4d.lib",
      "opencv_imgproc4d.lib",
      "opencv_photo4d.lib",
      "Shcore.lib",
      "tesseract55d.lib",
      "windowsapp.lib"
  ];

  binding.targets [0].configurations.Debug = {
    "msvs_settings": {
      "VCCLCompilerTool": {
      "ExceptionHandling": 1,
      "AdditionalOptions": [
        "/EHsc",
        "/Zi",
        "/Zc:__cplusplus",
        "/bigobj",
        "/await",
      ],
      "Optimization": 0,
      "DebugInformationFormat": 3,
      "RuntimeLibrary": 3
    },
    "VCLinkerTool": {
      "GenerateDebugInformation": "true",
        "AdditionalLibraryDirectories": [
          "<(module_root_dir)/vcpkg_installed/x64-windows/debug/lib"
        ]
      }
    }
  };
} else {
  binding.targets [0].libraries = [
    "d3d11.lib",
    "d3dcompiler.lib",
    "DirectXTex.lib",
    "DirectXTK.lib",
    "dxgi.lib",
    "dxguid.lib",
    "leptonica-1.84.1.lib",
    "opencv_core4.lib",
    "opencv_dnn4.lib",
    "opencv_highgui4.lib",
    "opencv_imgcodecs4.lib",
    "opencv_imgproc4.lib",
    "opencv_photo4.lib",
    "Shcore.lib",
    "tesseract55.lib",
    "windowsapp.lib"
  ];

  binding.targets [0].configurations.Release = {
    "msvs_settings": {
      "VCCLCompilerTool": {
        "ExceptionHandling": 1,
        "AdditionalOptions": [
          "/EHsc",
          "/Zi",
          "/Zc:__cplusplus",
          "/bigobj",
          "/await",
        ],
        "Optimization": 3,
        "FavorSizeOrSpeed": 1,
        "InlineFunctionExpansion": 2,
        "WholeProgramOptimization": "true",
        "StringPooling": "true",
        "EnableFunctionLevelLinking": "true",
        "EnableEnhancedInstructionSet": 2,
        "DebugInformationFormat": 3,
        "RuntimeLibrary": 2
      },
      "VCLinkerTool": {
        "LinkTimeCodeGeneration": 1,
        "GenerateDebugInformation": "true",
        "AdditionalLibraryDirectories": [
          "<(module_root_dir)/vcpkg_installed/x64-windows/lib"
        ]
      }
    }
  };
}

writeFileSync (
  BINDINGS,
  JSON.stringify (binding, null, 2)
);

try {
  const command = DEBUG ? "NODE_ENV=development node-gyp rebuild --verbose" : "node-gyp rebuild --verbose";
  execSync (command, { stdio: "inherit" });
} catch (error) {
  console.error ("Build failed: ", error);
  process.exit (1);
} finally {
  unlinkSync (BINDINGS);
}

// Copy DLLs

let source;
let destination;

if (DEBUG) {
  source = "vcpkg_installed/x64-windows/debug/bin";
  destination = "src/native/.build";
} else {
  source = "vcpkg_installed/x64-windows/bin";
  destination = "src/native/.build";
}

readdir (source, (err, files) => {
  if (err) {
    console.error ("Error reading source directory: ", err);
    return;
  }

  const dllFiles = files.filter ((file) => file.endsWith (".dll"));

  dllFiles.forEach ((file) => {
    const from = join (source, file);
    const to = join (destination, file);

    console.log ("Copying DLL: ", file);

    copyFile (from, to, (err) => {
      if (err) {
        console.error (`Error copying ${file}: `, err);
      }
    });
  });
});

execSync ('rm -rf ./build');