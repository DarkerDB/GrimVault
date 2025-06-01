import electron from 'electron';
import { logger } from './logger.js';
import { settings } from './settings.js';
import { getTooltip } from './native.js';
import { api } from './api.js';

const frontend = electron.ipcMain;

export function wire (overlay) {
  let send = (messageType, data) => {
    logger.debug (`Sending frontend message: ${messageType}`);
    overlay.webContents.send (messageType, data);
  };

  frontend.on ('ready', () => {
    logger.info ('Received frontend ready state');
    
    // If in safe mode, log it but don't send a notification
    if (settings.general.safe_mode) {
      logger.info('Running in safe mode with reduced performance');
    }
    
    send ('settings', settings);
  });

  frontend.on ('log', (event, data) => {
    logger.log (
      data.level,
      `[Frontend] ${data.message}`,
      data.meta || {}
    );
  });

  frontend.on ('scan', async () => {
    send ('scan:start');

    let tooltip;

    try {
      tooltip = await getTooltip ();
    } catch (e) {
      logger.error (`Error getting tooltip: ${e}`);
    }

    if (tooltip) {
      let stats = await getItemStats (tooltip.text);

      if (stats) {
        send ('hover:item', {
          ... tooltip,
          ... stats
        });
      }
    } else {
      send ('clear');
    }

    send ('scan:finish');
  });
}

async function getItemStats (tooltipText) {
  try {
    let response = await api.get ('/v1/internal/grimvault/analyze', {
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
