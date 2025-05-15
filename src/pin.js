import electron from 'electron';
import { windowManager } from 'node-window-manager';
import { logger } from './logger.js';

const TASKBAR_HEIGHT = 50;

const ALLOWED_WINDOWS = [
  'Dark and Darker',
  'GrimVault'
];

let previousGameBounds = null;

let isActive = false;
let isShown = false;

export async function pin (overlay, debugging) {
  let shouldHide = true;
  let activeWindow = windowManager.getActiveWindow ();

  if (activeWindow) {
    // Check if the active window is the game window

    let activeWindowTitle = activeWindow
      .getTitle ()
      .trim ();

    logger.debug (`Active window: ${activeWindowTitle}`);

    if (ALLOWED_WINDOWS.includes (activeWindowTitle)) {

      // If we are already active, we don't need to rebind anything.
      
      if (isActive) {
        return;
      }

      // Otherwise we became active and need to rebind the overlay.

      // The game window is not always the active window because GrimVault may
      // take active focus.

      let gameWindow = windowManager
        .getWindows ()
        .find (window => window.getTitle ().trim () === 'Dark and Darker');

      if (gameWindow) {
        let bounds = gameWindow.getBounds ();
        let monitor = gameWindow.getMonitor ();
        let scale = monitor.getScaleFactor ();

        let monitorBounds = monitor.getBounds ();

        bounds.x *= scale;
        bounds.y *= scale;
        bounds.width *= scale;
        bounds.height *= scale;

        overlay.webContents.send ('game:bounds', {
          ... bounds,

          // x and y relative to the monitor the game is running on
          x: bounds.x - monitorBounds.x,
          y: bounds.y - monitorBounds.y
        });

        let positionChanged = !previousGameBounds ||
          bounds.x !== previousGameBounds.x ||
          bounds.y !== previousGameBounds.y ||
          bounds.width !== previousGameBounds.width ||
          bounds.height !== previousGameBounds.height;

        if (positionChanged) {
          logger.info ('Updating overlay bounds: ', bounds);

          overlay.setBounds ({
            x: bounds.x,
            y: bounds.y,
            width: bounds.width,
            height: bounds.height
          });
        }
        
        if (!isShown) {
          overlay.setIgnoreMouseEvents (true, { forward: true });
          overlay.setAlwaysOnTop (true, 'screen-saver');
          overlay.setVisibleOnAllWorkspaces (true);
          overlay.show ();
          overlay.moveTop ();

          isShown = true;
        }

        previousGameBounds = bounds;

        isActive = true;
        shouldHide = false;
      } else {
        logger.warn ('Found a valid active window but could not find the game window');
      }
    } else {
      logger.debug ('Active window is not of interest');
    }
  } else {
    logger.warn ('Failed to get active window');
  }

  if (shouldHide) {
    if (isShown) {
      if (!debugging) {
        overlay.hide ();
      }
      
      isShown = false;
    }

    isActive = false;
    previousGameBounds = null;
  }
}