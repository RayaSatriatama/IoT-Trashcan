import { useSyncExternalStore } from 'react';
import { Card, Input, ScrollView, Text, XStack, YStack } from 'tamagui';

import type { DeviceBleSession } from '../../lib/ble/device-session';
import type { WifiSettingsController } from '../../lib/ble/wifi-settings';
import { isWebBluetoothAvailable, type WifiStatus } from '../../lib/ble/protocol';
import { AppButton } from '../AppButton';
import { DescriptionHint } from '../SectionHeader';
import { textStyles } from '../../lib/ui/typography';

export type WifiSettingsDialogProps = { deviceId: string; bleDeviceId?: string | null; session: DeviceBleSession; controller: WifiSettingsController; hideTrigger?: boolean };

function stateLabel(status: WifiStatus | null): string {
  if (!status) return 'Belum terhubung';
  if (status.state === 'CONNECTED') return status.ssid ? `Terhubung ke ${status.ssid}` : 'Terhubung';
  if (status.state === 'SETUP_REQUIRED') return 'Perlu konfigurasi Wi-Fi';
  if (status.state === 'CONNECTING') return 'Menghubungkan';
  if (status.state === 'BACKOFF') return 'Menunggu percobaan berikutnya';
  return 'Belum terkonfigurasi';
}

