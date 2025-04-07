import electron from 'electron';
const { app } = electron;

import { __dirname } from './config.js';
import merge from 'deepmerge';
import { parse } from 'ini';
import { logger } from './logger.js';
import { existsSync, readFileSync, copyFileSync } from 'node:fs';
import { join } from 'node:path';

const settingsPath = join (app.getPath ('userData'), 'settings.ini');
const defaultsPath = join (__dirname, '..', 'settings.ini');

logger.info (`Loading settings defaults: ${defaultsPath}`);

let settings = {};

if (existsSync (settingsPath)) {
  logger.info (`Loading settings from: ${settingsPath}`);

  let raw = readFileSync (settingsPath).toString ();

  try {
    settings = parse (raw);
  } catch (e) {
    logger.error (`Failed to parse settings from existing settings file: ${settingsPath}: \n ${raw}`);
  }
} else {
  logger.info (`Settings file does not exist: ${settingsPath}`);
  copyFileSync (defaultsPath, settingsPath);
} 

let template = readFileSync (defaultsPath).toString ();
let defaults = parse (template);

settings = merge (defaults, settings);

settings.general.telemetry = toBool (settings.general.telemetry);
settings.general.auto_updates = toBool (settings.general.auto_updates);
settings.general.launch_on_startup = toBool (settings.general.launch_on_startup);
settings.general.capture_method = toEnum (settings.general.capture_method, [ 'wgc', 'd3d', 'gdi' ]);
settings.general.default_mode = toEnum (settings.general.default_mode, [ 'automatic', 'manual', 'disabled' ]);
settings.general.alignment = toEnum (settings.general.alignment, [ 'attached', 'top-left', 'top-right', 'bottom-left', 'bottom-right' ]);

settings.hotkeys.toggle_mode = toHotkey (settings.hotkeys.toggle_mode) || 'Ctrl+F6';
settings.hotkeys.run_price_check = toHotkey (settings.hotkeys.run_price_check) || 'F5';

function toBool (s) {
  if (s === true || s === 'true') return true;
  if (s === false || s === 'false') return false;

  logger.warn (`Invalid boolean setting: ${s}`);
  return true;
}

function toEnum (s, values) {
  if (values.indexOf (s) === -1) {
    logger.warn (`Invalid enumerated setting: ${s}`);
    return values [0];
  }

  return s;
}

function toHotkey (s) {
  if (/^((Ctrl|Alt|Shift)\+)*([A-Za-z0-9]|F[1-9]|F1[0-2])$/.test (s)) {
    return s.replace (/Ctrl/g, 'CommandOrControl');
  }

  // logger.warn (`Invalid hotkey setting: ${s}`);
  return s;
}

export { settings, settingsPath };