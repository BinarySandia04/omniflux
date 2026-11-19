export default defineNuxtConfig({
  compatibilityDate: '2025-01-01',
  devtools: { enabled: false },
  runtimeConfig: {
    public: {
      // Override with NUXT_PUBLIC_API_BASE if the backend runs elsewhere.
      apiBase: 'http://localhost:3001',
    },
  },
})
