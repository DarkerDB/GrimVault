import '@/common/styles/main.css';
import '@/common/scripts/environment.js';

import { createApp } from 'vue';
import App from './App.vue';

const app = createApp (App);

app.mount ('#app');