import { readFileSync } from 'node:fs';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { BleProvisioningClient, type BleProvisioningClientOptions } from './client';
import { assembleBleFrames, decodeBleFrame, decodeBleResponse, encodeBleFrames } from './protocol';
import { decodeDebugResponse, type DebugCursor, type DebugLevel } from '../debug/codec';

const fixtureText = readFileSync(new URL('../../contracts/debug/v1/fixtures/ble.txt', import.meta.url), 'ascii');
const fixtures = new Map(fixtureText.split('\n').filter(Boolean).map((line) => {
  const separator = line.indexOf('=');
  return [line.slice(0, separator), line.slice(separator + 1)];
}));
const fixture = (name: string): string => {
  const value = fixtures.get(name);
  if (value === undefined) throw new Error('Missing actual producer fixture');
  return value;
};
const boot = '00000000000000000000000000000001';
const identity = '0000000000000001';
const levels: DebugLevel[] = ['ERROR', 'WARN', 'INFO', 'DEBUG'];
type Listener = (event: { target: { value: DataView } }) => void;

class Peer {
  readonly commands: Array<{ id: string; command: string }> = [];
  private readonly listeners: Listener[] = [];
  private fragments: ReturnType<typeof decodeBleFrame>[] = [];
  respond: (command: string) => string | undefined = (command) => {
    if (command === 'GET_DEVICE_ID') return `OK DEVICE_ID id=${identity}`;
    if (command === 'GET_DEBUG META') return fixture('META');
    if (command.startsWith('GET_DEBUG PAGE ')) return fixture('PAGE');
    if (command.startsWith('SET_DEBUG_LEVEL ')) {
      const level = command.slice('SET_DEBUG_LEVEL '.length) as DebugLevel;
      return levels.includes(level) ? `DBG1|L|${levels.indexOf(level)}` : 'ERR INVALID';
    }
    return undefined;
  };
  async startNotifications(): Promise<Peer> { return this; }
  addEventListener(_name: string, listener: Listener): void { this.listeners.push(listener); }
  emit(id: string, text: string): void {
    for (const bytes of encodeBleFrames(id, new TextEncoder().encode(text), 'R1')) {
      const value = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      this.listeners.forEach((listener) => listener({ target: { value } }));
    }
  }
  async writeValue(value: BufferSource): Promise<void> {
    const bytes = value instanceof ArrayBuffer ? new Uint8Array(value) :
      new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    expect(bytes.byteLength).toBeLessThanOrEqual(20);
    const frame = decodeBleFrame(bytes);
    expect(frame.version).toBe('F1');
    this.fragments.push(frame);
    if (frame.chunkIndex !== frame.chunkCount - 1) {
      this.emit(frame.requestId, 'OK CHUNK');
      return;
    }
    const command = new TextDecoder().decode(assembleBleFrames(this.fragments, 'F1'));
    this.fragments = [];
    this.commands.push({ id: frame.requestId, command });
    const reply = this.respond(command);
    if (reply !== undefined) this.emit(frame.requestId, reply);
  }
}

const originalNavigator = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
const originalSecureContext = Object.getOwnPropertyDescriptor(globalThis, 'isSecureContext');
const clients: BleProvisioningClient[] = [];
afterEach(() => {
  clients.splice(0).forEach((client) => client.close());
  vi.restoreAllMocks();
  vi.useRealTimers();
  if (originalNavigator) Object.defineProperty(globalThis, 'navigator', originalNavigator);
  else Reflect.deleteProperty(globalThis, 'navigator');
  if (originalSecureContext) Object.defineProperty(globalThis, 'isSecureContext', originalSecureContext);
  else Reflect.deleteProperty(globalThis, 'isSecureContext');
});

async function connected(options: BleProvisioningClientOptions = {}) {
  const peer = new Peer();
  const server = { disconnect: vi.fn(), getPrimaryService: async () => ({ getCharacteristic: async () => peer }) };
  Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, value: true });
  Object.defineProperty(globalThis, 'navigator', { configurable: true, value: {
    bluetooth: { requestDevice: async () => ({ gatt: { connect: async () => server }, addEventListener: () => {} }) },
  } });
  const client = new BleProvisioningClient(options);
  clients.push(client);
  await client.connect();
  await client.verifyDeviceIdentity(identity);
  peer.commands.length = 0;
  return { client, peer };
}

