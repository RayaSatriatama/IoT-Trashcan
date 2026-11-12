import React, { useSyncExternalStore } from 'react';
const { renderToStaticMarkup } = require('react-dom/server') as { renderToStaticMarkup: (node: React.ReactNode) => string };
import { afterEach, describe, expect, it, vi } from 'vitest';

vi.mock('tamagui', async (importOriginal) => {
  const actual = await importOriginal<any>();
  const React = require('react');
  const element = (tag: string) => ({ children, ...props }: any) => {
    const domProps: any = {};
    for (const [key, value] of Object.entries(props)) if (key.startsWith('aria-') || ['id','role','type','value','disabled','tabIndex'].includes(key)) domProps[key] = value;
    if (props.secureTextEntry) domProps.type = 'password';
    if (props.onChangeText) domProps.onChange = () => {};
    if (props.onPress) domProps.onClick = props.onPress;
    return React.createElement(tag, domProps, children);
  };
  return { ...actual, useMedia: () => ({ compact: false }), ScrollView: element('div'), Card: element('section'), Input: element('input'), Text: element('span'), XStack: element('div'), YStack: element('div'), Button: element('button') };
});
vi.mock('expo-router', () => ({ useLocalSearchParams: () => ({ id: 'fixture' }), useRouter: () => ({ back: vi.fn(), push: vi.fn() }) }));
vi.mock('react-native', () => ({ Image: () => null }));
// Composition/owner assertions do not exercise a real tooltip portal; browser QA does.
vi.mock('../../components/SectionHeader', () => ({ DescriptionHint: () => null }));
vi.mock('../../lib/ui/icons', () => Object.fromEntries('Activity ArrowLeft ArrowRight Bell Bluetooth Box CircleUserRound Clock3 ChevronDown ChevronRight ChevronUp Download Filter FlaskConical History House Info MapPin Menu Search RefreshCw Scale Settings ShieldAlert ShieldCheck SlidersHorizontal Terminal Thermometer Truck TriangleAlert Wifi Wind'.split(' ').map(key => [key, () => null])));
vi.mock('../../components/device/SensorSelector', () => ({ SensorSelector: ({ value, disabled }: any) => React.createElement('button', { 'aria-label': 'Pilih sensor', disabled }, value) }));

import { DeviceBleSession } from '../../lib/ble/device-session';
import { WifiSettingsController } from '../../lib/ble/wifi-settings';
import { WifiSettingsDialog } from '../../components/device/WifiSettingsDialog';
import { CalibrationWizard } from '../../components/device/CalibrationWizard';
import { DeviceDetailSessionView } from '../../app/devices/[id]';
import { DeviceWorkflowSteps } from '../../components/device/DeviceWorkflowSteps';
import { CALIBRATION_STEPS } from '../../lib/ui/deviceTools';

const clients: Array<{ options: any; client: any }> = [];
function makeClient(options: any) {
  const client = {
    connect: vi.fn(async () => options.onConnectionChange(true)),
    verifyDeviceIdentity: vi.fn(async (id: string) => { options.onVerifiedDeviceIdentityChange(id); return id; }),
    close: vi.fn(() => options.onConnectionChange(false)),
    status: vi.fn(async () => ({ state: 'CONNECTING', ssid: 'Net' })),
    listKnownWifi: vi.fn(async () => []), scanDetailed: vi.fn(async () => ({ networks: [{ ssid: 'Visible', rssi: -42, secure: true }], rawCount: 2 })),
    connectWifi: vi.fn(async () => ({ state: 'CONNECTING', ssid: 'Net' })),
    retryWifi: vi.fn(async () => ({ state: 'CONNECTING', ssid: 'Net' })),
    calibrationCapabilities: vi.fn(async () => ({ status: 'OK', sensor: 'mq7', data: { references: 'none', criteria: 'none', engineering: 'none' } })),
    calibrationEvidence: vi.fn(async () => ({ status: 'OK', sensor: 'mq7', data: {} })),
    calibrationStart: vi.fn(), calibrationCriteria: vi.fn(), calibrationCapture: vi.fn(), calibrationValidate: vi.fn(), calibrationPreview: vi.fn(), calibrationCommit: vi.fn(), calibrationCancel: vi.fn(),
  };
  clients.push({ options, client }); return client;
}
function session(id: string | null) { return new DeviceBleSession(id, { createClient: makeClient as any }); }
function WifiHarness({ owner, controller }: { owner: DeviceBleSession; controller: WifiSettingsController }) {
  useSyncExternalStore(controller.subscribe, controller.getSnapshot, controller.getSnapshot);
  return <WifiSettingsDialog deviceId="device" bleDeviceId={owner.getSnapshot().deviceId} session={owner} controller={controller} />;
}
function CalibrationHarness({ owner, transport = 'ble' }: { owner: DeviceBleSession; transport?: 'ble' | 'https' }) {
  const snapshot = useSyncExternalStore(owner.subscribe, owner.getSnapshot, owner.getSnapshot);
  return <CalibrationWizard deviceId="canonical-device" sensorId="mq7" diagnostic={null} api={null} transport={transport} bleClient={owner.calibrationAdapter} bleConnected={snapshot.connected} onControlActivity={owner.beginControlActivity} />;
}

