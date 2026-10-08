import { useEffect, useMemo, useState } from 'react';
import { Card, Input, Text, XStack, YStack } from 'tamagui';

import { BleProvisioningClient } from '../../lib/ble/client';
import { isWebBluetoothAvailable, type WifiNetwork, type WifiStatus } from '../../lib/ble/protocol';
import { AppButton } from '../AppButton';

export type WifiSettingsDialogProps = { deviceId: string };

function stateLabel(status: WifiStatus | null): string {
  if (!status) return 'Belum terhubung';
  if (status.state === 'CONNECTED') return status.ssid ? `Terhubung ke ${status.ssid}` : 'Terhubung';
  if (status.state === 'SETUP_REQUIRED') return 'Perlu konfigurasi Wi-Fi';
  if (status.state === 'CONNECTING') return 'Menghubungkan';
  if (status.state === 'BACKOFF') return 'Menunggu percobaan berikutnya';
  return 'Belum terkonfigurasi';
}

export function WifiSettingsDialog({ deviceId }: WifiSettingsDialogProps) {
  const [open, setOpen] = useState(false);
  const [bleConnected, setBleConnected] = useState(false);
  const [bleDeviceName, setBleDeviceName] = useState<string | null>(null);
  const [status, setStatus] = useState<WifiStatus | null>(null);
  const [networks, setNetworks] = useState<WifiNetwork[]>([]);
  const [knownNetworks, setKnownNetworks] = useState<WifiNetwork[]>([]);
  const [selectedSsid, setSelectedSsid] = useState('');
  const [password, setPassword] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [success, setSuccess] = useState(false);
  const client = useMemo(() => new BleProvisioningClient({ onConnectionChange: setBleConnected, onDeviceIdentityChange: setBleDeviceName }), []);

  useEffect(() => () => client.close(), [client]);

  const connectBle = async () => {
    setError(null);
    setBusy(true);
    try {
      await client.connect();
      setBleConnected(true);
      setStatus(await client.status());
      setKnownNetworks(await client.listKnownWifi());
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : 'BLE tidak dapat terhubung');
      setBleConnected(false);
    } finally {
      setBusy(false);
    }
  };

  const scan = async () => {
    setError(null);
    setBusy(true);
    try { setNetworks(await client.scan()); }
    catch (cause) { setError(cause instanceof Error ? cause.message : 'Pemindaian Wi-Fi gagal'); }
    finally { setBusy(false); }
  };

  const connectWifi = async () => {
    setError(null);
    setSuccess(false);
    setBusy(true);
    try {
      setStatus(await client.connectWifi(selectedSsid, password));
      setPassword('');
      setSuccess(true);
    } catch (cause) { setError(cause instanceof Error ? cause.message : 'Koneksi Wi-Fi gagal'); }
    finally { setBusy(false); }
  };

  const retrySavedWifi = async () => {
    setError(null);
    setSuccess(false);
    setBusy(true);
    try {
      setStatus(await client.retryWifi());
      setStatus(await client.status());
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : 'Percobaan ulang Wi-Fi gagal');
    } finally {
      setBusy(false);
    }
  };

  return <>
    <AppButton variant="secondary" size="sm" onPress={() => { setOpen(true); setError(null); }} ariaLabel={`Atur Wi-Fi ${deviceId}`}>Atur Wi-Fi</AppButton>
    {open ? <Card role="dialog" aria-label="Pengaturan Wi-Fi" borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4" gap="$3">
      <XStack justifyContent="space-between" alignItems="center" gap="$3"><YStack flex={1} gap="$1"><Text color="$color" fontSize="$5" fontWeight="800">Pengaturan Wi-Fi</Text><Text color="$colorMuted" fontSize="$2">Perangkat {deviceId}</Text></YStack><AppButton variant="ghost" size="sm" onPress={() => setOpen(false)} ariaLabel="Tutup pengaturan Wi-Fi">Tutup</AppButton></XStack>
      {!isWebBluetoothAvailable() ? <Text role="alert" color="$statusWaspada">Web Bluetooth memerlukan HTTPS dan browser yang mendukungnya.</Text> : null}
      <Text color="$colorMuted" fontSize="$3">BLE: {bleConnected ? `terhubung${bleDeviceName ? ` (${bleDeviceName})` : ''}` : 'belum terhubung'}</Text>
      <Text color="$color" fontWeight="700">Wi-Fi: {stateLabel(status)}</Text>
      <XStack gap="$2" flexWrap="wrap"><AppButton size="sm" onPress={connectBle} disabled={busy || !isWebBluetoothAvailable()} ariaLabel="Hubungkan BLE">Hubungkan perangkat</AppButton><AppButton size="sm" variant="secondary" onPress={scan} disabled={busy || !bleConnected} ariaLabel="Pindai Wi-Fi">{busy ? 'Memproses...' : 'Pindai Wi-Fi'}</AppButton>{knownNetworks.length ? <AppButton size="sm" variant="ghost" onPress={retrySavedWifi} disabled={busy || !bleConnected} ariaLabel="Coba ulang Wi-Fi tersimpan">Coba ulang tersimpan</AppButton> : null}</XStack>
      {knownNetworks.length ? <YStack gap="$2"><Text color="$color" fontWeight="700">Jaringan tersimpan</Text>{knownNetworks.map((network) => <Text key={network.ssid} color="$colorMuted" fontSize="$2">{network.ssid} · {network.secure ? 'Aman' : 'Terbuka'}</Text>)}</YStack> : null}
      {networks.length ? <YStack gap="$2"><Text color="$color" fontWeight="700">Jaringan terdekat</Text>{networks.map((network) => <AppButton key={`${network.ssid}-${network.rssi}`} variant={selectedSsid === network.ssid ? 'secondary' : 'ghost'} size="sm" onPress={() => setSelectedSsid(network.ssid)} ariaLabel={`Pilih jaringan ${network.ssid}`}>{network.ssid} · {network.rssi} dBm · {network.secure ? 'Aman' : 'Terbuka'}</AppButton>)}</YStack> : null}
      {selectedSsid ? <YStack gap="$2"><Text color="$color" fontWeight="700">Jaringan dipilih: {selectedSsid}</Text><Input value={password} onChangeText={setPassword} placeholder="Password Wi-Fi" secureTextEntry aria-label="Password Wi-Fi" maxLength={63} autoCapitalize="none" /><AppButton onPress={connectWifi} disabled={busy || !bleConnected} ariaLabel="Simpan dan hubungkan Wi-Fi">Hubungkan Wi-Fi</AppButton></YStack> : null}
      {success ? <Text role="status" color="$statusNormal">Wi-Fi tersimpan setelah perangkat mengonfirmasi koneksi.</Text> : null}
      {error ? <YStack gap="$2"><Text role="alert" color="$statusSafetyAlert">{error}</Text><AppButton variant="ghost" size="sm" onPress={() => { setError(null); void connectBle(); }} ariaLabel="Coba lagi pengaturan Wi-Fi">Coba lagi</AppButton></YStack> : null}
    </Card> : null}
  </>;
}