export function WifiSettingsDialog({ deviceId, bleDeviceId = null, session, controller, hideTrigger = false }: WifiSettingsDialogProps) {
  const view = useSyncExternalStore(controller.subscribe, controller.getSnapshot, controller.getSnapshot);
  const peer = useSyncExternalStore(session.subscribe, session.getSnapshot, session.getSnapshot);
  const open = view.open, busy = view.busy, status = view.status, error = view.error;
  const selectedSsid = view.selectedSsid, password = view.password;
  const knownNetworks = view.saved, networks = view.nearby;
  const bleConnected = peer.connected, verifiedBleDeviceId = peer.deviceId;
  const identityMatches = !!bleDeviceId && verifiedBleDeviceId === bleDeviceId && bleConnected;
  const actionBusy = busy || peer.busy !== null || peer.controlActive;
  const canDisconnect = bleConnected && !peer.controlActive &&
    (peer.busy === null || peer.busy === 'debug' || (peer.busy === 'wifi' && view.readonlyBusy));
  const connectBle = async () => {
    if (actionBusy || identityMatches) return;
    try {
      await controller.connectBle();
      await controller.loadStatus();
      await controller.loadSaved();
    } catch { /* The controller snapshot renders the connection error. */ }
  };
  const scan = () => { if (!actionBusy && identityMatches) void controller.scan(); };
  const connectWifi = () => { if (!actionBusy && identityMatches) void controller.submit(); };
  const retrySavedWifi = () => { if (!actionBusy && identityMatches) void controller.retrySaved(); };

  return <>
    {!hideTrigger ? <AppButton variant="secondary" size="sm" onPress={() => controller.open()} ariaLabel={`Atur Wi-Fi ${deviceId}`}>Atur Wi-Fi</AppButton> : null}
    {open ? <Card role="dialog" aria-label="Pengaturan Wi-Fi" borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3">
      <XStack justifyContent="space-between" alignItems="center" gap="$2" flexWrap="wrap"><XStack alignItems="center" flex={1} minWidth={180}><Text color="$color" {...textStyles.sectionTitle}>Pengaturan Wi-Fi</Text><DescriptionHint description={`Perangkat ${deviceId}. BLE memakai pencocokan identitas tanpa autentikasi; otorisasi API terpisah. Koneksi Wi-Fi berbeda dari BLE. Password tidak dicatat dalam trace.`} ariaLabel="Info pengaturan Wi-Fi" /></XStack><AppButton variant="ghost" size="sm" onPress={() => controller.close()} ariaLabel="Tutup pengaturan Wi-Fi">Tutup</AppButton></XStack>
      {!isWebBluetoothAvailable() ? <Text role="alert" color="$statusWaspada">Web Bluetooth memerlukan HTTPS dan browser yang mendukungnya.</Text> : null}
      <XStack gap="$2" flexWrap="wrap">
        <YStack flex={1} minWidth={130} padding="$3" backgroundColor="$background" borderRadius="$3" gap="$1"><Text color="$colorMuted" {...textStyles.caption}>BLE</Text><Text color="$color" {...textStyles.cardTitle}>{bleConnected ? 'Terhubung' : 'Belum terhubung'}</Text></YStack>
        <YStack flex={1} minWidth={130} padding="$3" backgroundColor="$background" borderRadius="$3" gap="$1"><Text color="$colorMuted" {...textStyles.caption}>Identitas perangkat</Text><Text color="$color" {...textStyles.cardTitle}>{identityMatches ? 'Identitas cocok' : 'Belum dicocokkan'}</Text></YStack>
        <YStack flex={1} minWidth={130} padding="$3" backgroundColor="$background" borderRadius="$3" gap="$1"><Text color="$colorMuted" {...textStyles.caption}>Wi-Fi</Text><Text color="$color" {...textStyles.cardTitle}>{stateLabel(status)}</Text></YStack>
      </XStack>
      {actionBusy ? <Text role="status" color="$colorMuted">Menunggu operasi perangkat selesai...</Text> : null}
      <XStack gap="$2" flexWrap="wrap">{!identityMatches ? <AppButton size="sm" onPress={connectBle} disabled={actionBusy || !isWebBluetoothAvailable() || !bleDeviceId} ariaLabel="Hubungkan BLE">Hubungkan perangkat</AppButton> : null}<AppButton size="sm" variant={identityMatches ? 'primary' : 'secondary'} onPress={scan} disabled={actionBusy || !identityMatches} ariaLabel="Pindai Wi-Fi">{busy ? 'Memproses...' : 'Pindai Wi-Fi'}</AppButton>{knownNetworks.length ? <AppButton size="sm" variant="ghost" onPress={retrySavedWifi} disabled={actionBusy || !identityMatches} ariaLabel="Coba ulang Wi-Fi tersimpan">Coba ulang tersimpan</AppButton> : null}<AppButton size="sm" variant="ghost" onPress={() => { if (canDisconnect) session.close(); }} disabled={!canDisconnect} ariaLabel="Putuskan BLE bersama">Putuskan BLE</AppButton></XStack>
      {knownNetworks.length ? <YStack gap="$2"><Text color="$color" fontWeight="700">Jaringan tersimpan</Text>{knownNetworks.map((network) => <Text key={network.ssid} color="$colorMuted" fontSize="$2">{network.ssid} · {network.secure ? 'Aman' : 'Terbuka'}</Text>)}</YStack> : null}
      {view.rawCount > 0 ? <Text role="status" color="$colorMuted" fontSize="$2">{view.rawCount} jaringan terdeteksi · {networks.length} dapat dipilih</Text> : null}
      {networks.length ? <YStack gap="$2"><Text color="$color" {...textStyles.cardTitle}>Jaringan terdekat</Text><ScrollView maxHeight={260} overflowY="auto" borderWidth={1} borderColor="$borderColor" borderRadius="$4" padding="$2" aria-label="Jaringan Wi-Fi tersedia"><YStack gap="$2">{networks.map((network) => <AppButton key={`${network.ssid}-${network.rssi}`} fullWidth disabled={actionBusy} variant={selectedSsid === network.ssid ? 'secondary' : 'ghost'} size="sm" onPress={() => { if (!actionBusy) controller.selectSsid(network.ssid); }} ariaLabel={`Pilih jaringan ${network.ssid}`}><Text width="100%" minWidth={0} flexShrink={1} flexWrap="wrap">{network.ssid} · {network.rssi} dBm · {network.secure ? 'Aman' : 'Terbuka'}{selectedSsid === network.ssid ? ' · Dipilih' : ''}</Text></AppButton>)}</YStack></ScrollView></YStack> : null}
      {selectedSsid ? <YStack gap="$2"><Text color="$color" {...textStyles.cardTitle}>Jaringan dipilih: {selectedSsid}</Text><Text color="$color" {...textStyles.caption}>Password Wi-Fi</Text><Input value={password} onChangeText={(value) => controller.setPassword(value)} placeholder="Masukkan password" type="password" secureTextEntry aria-label="Password Wi-Fi" minHeight={44} fontSize={16} borderColor="$borderColor" backgroundColor="$background" maxLength={63} autoCapitalize="none" /><AppButton onPress={connectWifi} disabled={actionBusy || !identityMatches} ariaLabel="Simpan dan hubungkan Wi-Fi">Hubungkan Wi-Fi</AppButton></YStack> : null}
      {view.success ? <Text role="status" color="$statusNormal">Wi-Fi tersimpan setelah perangkat mengonfirmasi koneksi.</Text> : null}
      {error ? <YStack gap="$2"><Text role="alert" color="$statusSafetyAlert">{error}</Text><AppButton variant="ghost" size="sm" disabled={actionBusy} onPress={() => { if (identityMatches && !actionBusy) void controller.loadStatus(); else void connectBle(); }} ariaLabel="Coba lagi pengaturan Wi-Fi">Coba lagi</AppButton></YStack> : null}
    </Card> : null}
  </>;
}
