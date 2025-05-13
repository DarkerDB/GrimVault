import electron from 'electron';
import { existsSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';
import winston from 'winston';
import 'winston-daily-rotate-file';
import { isDebug } from './config.js';

const { app } = electron;
const logPath = join (app.getPath ('userData'), 'logs');

if (!existsSync (logPath)) {
  mkdirSync (logPath, { recursive: true });
}

const transport = new winston.transports.DailyRotateFile ({ 
  filename: join (logPath, '%DATE%.log'),
  datePattern: 'YYYY-MM-DD',
  maxSize: '2m',
  maxFiles: 5,
  zippedArchive: false,
  createSymlink: true
});

const logger = winston.createLogger ({
  level: isDebug () ? 'debug' : 'info',
  format: winston.format.combine (
    winston.format.timestamp (),
    winston.format.printf (({ level, message, timestamp, ... meta }) => {
      return `${timestamp} [${level}] ${message} ${Object.keys (meta).length ? JSON.stringify (meta, null, 2) : ''}`
    })
  ),
  transports: [
    transport
  ]
});

if (isDebug ()) {
  logger.add (new winston.transports.Console ({
    format: winston.format.combine (
      winston.format.timestamp (),
      winston.format.colorize ({ all: false, level: true }),
      winston.format.printf (({ level, message, timestamp, ...meta }) => {
        const grayColor = '\x1b[90m'; // ANSI escape code for gray
        const resetColor = '\x1b[0m';  // ANSI escape code to reset color
        const metaString = Object.keys(meta).length 
          ? `${grayColor}${JSON.stringify(meta, null, 2)}${resetColor}` 
          : '';
        
          return `${timestamp} [${level}] ${message} ${metaString}`;
      })
    )
  }));
}

// transport.on ('rotate', async (oldFile, newFile) => {
//   if (isDebug ()) {
//     return;
//   }

//   uploadLog (oldFile);
// });

export { logger, transport, logPath };