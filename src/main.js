// import { app, BrowserWindow, ipcMain, screen } from 'electron';
import electron, { dialog, globalShortcut, Menu, shell, Tray } from 'electron';
const { app, BrowserWindow, ipcMain, screen } = electron;

import updater from 'electron-updater';
const { autoUpdater } = updater;

import { createRequire } from 'node:module';
import { basename, join } from 'node:path';
import { api } from './api.js';
import { logger, logPath, transport } from './logger.js';
import { bringToFront, isDebug, logSystemInformation, updateOverlayPosition, uploadLog } from './util.js';
import { checkAndInstallVCRedist } from './vcredist.js';
import { ROOT, SOURCE } from './config.js';

import { settings, settingsPath } from './settings.js';

logger.info ('Loaded settings: ', settings);

const require = createRequire (import.meta.url);

// -- -- //

if (process.resourcesPath) {
  process.env.PATH = `${join(process.resourcesPath, 'dlls')};${process.env.PATH}`;
} else {
  // %APPDATA%/../Local/Programs/grimvault/resources/app.asar/src
  process.env.PATH = `${join(SOURCE, '..', '..', 'dlls')};${process.env.PATH}`;
}

process.on ('uncaughtException', (error) => {
  logger.error ('Uncaught Exception:', error);
});

process.on ('unhandledRejection', (reason, promise) => {
  logger.error (`Unhandled Promise Rejection: ${reason.toString ()}`);
});

process.on ('SIGTERM', () => {
  logger.info ('Received SIGTERM. Performing graceful shutdown');
  process.exit (0);
});

process.on ('SIGINT', () => {
  logger.info ('Received SIGINT. Performing graceful shutdown');
  process.exit (0);
});

process.on ('warning', (warning) => {
  logger.warn ('Node warning:', warning);
});

process.on ('exit', () => {
  logger.info ('Process exiting');
});

// if (isDebug ()) {
//   try {
//     const module = {
//       filename: fileURLToPath (import.meta.url),
//       children: [],
//     };

//     await import ('electron-reloader').then (reloader => {
//       reloader.default (module, {
//         watchDir: [
//           join (ROOT, 'src') + '/**/*'
//         ],

//         ignore: [
//           join (ROOT, 'src/native')
//         ]
//       });
//     }).catch (error => {
//       logger.error (`Electron hot reload error: ${error}`);
//     });
//   } catch (error) {
//     logger.error (`Electron hot reload error: ${error}`);
//   }
// }

autoUpdater.setFeedURL ({
  provider: 's3',
  bucket: 'darkerdb.com',
  path: 'GrimVault',
  region: 'us-west-2'
});

let overlay;
// let previousMonitorId;

// async function updateOverlayPosition () {
//   let window = await activeWindow ();

//   if (!window?.owner?.path) {
//     overlay.hide ();
//     return;
//   }

//   let activeWindowPath = basename (window.owner.path);

//   if (activeWindowPath === 'DungeonCrawler.exe' ||
//       activeWindowPath === 'GrimVault.exe' ||
//       activeWindowPath === 'electron.exe') {
//     overlay.show ();
//   } else {
//     overlay.hide ();
//   }

//   if (activeWindowPath === "DungeonCrawler.exe") {
//     const gameBounds = window.bounds;

//     const displays = screen.getAllDisplays ();

//     const currentMonitor = displays.find ((display) => {
//       const { x, y, width, height } = display.bounds;

//       return (
//         gameBounds.x >= x &&
//         gameBounds.x < x + width &&
//         gameBounds.y >= y &&
//         gameBounds.y < y + height
//       );
//     });

//     if (currentMonitor && currentMonitor.id !== previousMonitorId) {
//       overlay.setBounds ({
//         x: currentMonitor.bounds.x,
//         y: currentMonitor.bounds.y,
//         width: currentMonitor.bounds.width,
//         height: currentMonitor.bounds.height,
//       });

//       overlay.setIgnoreMouseEvents (true, { forward: true });

//       logger.info (`Moved overlay to monitor: ${currentMonitor.id}`);
//       previousMonitorId = currentMonitor.id;
//     }
//   }
// }

app.commandLine.appendSwitch ('high-dpi-support', 1);
app.commandLine.appendSwitch ('force-device-scale-factor', 1);
app.commandLine.appendSwitch ('enable-hardware-acceleration');
app.commandLine.appendSwitch ('force-node-api-uncaught-exceptions-policy', 'true');

