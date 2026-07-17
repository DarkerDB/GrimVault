import electron from 'electron';
import { logger } from './logger.js';
import { settings } from './settings.js';
import { getTooltip } from './native.js';
import { api } from './api.js';
import { authServer } from './authServer.js';
import { getCanScan } from './pin.js';

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

  frontend.on ('scan', async (event, data) => {
    const scanId = data?.scanId || 0;

    send ('scan:start');

    // Helper to send error events
    const sendError = (message, tooltip = null) => {
      send ('hover:error', {
        scanId,
        message,
        x: tooltip?.x || 0,
        y: tooltip?.y || 0,
        width: tooltip?.width || 100,
        height: tooltip?.height || 50
      });
    };

    // Safety check: Verify window state before scanning
    if (!getCanScan ()) {
      logger.debug ('Scan rejected: game window not in valid state for scanning');
      send ('clear', { scanId });
      send ('scan:finish');
      return;
    }

    let tooltip;

    try {
      tooltip = await getTooltip ();
    } catch (e) {
      logger.error (`Error getting tooltip: ${e}`);
    }

    logger.debug ('Found tooltip: ', tooltip);

    if (tooltip) {
      let result = await getItemStats (tooltip.text);

      if (result.success) {
        send ('hover:item', {
          scanId,
          ... tooltip,
          ... result.data
        });
      } else {
        // Error occurred during API call
        sendError (result.error, tooltip);
      }
    } else {
      send ('clear', { scanId });
    }

    send ('scan:finish');
  });
  
  frontend.handle ('auth:status', async () => {
    const hasCredentials = await authServer.hasCredentials ();
    return { linked: hasCredentials };
  });
  
  frontend.handle ('auth:logout', async () => {
    await authServer.clearCredentials ();
    return { success: true };
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
      return {
        success: false,
        error: 'No response from server'
      };
    }

    return {
      success: true,
      data: response.data.body
    };
  } catch (e) {
    logger.error ('API error:', e);

    // Extract error message from response
    let errorMessage = 'Unknown error occurred';

    if (e.response) {
      // Server responded with error status
      if (e.response.data?.errors && Array.isArray (e.response.data.errors) && e.response.data.errors.length > 0) {
        // Use first error from errors array
        errorMessage = e.response.data.errors[0];
      } else if (e.response.data?.status) {
        // Use status message
        errorMessage = e.response.data.status;
      } else if (e.response.statusText) {
        // Use HTTP status text
        errorMessage = `${e.response.status}: ${e.response.statusText}`;
      }
    } else if (e.message) {
      // Network error or other client-side error
      errorMessage = e.message;
    }

    return {
      success: false,
      error: errorMessage
    };
  }
}
