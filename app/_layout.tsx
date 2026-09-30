import {
  Inter_400Regular,
  Inter_500Medium,
  Inter_600SemiBold,
  Inter_700Bold,
  Inter_800ExtraBold,
} from '@expo-google-fonts/inter';
import { useFonts } from 'expo-font';
import { Slot } from 'expo-router';
import { useEffect } from 'react';
import { Platform } from 'react-native';
import { TamaguiProvider } from 'tamagui';

import { MotionProvider } from '../components/MotionProvider';
import { tamaguiConfig } from '../tamagui.config';

export default function RootLayout() {
  const [fontsLoaded] = useFonts({
    Inter: Inter_400Regular,
    Inter_500Medium,
    Inter_600SemiBold,
    Inter_700Bold,
    Inter_800ExtraBold,
  });

  useEffect(() => {
    if (Platform.OS !== 'web') return undefined;
    const style = document.createElement('style');
    style.setAttribute('data-sigap-font', 'inter');
    style.textContent = 'body, body * { font-family: Inter, Arial, sans-serif !important; }';
    document.head.appendChild(style);
    return () => style.remove();
  }, []);

  if (!fontsLoaded) return null;

  return (
    <TamaguiProvider config={tamaguiConfig} defaultTheme="light">
      <MotionProvider>
        <Slot />
      </MotionProvider>
    </TamaguiProvider>
  );
}
