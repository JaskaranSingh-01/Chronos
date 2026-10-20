import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './App';
import { lab } from './lab';
import { store } from './store';
import './styles.css';

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
void store.start();
void lab.start();