afterEach(() => { clients.length = 0; });

describe('two-stage calibration presentation', () => {
  it('maps the seven acknowledged protocol steps into exactly two informative stages', () => {
    const first = renderToStaticMarkup(<DeviceWorkflowSteps steps={CALIBRATION_STEPS} current={0} />);
    const second = renderToStaticMarkup(<DeviceWorkflowSteps steps={CALIBRATION_STEPS} current={4} />);
    expect(first).toContain('Kriteria &amp; Capture');
    expect(first).toContain('Validasi &amp; Simpan');
    expect(first).toContain('Diagnostik');
    expect(second).toContain('Preview');
    expect(first.match(/aria-label="1\. /g)).toHaveLength(1);
    expect(first.match(/aria-label="2\. /g)).toHaveLength(1);
  });
});

describe('production device session composition', () => {
  it('renders both real consumers against one owner and never creates a second client', async () => {
    const owner = session('canonical-device'); const controller = new WifiSettingsController(owner);
    await owner.connect();
    controller.open(); await controller.scan();
    const html = renderToStaticMarkup(<><WifiHarness owner={owner} controller={controller} /><CalibrationHarness owner={owner} /></>);
    expect(html).toContain('Atur Wi-Fi');
    expect(html).toContain('Kalibrasi sensor');
    expect(html).toContain('2 jaringan terdeteksi · 1 dapat dipilih');
    expect(html).toContain('2 jaringan terdeteksi · 1 dapat dipilih');
    renderToStaticMarkup(<><WifiHarness owner={owner} controller={controller} /><CalibrationHarness owner={owner} /></>);
    expect(clients).toHaveLength(1);
    expect(owner.calibrationAdapter).toBeDefined();
    controller.dispose(); owner.dispose();
  });

  it('rejects missing canonical identity before opening the chooser', async () => {
    const owner = session(null);
    await expect(owner.connect()).rejects.toThrow(/identity/i);
    expect(clients[0].client.connect).not.toHaveBeenCalled();
    owner.dispose();
  });

  it('keeps GATT on dialog close; explicit shared disconnect closes once', async () => {
    const owner = session('canonical-device'); const controller = new WifiSettingsController(owner);
    await owner.connect(); controller.close();
    expect(clients[0].client.close).not.toHaveBeenCalled();
    owner.close(); expect(clients[0].client.close).toHaveBeenCalledTimes(1);
    controller.dispose(); owner.dispose();
  });

  it('does not report CONNECTING as a confirmed Wi-Fi success', async () => {
    const owner = session('canonical-device'); const controller = new WifiSettingsController(owner);
    await owner.connect(); controller.open(); controller.selectSsid('Net'); controller.setPassword('secret'); await controller.submit();
    expect(controller.getSnapshot().status?.state).toBe('CONNECTING');
    expect(controller.getSnapshot().success).toBeNull();
    expect(controller.getSnapshot().password).toBe('');
    controller.dispose(); owner.dispose();
  });

  it('keeps HTTPS activity independent from BLE disconnect and stale releases', () => {
    const owner = session('canonical-device');
    const releaseHttps = owner.beginControlActivity('https-calibration');
    const releaseBle = owner.beginControlActivity('ble-calibration');
    owner.close(); releaseBle();
    expect(owner.getSnapshot().controlActive).toBe(true);
    releaseHttps(); releaseHttps();
    expect(owner.getSnapshot().controlActive).toBe(false);
    owner.dispose();
  });

  it('renders explicit shared disconnect and activity callback on route composition', async () => {
    const owner = session('canonical-device');
    const controller = new WifiSettingsController(owner);
    const html = renderToStaticMarkup(<DeviceDetailSessionView owner={owner} controller={controller} deviceCode="unit" bleDeviceId="canonical-device" sensorId="mq7" onSensorChange={vi.fn()} diagnostic={null} onDiagnosticRefresh={vi.fn()} />);
    expect(html).toContain('Konfigurasi Wi-Fi');
    expect(html).toContain('Mulai');
    expect(clients).toHaveLength(1);
    owner.dispose();
  });
  it('renders one ordered tools region with a single selector and real wizard opener', () => {
    const owner = session('canonical-device'); const controller = new WifiSettingsController(owner);
    const html = renderToStaticMarkup(<DeviceDetailSessionView owner={owner} controller={controller} deviceCode="unit" bleDeviceId="canonical-device" sensorId="mq7" onSensorChange={vi.fn()} diagnostic={null} onDiagnosticRefresh={vi.fn()} />);
    expect(html).toContain('aria-label="Alat perangkat"');
    expect(html.indexOf('Konfigurasi Wi-Fi')).toBeLessThan(html.indexOf('Pilih sensor'));
    expect(html.indexOf('Pilih sensor')).toBeLessThan(html.indexOf('aria-label="Diagnostik sensor"'));
    expect(html.indexOf('aria-label="Diagnostik sensor"')).toBeLessThan(html.indexOf('aria-label="Kalibrasi"'));
    expect(html.match(/aria-label="Pilih sensor"/g)).toHaveLength(1);
    expect(html).toContain('aria-label="Kalibrasi mq7"');
    expect(html).toContain('aria-pressed="true"');
    controller.dispose(); owner.dispose();
  });
});
