import { distance } from './util.js';

let sleepPosition = {
  x: null,
  y: null
};

let currentPosition = {
  x: null,
  y: null
};

export function onMouseStill (callback, stillForMs = 100) {
  let lastMoveTime = null;
  
  let lastPosition = { 
    x: null, 
    y: null 
  };

  function onMouseMove (event) {
    const now = Date.now ();

    currentPosition = { 
      x: event.clientX, 
      y: event.clientY 
    };

    if (
        lastPosition.x !== currentPosition.x ||
        lastPosition.y !== currentPosition.y
    ) {
        lastPosition = currentPosition;
        lastMoveTime = now;
    }
  }

  function onCheckStill () {
    const now = Date.now ();

    // If enough time has passed without movement
    if (lastMoveTime && now - lastMoveTime >= stillForMs) {
      callback ();

      sleepPosition = lastPosition;

      lastMoveTime = null; // Reset to avoid repeated detection
      
      lastPosition = {
        x: null,
        y: null
      };
    }
  }

  window.addEventListener ('mousemove', onMouseMove);

  // Check stillness periodically (e.g., every 100 ms)
  const interval = setInterval (onCheckStill, 100);

  return () => clearInterval (interval);
}

export function onMouseWakeup (callback, minDistance = 5) {
  function onMouseMove (event) {
    if (sleepPosition.x === null ||
        sleepPosition.y === null) {
      return;
    }

    if (distance (sleepPosition, { x: event.clientX, y: event.clientY }) > minDistance) {
      sleepPosition = {
        x: null,
        y: null
      };

      callback ();
    }
  }

  window.addEventListener ('mousemove', onMouseMove);
}

export function setMouseSleepPosition (position) {
  if (!position) {
    position = currentPosition;
  }

  sleepPosition = position;
}