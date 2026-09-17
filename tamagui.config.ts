import { createFont, createTamagui, createTokens } from 'tamagui';

const bodyFont = createFont({
  family: 'System',
  size: {
    1: 12,
    2: 14,
    3: 16,
    4: 18,
    5: 20,
    6: 24,
    7: 30,
    8: 36,
  },
  lineHeight: {
    1: 16,
    2: 20,
    3: 24,
    4: 26,
    5: 28,
    6: 32,
    7: 38,
    8: 44,
  },
  weight: {
    1: '400',
    2: '500',
    3: '600',
    4: '700',
  },
  letterSpacing: {
    1: 0,
    2: -0.2,
    3: -0.4,
  },
});

export const tokens = createTokens({
  color: {
    white: '#FFFFFF',
    ink: '#17212B',
    inkMuted: '#5E6A75',
    background: '#F6F8FB',
    surface: '#FFFFFF',
    surfaceMuted: '#EEF3F5',
    border: '#D7E0E5',
    primary: '#0D6E5A',
    primarySoft: '#E3F2EE',
    statusNormal: '#1F9D65',
    statusWaspada: '#D89000',
    statusAngkat: '#E26A2C',
    statusSafetyAlert: '#C63C3C',
    statusOffline: '#68727C',
    statusDataInvalid: '#7A4EA3',
  },
  space: {
    0: 0,
    1: 4,
    2: 8,
    3: 12,
    4: 16,
    5: 20,
    6: 24,
    7: 32,
    8: 40,
    9: 48,
    10: 64,
  },
  size: {
    0: 0,
    1: 4,
    2: 8,
    3: 12,
    4: 16,
    5: 20,
    6: 24,
    7: 32,
    8: 40,
    9: 48,
    10: 64,
  },
  radius: {
    0: 0,
    1: 4,
    2: 8,
    3: 12,
    4: 16,
    5: 24,
    6: 999,
  },
  zIndex: {
    0: 0,
    1: 1,
    2: 10,
    3: 100,
  },
});

export const tamaguiConfig = createTamagui({
  fonts: {
    body: bodyFont,
    heading: bodyFont,
  },
  tokens,
  themes: {
    light: {
      background: '#F6F8FB',
      surface: '#FFFFFF',
      surfaceMuted: '#EEF3F5',
      color: '#17212B',
      colorMuted: '#5E6A75',
      borderColor: '#D7E0E5',
      primary: '#0D6E5A',
      primarySoft: '#E3F2EE',
      statusNormal: '#1F9D65',
      statusWaspada: '#D89000',
      statusAngkat: '#E26A2C',
      statusSafetyAlert: '#C63C3C',
      statusOffline: '#68727C',
      statusDataInvalid: '#7A4EA3',
    },
  },
  media: {
    compact: { maxWidth: 767 },
    wide: { minWidth: 768 },
  },
  shorthands: {
    bg: 'backgroundColor',
    br: 'borderRadius',
    f: 'flex',
    fd: 'flexDirection',
    fw: 'flexWrap',
    h: 'height',
    jc: 'justifyContent',
    m: 'margin',
    mb: 'marginBottom',
    ml: 'marginLeft',
    mr: 'marginRight',
    mt: 'marginTop',
    mx: 'marginHorizontal',
    my: 'marginVertical',
    p: 'padding',
    pb: 'paddingBottom',
    pl: 'paddingLeft',
    pr: 'paddingRight',
    pt: 'paddingTop',
    px: 'paddingHorizontal',
    py: 'paddingVertical',
    w: 'width',
  } as const,
});

type AppConfig = typeof tamaguiConfig;

declare module 'tamagui' {
  interface TamaguiCustomConfig extends AppConfig {}
}

export default tamaguiConfig;
