import axios from 'axios';
import electron from 'electron';
import { isDebug } from "./util.js";
import { logger } from "./logger.js";

import nmi from 'node-machine-id';
const { machineIdSync } = nmi;

const { app } = electron;

const api = axios.create ({
  baseURL: isDebug ()
    ? 'https://api-dev.darkerdb.com'
    : 'https://api.darkerdb.com',

  timeout: 15000,
  headers: { "User-Agent": `GrimVault v${app.getVersion ()} (${machineIdSync ()})` },
  withCredentials: 'include'
});

api.interceptors.request.use ((request) => {
  logger.debug (`${request.method.toUpperCase ()} ${getUrl (request)}`);
  return request;
});

api.interceptors.response.use(
  (response) => {
    logger.debug (`${response.status} ${response.statusText} (${response.config.method.toUpperCase ()} ${getUrl (response.config)})`);
    return response;
  },

  (error) => {
    if (error.config) {
      logger.error (`${error.name}: ${error.message} (${error.config.method.toUpperCase ()} ${getUrl (error.config)})`);
    } else {
      logger.error (`${error.name}: ${error.message}`);
    }
  },
);

function getUrl (request) {
  const baseURL = request.baseURL || "";
  const url = new URL (request.url, baseURL);

  if (request.params) {
    Object.keys (request.params).forEach ((key) =>
      url.searchParams.append (key, request.params [key]),
    );
  }

  return url.toString ();
}

export { api };
