import electron from 'electron';
const { app } = electron;

import { isDebug, SOURCE } from './config.js';
import { exec } from 'node:child_process';
import { join } from 'node:path';
import { freemem, totalmem } from 'node:os';
import { logger, logPath } from './logger.js';

import nmi from 'node-machine-id';
const { machineId } = nmi;

export function isPackaged () {
  return app.isPackaged;
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

      // uploadLog (dxdiag);
    }
  });
};

// export async function uploadLog (path) {
//   if (!settings.general.telemetry) {
//     logger.info ('[System] Skipping log upload because telemetry is disabled');
//     return;
//   }

//   try {
//     const formData = new FormData ();
//     formData.append ('file', createReadStream (path));
//     formData.append ('machine_id', await machineId ());

//     logger.info (`Uploading log: ${path}`);

//     await api.post ('/v1/upload/logs', formData, {
//       headers: {
//         ...formData.getHeaders ()
//       }
//     });

//     logger.info (`Successfully uploaded log: ${path}`);  
//   } catch (error) {
//     logger.error (`Error uploading log: ${path}`, error);
//   }
// }