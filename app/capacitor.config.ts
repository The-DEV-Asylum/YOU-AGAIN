import type { CapacitorConfig } from '@capacitor/cli';

const config: CapacitorConfig = {
  appId: 'com.antistalker.app',
  appName: 'AntiStalker',
  webDir: 'www',
  backgroundColor: '#08110f',
  plugins: {
    CapacitorHttp: {
      enabled: true,
    },
  },
};

export default config;
