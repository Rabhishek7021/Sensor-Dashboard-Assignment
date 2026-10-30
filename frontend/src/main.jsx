/**
 * @file main.jsx
 * @brief Browser entry point: mounts the dashboard (App) into the \#root element.
 */
import React from 'react';
import { createRoot } from 'react-dom/client';
import App from './App.jsx';
// Typefaces bundled with the app (Latin subset only), so the dashboard needs no
// internet access: IBM Plex Mono for headings, tabs and readouts, IBM Plex Sans for text.
import '@fontsource/ibm-plex-mono/latin-400.css';
import '@fontsource/ibm-plex-mono/latin-500.css';
import '@fontsource/ibm-plex-mono/latin-600.css';
import '@fontsource/ibm-plex-sans/latin-400.css';
import '@fontsource/ibm-plex-sans/latin-500.css';
import './styles.css';

/** Renders the app in StrictMode (extra development-time checks; no effect in production builds). */
createRoot(document.getElementById('root')).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
