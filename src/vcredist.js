import Registry from 'winreg';

import { exec, execFile } from 'node:child_process';
import { unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { app, dialog } from 'electron';
import { download } from 'electron-dl';
import { logger } from './logger.js';

// VC++ 2015-2022 x64 registry keys
// Visual Studio 2015, 2017, 2019, and 2022
const VC_REDIST_KEYS = [
  {
      key: '\\SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64',
      name: 'Version'
  },
  {
      key: '\\SOFTWARE\\WOW6432Node\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64',
      name: 'Version'
  }
];

// Download URL for latest VC++ Redistributable
const VC_REDIST_URL = 'https://aka.ms/vs/17/release/vc_redist.x64.exe';

async function checkRegistry (regKey) {
  return new Promise ((resolve) => {
    const key = new Registry ({
      hive: Registry.HKLM,
      key: regKey.key
    });

    key.get (regKey.name, (error, item) => {
      if (error) {
        logger.error ('Registry error', {
          key: regKey.key,
          name: regKey.name,
          error: error.message,
          stack: error.stack
        });

        resolve (false);
        return;
      } 

      resolve (true);
    });
  });

}

async function isVCRedistInstalled () {
  logger.info ('Checking registry for VC++ redistributable');

  for (const key of VC_REDIST_KEYS) {
    if (await checkRegistry (key)) {
      return true;
    }
  }

  return false;
}

async function installVCRedist (window) {
  try {
    const tempPath = app.getPath ('temp') || 'C:\\Windows\\Temp';
    const downloadPath = join (tempPath, 'vc_redist.x64.exe');

    await download (window, VC_REDIST_URL, {
      directory: tempPath,
      filename: 'vc_redist.x64.exe',

      onProgress: (progress) => {
        logger.info ('Redistributable download progress: ' + (progress.percent * 100) + '%');
      }
    });

    logger.info ('Redistributable download complete');

    await new Promise ((resolve, reject) => {
      const installer = execFile (downloadPath, ['/quiet', '/norestart'], {
          timeout: 120000,
          windowsHide: true
      }, (error, stdout, stderr) => {
        if (error) {
          logger.error (`Installation error: ${error}`);
          logger.error (`Installation stderr: ${stderr}`);

          reject (error);
          return;
        }

        resolve ();
      });

      // Log any output
      installer.stdout?.on ('data', (data) => {
        logger.info (`Installer output: ${data}`);
      });

      installer.stderr?.on ('data', (data) => {
        logger.error (`Installer error: ${data}`);
      });
    });

    // Cleanup downloaded file
    unlinkSync (downloadPath);

    dialog.showMessageBox (window, {
      type: 'info',
      message: 'Installation Complete',
      detail: 'Visual C++ Redistributable has been installed successfully.'
    });

    return true;
  } catch (error) {
    logger.error (`VCRedist installation error: ${error}`);

    dialog.showErrorBox (
      'Installation Error',
      'Failed to install Visual C++ Redistributable. Please download and install manually.'
    );
  
    return false;
  }

  return false;
}

export async function checkAndInstallVCRedist (window) {
  const installed = await isVCRedistInstalled ();

  if (!installed) {
    const choice = await dialog.showMessageBox (window, {
      type: 'warning',
      message: 'Missing Required Component',
      detail: 'Visual C++ Redistributable 2015-2022 is required but not installed. Would you like to install it now?',
      buttons: [ 'Install', 'Cancel' ],
      defaultId: 0
    });

    if (choice.response === 0) {
      return await installVCRedist (window);
    }

    return false;
  }

  return true;
}