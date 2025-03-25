import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

export const __filename = fileURLToPath (import.meta.url);
export const __dirname = dirname (__filename);

export const SOURCE = __dirname;
export const ROOT = join (SOURCE, '..');