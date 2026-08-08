import Koa from 'koa';
import Router from '@koa/router';
import bodyParser from 'koa-bodyparser';
import cors from '@koa/cors';
import { promises as fs } from 'fs';
import path from 'path';
import { app } from 'electron';
import { logger } from './logger.js';

class AuthServer {
  constructor () {
    this.app = new Koa ();
    this.router = new Router ();
    this.server = null;
    this.port = 7777;
    this.credentialsPath = path.join (app.getPath ('userData'), 'credentials.json');
    
    this.setupMiddleware ();
    this.setupRoutes ();
  }

  setupMiddleware () {
    this.app.use (cors ({
      origin: ctx => {
        const allowedOrigins = ['https://darkerdb.com', 'http://localhost:5173'];
        const origin = ctx.get ('Origin');
        return allowedOrigins.includes (origin) ? origin : false;
      },
      credentials: true
    }));
    
    this.app.use (bodyParser ());
    
    this.app.use (async (ctx, next) => {
      logger.info (`Auth server request: ${ctx.method} ${ctx.path}`);
      await next ();
    });
  }

  setupRoutes () {
    this.router.post ('/auth', async (ctx) => {
      try {
        const { key, settings } = ctx.request.body;
        
        if (!key) {
          ctx.status = 400;
          ctx.body = { error: 'API key required' };
          return;
        }
        
        await this.saveCredentials ({ key });
        
        logger.info ('Successfully linked GrimVault account');
        
        // Fetch settings from server after authentication
        try {
          const { api } = await import ('./api.js');
          const response = await api.get ('/v1/grimvault/settings');
          
          if (response.data) {
            const { updateFromServer } = await import ('./settings.js');
            updateFromServer (response.data);
          }
        } catch (error) {
          logger.error ('Failed to fetch settings from server:', error);
        }
        
        ctx.body = { success: true, message: 'Account linked successfully' };
      } catch (error) {
        logger.error ('Auth error:', error);
        ctx.status = 500;
        ctx.body = { error: 'Failed to save credentials' };
      }
    });
    
    this.router.post ('/logout', async (ctx) => {
      try {
        await this.clearCredentials ();
        logger.info ('Cleared GrimVault credentials');
        ctx.body = { success: true };
      } catch (error) {
        logger.error ('Logout error:', error);
        ctx.status = 500;
        ctx.body = { error: 'Failed to clear credentials' };
      }
    });
    
    this.router.get ('/status', async (ctx) => {
      try {
        const hasCredentials = await this.hasCredentials ();
        ctx.body = { linked: hasCredentials };
      } catch (error) {
        ctx.body = { linked: false };
      }
    });
    
    this.app.use (this.router.routes ());
    this.app.use (this.router.allowedMethods ());
  }

  async start () {
    return new Promise ((resolve, reject) => {
      this.server = this.app.listen (this.port, '127.0.0.1', () => {
        logger.info (`Auth server listening on http://localhost:${this.port}`);
        resolve ();
      });
      
      this.server.on ('error', (error) => {
        if (error.code === 'EADDRINUSE') {
          logger.error (`Port ${this.port} is already in use`);
        } else {
          logger.error ('Auth server error:', error);
        }
        reject (error);
      });
    });
  }

  stop () {
    if (this.server) {
      this.server.close (() => {
        logger.info ('Auth server stopped');
      });
    }
  }

  async saveCredentials (credentials) {
    const dir = path.dirname (this.credentialsPath);
    await fs.mkdir (dir, { recursive: true });
    await fs.writeFile (this.credentialsPath, JSON.stringify (credentials, null, 2));
  }

  async loadCredentials () {
    try {
      const data = await fs.readFile (this.credentialsPath, 'utf8');
      return JSON.parse (data);
    } catch (error) {
      if (error.code !== 'ENOENT') {
        logger.error ('Failed to load credentials:', error);
      }
      return null;
    }
  }

  async hasCredentials () {
    const creds = await this.loadCredentials ();
    return creds && creds.key;
  }

  async clearCredentials () {
    try {
      await fs.unlink (this.credentialsPath);
    } catch (error) {
      if (error.code !== 'ENOENT') {
        throw error;
      }
    }
  }

  async getApiKey () {
    const creds = await this.loadCredentials ();
    return creds?.key || null;
  }
}

export const authServer = new AuthServer ();