describe('typed BLE debug client', () => {
  it('decodes a raw framed DBG1 response through the shared debug codec', () => {
    const actual = decodeBleResponse(new TextEncoder().encode('DBG1|L|2'));
    expect(actual.debug).toEqual(decodeDebugResponse('DBG1|L|2'));
  });

  it('returns actual producer META/PAGE types through F1 writes and R1 notifications', async () => {
    const { client, peer } = await connected();
    const meta = decodeDebugResponse(fixture('META'));
    const page = decodeDebugResponse(fixture('PAGE'));
    if (meta.kind !== 'meta' || page.kind !== 'page') throw new Error('Invalid producer fixtures');
    await expect(client.debugMeta()).resolves.toEqual(meta.meta);
    await expect(client.debugPage({ boot_id: boot, after: '1' })).resolves.toEqual(page.page);
    expect(peer.commands.map((value) => value.command)).toEqual([
      'GET_DEBUG META', `GET_DEBUG PAGE ${boot} 1`,
    ]);
    expect(page.page.event?.monotonic_us).toBe('9007199254740993');
  });

  it('sends literal level words and returns the observed level, not optimistic state', async () => {
    const { client, peer } = await connected();
    for (const level of levels) await expect(client.setDebugLevel(level)).resolves.toBe(level);
    expect(peer.commands.map((value) => value.command)).toEqual(levels.map((level) => `SET_DEBUG_LEVEL ${level}`));
    peer.respond = () => 'DBG1|L|1';
    await expect(client.setDebugLevel('DEBUG')).resolves.toBe('WARN');
  });

  it.each([
    ['meta', (client: BleProvisioningClient) => client.debugMeta(), 'DBG1|L|2'],
    ['page', (client: BleProvisioningClient) => client.debugPage({ boot_id: boot, after: '0' }), 'DBG1|L|2'],
    ['level', (client: BleProvisioningClient) => client.setDebugLevel('INFO'), fixture('META')],
  ] as const)('rejects a valid wrong-kind %s response without waiting for timeout', async (_name, invoke, reply) => {
    vi.useFakeTimers();
    const { client, peer } = await connected();
    peer.respond = () => reply;
    await expect(invoke(client)).rejects.toThrow(/Respons debug/u);
    expect(peer.commands).toHaveLength(1);
    expect(vi.getTimerCount()).toBe(0);
  });

  it.each([
    { boot_id: boot, after: '01' }, { boot_id: boot, after: '+1' },
    { boot_id: boot, after: '-1' }, { boot_id: boot, after: ' 1' },
    { boot_id: boot, after: '18446744073709551616' },
    { boot_id: 'ABCDEF0123456789abcdef0123456789', after: '0' },
    { boot_id: boot, after: 0 }, { boot_id: 123, after: '0' }, null,
  ])('rejects noncanonical or non-string cursor %j before any write', async (cursor) => {
    const { client, peer } = await connected();
    await expect(client.debugPage(cursor as unknown as DebugCursor)).rejects.toThrow(/Cursor debug/u);
    expect(peer.commands).toHaveLength(0);
  });

  it('preserves the full uint64 cursor token and present-zero values', async () => {
    const { client, peer } = await connected();
    peer.respond = () => `DBG1|P|${boot}|2|0|18446744073709551615|0|0|0|-`;
    await expect(client.debugPage({ boot_id: boot, after: '18446744073709551615' }))
      .resolves.toMatchObject({ next_after: '18446744073709551615', event: null });
    expect(peer.commands[0].command).toBe(`GET_DEBUG PAGE ${boot} 18446744073709551615`);
    peer.respond = () => fixture('PAGE_ZERO');
    await expect(client.debugPage({ boot_id: boot, after: '0' })).resolves.toMatchObject({
      event: { request_id: 0, related_sequence: '0' },
    });
  });

  it('rejects an invalid level before writing and propagates an explicit device error', async () => {
    const { client, peer } = await connected();
    await expect(client.setDebugLevel('TRACE' as DebugLevel)).rejects.toThrow(/Level debug/u);
    expect(peer.commands).toHaveLength(0);
    peer.respond = () => 'ERR DEBUG_UNAVAILABLE';
    await expect(client.debugMeta()).rejects.toThrow('DEBUG_UNAVAILABLE');
  });

  it('retains exact debug replies across request IDs 9 and 10', async () => {
    const { client, peer } = await connected();
    for (let index = 0; index < 7; index += 1) await client.debugMeta();
    await client.debugMeta();
    await client.debugPage({ boot_id: boot, after: '1' });
    expect(peer.commands.slice(-2).map((value) => value.id)).toEqual(['9', '10']);
  });

  it.each(['\uFEFFDBG1|L|2', 'DBG1|L|2\n', 'DBG1|L|2\u0000', 'DBG1|L|02'])
    ('does not sanitize malformed debug bytes %j into success', (body) => {
      expect(() => decodeBleResponse(new TextEncoder().encode(body))).toThrow();
    });
});

