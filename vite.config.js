import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vite.dev/config/
export default defineConfig({
  plugins: [react()],
  base: '/455-Brain-Stim-2/',
  build: {
    outDir: 'docs'
  }
})
