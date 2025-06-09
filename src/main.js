// import { app, BrowserWindow, ipcMain, screen } from 'electron';
import electron, { dialog, globalShortcut, Menu, shell, Tray } from 'electron';
import updater from 'electron-updater';
import { basename, join } from 'node:path';
import { logger, logPath } from './logger.js';
import { logSystemInformation } from './util.js';
import { checkAndInstallVCRedist } from './vcredist.js';
import { ROOT, SOURCE, isDebug } from './config.js';
import { settings, settingsPath } from './settings.js';
import { pin } from './pin.js';
import { wire } from './frontend.js';

const { app, BrowserWindow } = electron;
const { autoUpdater } = updater;

let debugging = false;

logger.info ('Loaded settings: ', settings);

process.on ('uncaughtException', (error) => {
  logger.error ('Uncaught Exception:', error);
  logger.error (`Stack trace: ${error.stack}`);
});

process.on ('unhandledRejection', (reason, promise) => {
  logger.error(`Unhandled Promise Rejection: ${reason.toString()}`);
  logger.error(`Stack trace: ${reason.stack}`);
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

app.commandLine.appendSwitch ('high-dpi-support', 1);
app.commandLine.appendSwitch ('force-device-scale-factor', 1);
app.commandLine.appendSwitch ('disable-crash-reporter');

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

if (!app.requestSingleInstanceLock ()) {
  app.quit ();
}

app.on ('second-instance', (event, argv, cwd) => {
  logger.info ('Prevented second instance from spawning');
});

app.on ('render-process-gone', (event, webContents, details) => {
  logger.error (`Render process crashed: ${JSON.stringify (details)}`);
});

app.on ('child-process-gone', (event, details) => {
  logger.error (`Child process crashed: ${JSON.stringify (details)}`);
});

app.on ('before-quit', () => {
  globalShortcut.unregisterAll ();
});

app.on ('ready', async () => {
  logSystemInformation ();

  if (settings.general.auto_updates) {
    autoUpdater.setFeedURL ({
      provider: 's3',
      bucket: 'darkerdb.com',
      path: 'GrimVault',
      region: 'us-west-2'
    });

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
  }

  logger.info ('Setting up system tray');

  let menu = Menu.buildFromTemplate ([
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
  
  let tray = new Tray (join (ROOT, 'assets/images/Icon-81x89.png'));

  tray.setToolTip ('GrimVault');
  tray.setContextMenu (menu);

  logger.info ('Creating overlay window');

  let overlay = new BrowserWindow ({
    backgroundColor: '#00000000',
    // backgroundColor: '#ff000000',
    show: false,
    frame: false,
    transparent: true,
    alwaysOnTop: true,
    skipTaskbar: true,
    type: 'toolbar',
    webPreferences: {
      preload: join (SOURCE, 'preload.cjs'),
      sandbox: false
    },
  });

  overlay.webContents.setZoomFactor (1);

  logger.info ('Registering overlay pin');

  setInterval (() => {
    pin (overlay, debugging);
  }, 2500);

  wire (overlay);

  if (settings.general.auto_updates) {
    logger.info ('Checking for updates every hour');

    setInterval (() => {
      autoUpdater.checkForUpdates ();
    }, 60 * 60 * 1000);
  } else {
    logger.info ('Auto updates are disabled');
  }

  const vcredistInstalled = await checkAndInstallVCRedist (overlay);
  
  if (!vcredistInstalled) {
    logger.error ('Required Visual C++ Redistributable is not installed');
    app.quit ();

    return;
  } else {
    logger.info ('Visual C++ Redistributable installed successfully or already installed');
  }

  if (app.isPackaged) {
    overlay.loadFile (join (ROOT, 'ui', 'overlay', 'dist', 'index.html'));
  } else {
    overlay.loadURL ('http://localhost:5173');
  }

  globalShortcut.register (settings.hotkeys.toggle_mode, () => {
    logger.info ('Toggling mode');
    overlay.webContents.send ('manual:toggle');
  });
  
  globalShortcut.register (settings.hotkeys.run_price_check, () => {
    logger.info ('Running manual price check');
    overlay.webContents.send ('manual:scan');
  });

  if (isDebug ()) {
    overlay.webContents.openDevTools ({
      mode: 'detach'
    });
  }

  globalShortcut.register ('F7', () => {
    logger.info ('Toggling debugger');
    overlay.webContents.send ('manual:debugger');

    debugging = !debugging;

    if (debugging) {
      overlay.webContents.openDevTools ({
        mode: 'detach'
      });
    } else {
      overlay.webContents.closeDevTools ();
    }
  });

  globalShortcut.register ('F8', () => {
    overlay.webContents.send ('clear');
  });
});