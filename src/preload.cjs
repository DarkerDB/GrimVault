const { contextBridge, ipcRenderer } = require ('electron');

contextBridge.exposeInMainWorld ('logger', (() => {
  let bridge = {};

  for (let level of [ 'info', 'warn', 'error', 'debug' ]) {
    bridge [level] = (message, meta = {}) => {
      ipcRenderer.send ('log', { level, message, meta });
    };
  }

  return bridge;
}) ());

contextBridge.exposeInMainWorld ('electron', {
  send: (channel, data) => {
    ipcRenderer.send (channel, data);
  },
  
  on: (channel, func) => {
    ipcRenderer.addListener (channel, (event, ...args) => func (...args));
  },
  
  off: (channel, func) => {
    ipcRenderer.removeListener (channel, func);
  },

  once: (channel, func) => {
    ipcRenderer.once (channel, (event, ... args) => func (... args));
  }
});