if (settings.general.launch_on_startup) {
  logger.info ('Registering app startup on login');

  app.setLoginItemSettings ({
    openAtLogin: true,
    path: process.execPath,
    args: [
      '--processStart',
      `${basename (process.execPath)}`,
      '--process-start-args',
      "--hidden"
    ]
  });
} else {
  logger.info ('Deregistering app startup on login');

  app.setLoginItemSettings ({ 
    openAtLogin: false
  });
}

let lock = app.requestSingleInstanceLock ();

if (!lock) {
  app.quit ();
}

app.on ('second-instance', (event, argv, cwd) => {
  logger.info ('Prevented second instance from spawning');
});

app.on ('render-process-gone', (event, webContents, details) => {
  logger.error (`Render process crashed: ${details}`);
});

app.on ('child-process-gone', (event, details) => {
  logger.error (`Child process crashed: ${details}`);
});

app.on ('ready', async () => {
  logSystemInformation ();

  autoUpdater.checkForUpdates ();

  // Update event handlers
  autoUpdater.on ('checking-for-update', () => {
    logger.info ('Checking for updates');
  });

  autoUpdater.on ('update-available', (info) => {
    logger.info ('Update available:', info);
  });

  autoUpdater.on ('update-not-available', (info) => {});

  autoUpdater.on ('download-progress', (progressObj) => {
    logger.info (`Download speed: ${progressObj.bytesPerSecond}`);
    logger.info (`Downloaded ${progressObj.percent}%`);
  });

  autoUpdater.on ('update-downloaded', (info) => {
    logger.info ('Installing update');
    autoUpdater.quitAndInstall ();
  });

  autoUpdater.on ('error', (error) => {
    logger.info (`Auto update error: ${error}`);
  });

  logger.info ('Setting up system tray');

  let tray;
  let menu;

  menu = Menu.buildFromTemplate ([
    {
      label: 'Version',
      type: 'normal',
      click: () => {
        dialog.showMessageBox (
          null,
          {
            title: 'GrimVault',
            message: `GrimVault ${app.getVersion ()} (${app.getLocale ()})`
          }
        );
      }
    },
    {
      label: 'Logs',
      click: () => {
        shell.openPath (logPath);
      }
    },

    {
      label: 'Settings',
      click: () => {
        shell.openPath (settingsPath);
      }
    },

    // {
    //   label: 'Upload Logs',
    //   click: () => {
    //     uploadLogs ()

    //     dialog.showMessageBox (
    //       null,
    //       {
    //         title: 'GrimVault',
    //         message: 'Your GrimVault logs are being sent to our development team. Thank you for your help!'
    //       }
    //     );
    //   }
    // }
    {
      label: 'Check for Updates',
      type: 'normal',
      click: () => {
        autoUpdater.checkForUpdates ();
      }
    },
    {
      label: 'Exit',
      type: 'normal',
      click: () => {
        app.quit ();
      }
    }
  ]);
  
  tray = new Tray (join (ROOT, 'assets/images/Icon-81x89.png'));

  tray.setToolTip ('GrimVault');
  tray.setContextMenu (menu);

  tray.on ('click', (event, bounds, position) => {
  });

  logger.info ('Creating overlay window');

  overlay = new BrowserWindow ({
    backgroundColor: '#00000000',
    show: false,
    frame: false,
    transparent: true,
    alwaysOnTop: true,
    skipTaskbar: true,
    paintWhenInitiallyHidden: true,
    webPreferences: {
      preload: join (SOURCE, 'preload.cjs'),
      nodeIntegration: false,
      contextIsolation: true,
      sandbox: false,
      offscreen: false
    },
  });

  overlay.maximize ();

  const vcredistInstalled = await checkAndInstallVCRedist (overlay);
  
  if (!vcredistInstalled) {
    logger.error ('Required Visual C++ Redistributable is not installed');
    app.quit ();

    return;
  } else {
    logger.info ('Visual C++ Redistributable installed successfully or already installed');
  }

  logger.info ('Loading native screen module');

  const { 
    initialize, 
    getTooltip, 
    getWindow,
    // startWindowEventListener, 
    // stopWindowEventListener 
  } = require (join (SOURCE, 'native/.build/native.node'));

  let tesseractPath;
  let onnxFile;

  if (app.isPackaged) {
    tesseractPath = join (ROOT, '../models');
    onnxFile = join (ROOT, '../models/tooltip.onnx');
  } else {
    tesseractPath = join (ROOT, '../models/tesseract');
    onnxFile = join (ROOT, '../models/vision/runs/detect/train/weights/best.onnx');
  }

  let onMessage = (level, message) => {
    logger [level] (`[Native] ${message}`);
  }

  logger.info (`Initializing native screen module with mode: ${settings.general.capture_method}`);

  if (!initialize (tesseractPath, onnxFile, onMessage, settings.general.capture_method)) {
    logger.error ('Failed to initialize native screen module');
    app.quit ();

    return;
  }

  bringToFront (overlay);

  let previous;
  let isGameOpen = false;

  setInterval (() => {
    let window = getWindow ("DungeonCrawler.exe");

    if (window) {
      isGameOpen = true;

      if (window.x !== previous?.x ||
          window.y !== previous?.y ||
          window.width !== previous?.width ||
          window.height !== previous?.height) {
        logger.info ('Updating overlay position (and setting opacity to 1.0): ', window);
        overlay.setOpacity (1);
        updateOverlayPosition (overlay, window);
      }
      
      previous = window;
    } else {
      logger.debug ("Game window not found (setting opacity to 0.0)");
      overlay.setOpacity (0.0);
      isGameOpen = false;
    }
  }, 1000);

  // startWindowEventListener ((window, bounds) => {
  //   try {
  //     updateOverlayPosition (overlay, window, bounds);
  //   } catch (error) {
  //     logger.error (`Failed to update overlay position: ${error}`);
  //   }
  // });

  app.on ('before-quit', () => {
    globalShortcut.unregisterAll ();
    // stopWindowEventListener ();
  });

  // Pass all mouse events through
  overlay.setIgnoreMouseEvents (true, { forward: true });

  overlay.on ('focus', () => {
    overlay.setIgnoreMouseEvents (true, { forward: true });
  });

  if (app.isPackaged) {
    overlay.loadFile (join (ROOT, 'ui/overlay/dist/index.html'));
  } else {
    overlay.loadURL ('http://localhost:5173');
    
    overlay.webContents.openDevTools ({
      mode: 'detach'
    });
  }

  if (settings.general.auto_updates) {
    logger.info ('Checking for updates every hour');

    setInterval (() => {
      autoUpdater.checkForUpdates ();
    }, 60 * 60 * 1000);
  } else {
    logger.info ('Auto updates are disabled');
  }

  overlay.webContents.send ('settings', settings);

  globalShortcut.register (settings.hotkeys.toggle_mode, () => {
    overlay.webContents.send ('toggleMode');
  });
  
  globalShortcut.register (settings.hotkeys.run_price_check, () => {
    overlay.webContents.send ('manual:checkForTooltips');
  });

  ipcMain.on ('log', (event, data) => {
    const { level, message, meta = {} } = data;
    logger.log (level, `[Renderer] ${message}`, meta);
  });

  ipcMain.on ('checkForTooltips', async () => {
    if (!isGameOpen) {
      logger.debug ('Tried to check for tooltips but the game is not open');
      return;
    }

    overlay.webContents.send ('checkForTooltips:start');

    let tooltip; 
    
    try {
      tooltip = await getTooltip ();
    } catch (e) {
      logger.error (`Error getting tooltip: ${e}`);
    }

    if (!tooltip) {
      overlay.webContents.send ('checkForTooltips:finish');
      return;
    }

    // isEnabled = false;

    let stats = await getItemStats (tooltip.text);

    if (stats) {
      overlay.webContents.send ('hover:item', {
        ... tooltip,
        ... stats  
      });
    }

    overlay.webContents.send ('checkForTooltips:finish');
  });
});

transport.on ('rotate', async (oldFile, newFile) => {
  if (isDebug ()) {
    return;
  }

  uploadLog (oldFile);
});

async function getItemStats (tooltipText) {
  try {
    let response = await api.get ('/v1/price-check', {
      params: {
        tooltip: tooltipText
      }
    });

    if (!response) {
      return false;
    }

    return response.data.body;
  } catch (e) {
    logger.error (e);
    return false;
  }
}