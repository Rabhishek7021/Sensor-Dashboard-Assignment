/**
 * @file main.jsx
 * @brief Browser entry point: mounts the dashboard (App) into the \#root element.
 */
import React from 'react';
import { createRoot } from 'react-dom/client';
import App from './App.jsx';
import './styles.css';

/** Renders the app in StrictMode (extra development-time checks; no effect in production builds). */
createRoot(document.getElementById('root')).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
