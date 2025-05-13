import electron from 'electron';
import { join } from 'node:path';
import { RESOURCES, ROOT, SOURCE } from './config.js';
import { logger } from './logger.js';
import { settings } from './settings.js';
import { createRequire } from 'module';

const { app } = electron;

logger.info ('Loading native screen module');

// %APPDATA%/../Local/Programs/GrimVault/resources/app.asar/src

// GrimVault needs several DLLs to load. 
// The necessary DLLs are added to resources/dlls when the app is packaged.
process.env.PATH = `${join (RESOURCES, 'dlls')};${process.env.PATH}`;

let nativeModulePath;

if (app.isPackaged) {
  nativeModulePath = join (RESOURCES, 'native.node');
} else {
  nativeModulePath = join (SOURCE, 'native', '.build', 'native.node');
}

let native = createRequire (import.meta.url) (nativeModulePath);

let tesseractModelPath;
let onnxModelPath;

if (app.isPackaged) {
  tesseractModelPath = join (ROOT, '..', 'models');
  onnxModelPath = join (ROOT, '..', 'models', 'tooltip.onnx');  
} else {
  tesseractModelPath = join (ROOT, 'models', 'tesseract');
  onnxModelPath = join (ROOT, 'models', 'vision', 'runs', 'detect', 'train', 'weights', 'best.onnx');
}

let onMessageCallback = (level, message) => {
  logger [level] (`[Native] ${message}`);
};

logger.info (`Initializing native screen module with mode: ${settings.general.capture_method}`);

let success = native.initialize (
  tesseractModelPath,
  onnxModelPath,
  onMessageCallback,
  settings.general.capture_method
);

if (!success) {
  logger.error ('Failed to initialize native screen module');
  app.quit ();
}

let {
  getTooltip
} = native;

export {
  getTooltip, 
};