describe('per-command deadlines without mutation retries', () => {
  it.each([0, -1, Number.NaN, Number.POSITIVE_INFINITY])('rejects invalid positive-finite timeout %s', (timeout) => {
    expect(() => new BleProvisioningClient({ commandTimeoutMs: { GET_DEBUG: timeout } })).toThrow(/timeout/u);
  });

  it('rejects a timeout override for commands outside the explicit allowlist', () => {
    expect(() => new BleProvisioningClient({ commandTimeoutMs: { CONNECT_WIFI: 30_000 } as never }))
      .toThrow(/timeout/u);
  });

  it.each(['meta', 'page'] as const)('copies GET_DEBUG options and waits exactly 30s once for %s', async (operation) => {
    vi.useFakeTimers();
    const timeouts = { GET_DEBUG: 30_000 };
    const events: unknown[] = [];
    const { client, peer } = await connected({ requestTimeoutMs: 7, readOnlyMaxAttempts: 3,
      commandTimeoutMs: timeouts, onDiagnosticEvent: (event) => events.push(event) });
    timeouts.GET_DEBUG = 1;
    peer.respond = () => undefined;
    let settled = false;
    const result = (operation === 'meta' ? client.debugMeta() : client.debugPage({ boot_id: boot, after: '0' }))
      .catch((error: unknown) => { settled = true; return error; });
    await vi.advanceTimersByTimeAsync(29_999);
    expect(settled).toBe(false);
    expect(peer.commands).toHaveLength(1);
    await vi.advanceTimersByTimeAsync(1);
    expect(await result).toBeInstanceOf(Error);
    expect(peer.commands).toHaveLength(1);
    expect(events).toContainEqual(expect.objectContaining({
      type: 'request_timeout', operation: 'GET_DEBUG', elapsedMs: 30_000, attempt: 1, maxAttempts: 1,
    }));
  });

  it.each(['GET_DEVICE_ID', 'GET_STATUS', 'SCAN_WIFI'] as const)('applies 30s x 3 only to %s', async (operation) => {
    vi.useFakeTimers();
    const { client, peer } = await connected({ readOnlyMaxAttempts: 3,
      commandTimeoutMs: { [operation]: 30_000 } });
    peer.respond = () => undefined;
    const pending = operation === 'GET_DEVICE_ID' ? client.verifyDeviceIdentity(identity) :
      operation === 'GET_STATUS' ? client.status() : client.scan();
    const result = pending.catch((error: unknown) => error);
    await vi.advanceTimersByTimeAsync(90_000);
    expect(await result).toBeInstanceOf(Error);
    expect(peer.commands.map((value) => value.command)).toEqual([operation, operation, operation]);
    expect(new Set(peer.commands.map((value) => value.id)).size).toBe(3);
    expect(vi.getTimerCount()).toBe(0);
  });

  it.each(['meta', 'level', 'calibration', 'calibration_commit', 'connect'] as const)('retains the default 5s one-send deadline for %s', async (operation) => {
    vi.useFakeTimers();
    const { client, peer } = await connected({ readOnlyMaxAttempts: 3,
      commandTimeoutMs: { GET_STATUS: 30_000 } });
    peer.respond = () => undefined;
    const pending = operation === 'meta' ? client.debugMeta() : operation === 'level' ? client.setDebugLevel('DEBUG') :
      operation === 'calibration' ? client.calibrationStatus('hx711_organic') :
      operation === 'calibration_commit' ? client.calibrationCommit('hx711_organic') : client.connectWifi('synthetic-network', '');
    let settled = false;
    const result = pending.catch((error: unknown) => { settled = true; return error; });
    await vi.advanceTimersByTimeAsync(4_999);
    expect(settled).toBe(false);
    await vi.advanceTimersByTimeAsync(1);
    expect(await result).toBeInstanceOf(Error);
    expect(peer.commands).toHaveLength(1);
    await vi.advanceTimersByTimeAsync(60_000);
    expect(peer.commands).toHaveLength(1);
  });

  it.each(['write_failure', 'device_rejected', 'decoder_rejected'] as const)('does not retry a read after %s', async (failure) => {
    vi.useFakeTimers();
    const { client, peer } = await connected({ requestTimeoutMs: 10, readOnlyMaxAttempts: 3 });
    const write = vi.spyOn(peer, 'writeValue');
    if (failure === 'write_failure') write.mockRejectedValue(new Error('Synthetic GATT failure'));
    peer.respond = () => failure === 'device_rejected' ? 'ERR WIFI_UNAVAILABLE' : 'OK STATUS state=INVALID ssid=';
    const result = client.status().catch((error: unknown) => error);
    await vi.advanceTimersByTimeAsync(100);
    expect(await result).toBeInstanceOf(Error);
    expect(write).toHaveBeenCalledTimes(1);
    expect(peer.commands).toHaveLength(failure === 'write_failure' ? 0 : 1);
    expect(vi.getTimerCount()).toBe(0);
  });

  it('bounds an unfinished write without resending or allowing its late continuation', async () => {
    vi.useFakeTimers();
    const { client, peer } = await connected({ requestTimeoutMs: 10, readOnlyMaxAttempts: 3 });
    let release!: () => void;
    const write = vi.spyOn(peer, 'writeValue').mockImplementation(() => new Promise<void>((resolve) => { release = resolve; }));
    const result = client.verifyDeviceIdentity(identity).catch((error: unknown) => error);
    await vi.advanceTimersByTimeAsync(100);
    expect(await result).toBeInstanceOf(Error);
    expect(write).toHaveBeenCalledTimes(1);
    expect(decodeBleFrame(write.mock.calls[0][0] as Uint8Array).chunkCount).toBeGreaterThan(1);
    release();
    await vi.advanceTimersByTimeAsync(100);
    expect(write).toHaveBeenCalledTimes(1);
    expect(peer.commands).toHaveLength(0);
  });

  it('isolates a cancelled debug request and stale response from a reconnected session', async () => {
    vi.useFakeTimers();
    const { client, peer } = await connected();
    const respond = peer.respond;
    peer.respond = () => undefined;
    const old = client.debugMeta().catch((error: unknown) => error);
    await vi.advanceTimersByTimeAsync(0);
    const oldId = peer.commands[0].id;
    client.close();
    expect(await old).toBeInstanceOf(Error);
    peer.respond = respond;
    await client.connect();
    await client.verifyDeviceIdentity(identity);
    peer.commands.length = 0;
    peer.respond = () => undefined;
    let completed = false;
    const current = client.debugMeta().then((value) => { completed = true; return value; });
    await vi.advanceTimersByTimeAsync(0);
    peer.emit(oldId, fixture('META'));
    await vi.advanceTimersByTimeAsync(0);
    expect(completed).toBe(false);
    peer.emit(peer.commands[0].id, fixture('META'));
    await expect(current).resolves.toMatchObject({ build_id: 'TEST' });
    expect(vi.getTimerCount()).toBe(0);
  });

  it('does not delay successful debug responses or leak request contents into diagnostics', async () => {
    vi.useFakeTimers();
    const events: unknown[] = [];
    const { client } = await connected({ commandTimeoutMs: { GET_DEBUG: 30_000 },
      onDiagnosticEvent: (event) => events.push(event) });
    const before = Date.now();
    await expect(client.debugMeta()).resolves.toMatchObject({ build_id: 'TEST' });
    expect(Date.now()).toBe(before);
    expect(JSON.stringify(events)).not.toContain('DBG1|');
    expect(JSON.stringify(events)).not.toContain(boot);
    expect(vi.getTimerCount()).toBe(0);
  });
});
