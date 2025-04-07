import electron from 'electron';
const { app, screen } = electron;

import { api } from './api.js';
import { SOURCE } from './config.js';
import FormData from 'form-data';
import { exec } from 'node:child_process';
import { createReadStream } from 'node:fs';
import { join } from 'node:path';
import { freemem, machine, totalmem } from 'node:os';
import { logger, logPath } from './logger.js';
import { settings } from './settings.js';

import nmi from 'node-machine-id';
const { machineId } = nmi;

export function isDebug () {
  return process.env.NODE_ENV === 'development';
}

export function isPackaged () {
  return app.isPackaged;
}

export function bringToFront (overlay) {  
  // overlay.setIgnoreMouseEvents (true, { forward: true });
  logger.info ('Bringing overlay to the front');

  overlay.setAlwaysOnTop (true, 'screen-saver', 1);

  overlay.show ();
  overlay.moveTop ();
}

let previousMonitorId;

export function updateOverlayPosition (overlay, window) {
  // let windowLower = window.toLowerCase ();

  // let shouldBeVisible = windowLower === 'dungeoncrawler.exe' ||
  //                       windowLower === 'grimvault.exe' ||
  //                       windowLower === 'electron.exe';

  let isCurrentlyVisible = overlay.isVisible ();

  function updateBounds () {
    const displays = screen.getAllDisplays ();

    const currentMonitor = displays.find ((display) => {
      const { x, y, width, height } = display.bounds;

      return (
        window.x >= x &&
        window.x < x + width &&
        window.y >= y &&
        window.y < y + height
      );
    });

    if (!currentMonitor) {
      logger.warn ('Failed to find monitor running the game');
    } else {
      logger.info (`Found monitor running the game: ${currentMonitor.id}`);
    }

    if (currentMonitor && currentMonitor.id !== previousMonitorId) {
      const dimensions = {
        x: currentMonitor.bounds.x,
        y: currentMonitor.bounds.y,
        width: currentMonitor.bounds.width,
        height: currentMonitor.bounds.height - 50,
      };

      logger.info ('Setting overlay dimensions', dimensions);

      overlay.setBounds (dimensions);

      logger.info (`Moved overlay to monitor: ${currentMonitor.id}`);
      previousMonitorId = currentMonitor.id;
    }
  }

  // try {
    // if (shouldBeVisible) {
      logger.info ('Changed active window: ', window);
      
      // if (window === 'DungeonCrawler.exe') {
        updateBounds ();
      // }
      
      if (!isCurrentlyVisible) {
        bringToFront (overlay);
      }
    // } else {
    //   overlay.hide ();
    // }
  // } catch (error) {
  //   logger.error (`Encountered error updating overlay position: ${error}`);
  //   overlay.hide ();
  // }
}

export async function logSystemInformation () {
  logger.info (`[System] Machine ID: ${await machineId ()}`);
  logger.info (`[System] Platform: ${process.platform}`);
  logger.info (`[System] Architecture: ${process.arch}`);
  logger.info (`[System] Node Version: ${process.version}`);
  logger.info (`[System] Chrome Version: ${process.versions.chrome}`);
  logger.info (`[System] Electron Version: ${process.versions.electron}`);
  logger.info (`[System] Resources Path: ${process.resourcesPath}`);
  logger.info (`[System] Process Directory: ${SOURCE}`);
  logger.info (`[System] Total Memory: ${(totalmem () / 1024 / 1024 / 1024).toFixed (2)} GB`);
  logger.info (`[System] Free Memory: ${(freemem () / 1024 / 1024 / 1024).toFixed (2)} GB`);

  const dxdiag = join (logPath, 'dxdiag.txt');
  const command = `dxdiag /t ${dxdiag}`;

  logger.info (`[System] Running command: ${command}`);

  exec (command, async function (error, stdout, stderr) {
    if (error) {
      logger.error (`[System] Failed to log DXDIAG`);
    } else {
      logger.info (`[System] DXDIAG Saved`);

      if (isDebug ()) {
        return;
      }

      uploadLog (dxdiag);
    }
  });
};

export async function uploadLog (path) {
  if (!settings.general.telemetry) {
    logger.info ('[System] Skipping log upload because telemetry is disabled');
    return;
  }

  try {
    const formData = new FormData ();
    formData.append ('file', createReadStream (path));
    formData.append ('machine_id', await machineId ());

    logger.info (`Uploading log: ${path}`);

    await api.post ('/v1/upload/logs', formData, {
      headers: {
        ...formData.getHeaders ()
      }
    });

    logger.info (`Successfully uploaded log: ${path}`);  
  } catch (error) {
    logger.error (`Error uploading log: ${path}`, error);
  }
}