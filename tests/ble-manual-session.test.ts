import { describe, expect, it, vi } from 'vitest';
import { ManualBleSession, type ManualDevice } from '../scripts/ble-manual/session';
import { assembleBleFrames, decodeBleFrame, encodeBleFrames, SIGAP_BLE_EVENTS_UUID, SIGAP_BLE_SERVICE_UUID } from '../lib/ble/protocol';

const expectedId = '000050c1f516a398';
const diagnosticStatus = 'OK STATUS mode=BLE_DIAGNOSTIC wifi=DISABLED';

function harness(
  identity = expectedId,
  status = diagnosticStatus,
  afterResponse: (command: string) => void = () => {},
  dropTerminal = false,
) {
  let notify: (event: { target: { value: DataView } }) => void = () => {};
  let disconnected = () => {};
  let received: ReturnType<typeof decodeBleFrame>[] = [];
  const commands: string[] = [];
  const characteristic = {
    writeValue: vi.fn(async (value: BufferSource) => {
      const frame = decodeBleFrame(new Uint8Array(value as ArrayBuffer));
      received.push(frame);
      if (frame.chunkIndex < frame.chunkCount - 1) {
        const acknowledgement = encodeBleFrames(frame.requestId, new TextEncoder().encode('OK CHUNK'), 'R1')[0];
        for (let count = 0; count < 2; count += 1) {
          notify({ target: { value: new DataView(acknowledgement.buffer, acknowledgement.byteOffset, acknowledgement.byteLength) } });
        }
      }
      if (received.length !== frame.chunkCount) return;
      const command = new TextDecoder().decode(assembleBleFrames(received, 'F1'));
      commands.push(command);
      received = [];
      if (dropTerminal) return;
      const response = command === 'GET_DEVICE_ID' ? `OK DEVICE_ID id=${identity}` : status;
      for (const part of encodeBleFrames(frame.requestId, new TextEncoder().encode(response), 'R1')) {
        notify({ target: { value: new DataView(part.buffer, part.byteOffset, part.byteLength) } });
      }
      afterResponse(command);
    }),
    startNotifications: vi.fn(async () => characteristic),
    addEventListener: vi.fn((_type: string, listener: typeof notify) => { notify = listener; }),
  };
  const server = {
    getPrimaryService: vi.fn(async (_uuid: string) => ({ getCharacteristic: vi.fn(async (_characteristic: string) => characteristic) })),
    disconnect: vi.fn(() => disconnected()),
  };
  const device: ManualDevice = {
    name: 'SIGAP-A398',
    gatt: { connect: vi.fn(async () => server) },
    addEventListener: vi.fn((_type, listener) => { disconnected = listener; }),
  };
  const requestDevice = vi.fn(async () => device);
  return { client: new ManualBleSession({ requestDevice }), requestDevice, device, commands, characteristic, server, disconnected: () => disconnected() };
}

describe('manual BLE environment', () => {
  it('does not finish a connection after an explicit manual disconnect', async () => {
    const h = harness();
    let complete: ((server: typeof h.server) => void) | undefined;
    h.device.gatt!.connect = vi.fn(() => new Promise<typeof h.server>((resolve) => { complete = resolve; }));
    const choosing = h.client.choose();
    await Promise.resolve();
    h.client.disconnect();
    expect(complete).toBeDefined();
    complete!(h.server);
    await expect(choosing).rejects.toThrow('CANCELLED');
    expect(h.client.state.connected).toBe(false);
    expect(h.server.getPrimaryService).not.toHaveBeenCalled();
  });

  it('never scans or connects when the page creates a session', () => {
    const h = harness();
    expect(h.requestDevice).not.toHaveBeenCalled();
    expect(h.client.state).toMatchObject({ connected: false, verifiedId: null, status: null });
    expect(h.commands).toEqual([]);
  });

  it('does not restore identity from a reply after disconnect', async () => {
    let disconnected = () => {};
    const h = harness(expectedId, diagnosticStatus, () => disconnected());
    disconnected = h.disconnected;
    await h.client.choose();
    await expect(h.client.verifyIdentity()).rejects.toThrow('DISCONNECTED');
    expect(h.client.state.verifiedId).toBeNull();
  });

  it('does not restore diagnostic status from a reply after disconnect', async () => {
    let disconnected = () => {};
    const h = harness(expectedId, diagnosticStatus, (command) => {
      if (command === 'GET_STATUS') disconnected();
    });
    disconnected = h.disconnected;
    await h.client.choose();
    await h.client.verifyIdentity();
    await expect(h.client.readStatus()).rejects.toThrow('DISCONNECTED');
    expect(h.client.state.status).toBeNull();
  });

  it('requires identity verification before diagnostic status', async () => {
    const h = harness();
    await expect(h.client.readStatus()).rejects.toThrow('IDENTITY_REQUIRED');
    await h.client.choose();
    await expect(h.client.readStatus()).rejects.toThrow('IDENTITY_REQUIRED');
    expect(h.commands).toEqual([]);
  });

  it('uses the safe name filter and only reads after explicit actions', async () => {
    const h = harness();
    await h.client.choose();
    expect(h.requestDevice).toHaveBeenCalledWith({ filters: [{ namePrefix: 'SIGAP-' }], optionalServices: [SIGAP_BLE_SERVICE_UUID] });
    expect(h.server.getPrimaryService).toHaveBeenCalledWith(SIGAP_BLE_SERVICE_UUID);
    expect(h.commands).toEqual([]);
    await h.client.verifyIdentity();
    expect(h.commands).toEqual(['GET_DEVICE_ID']);
    Object.defineProperty(h.client, 'nextId', { configurable: true, value: 1000, writable: true });
    expect(await h.client.readStatus()).toBe(diagnosticStatus);
    expect(h.commands).toEqual(['GET_DEVICE_ID', 'GET_STATUS']);
    expect(h.characteristic.writeValue).toHaveBeenCalledTimes(4);
  });

  it('keeps the original response deadline across repeated chunk acknowledgements', async () => {
    vi.useFakeTimers();
    try {
      const h = harness(expectedId, diagnosticStatus, () => {}, true);
      await h.client.choose();
      const verification = h.client.verifyIdentity();
      await Promise.resolve();
      await Promise.resolve();
      expect(h.characteristic.writeValue).toHaveBeenCalledTimes(2);
      const timedOut = expect(verification).rejects.toThrow('RESPONSE_TIMEOUT');
      await vi.advanceTimersByTimeAsync(5000);
      await timedOut;
      expect(h.client.state.connected).toBe(true);
      expect(h.characteristic.writeValue).toHaveBeenCalledTimes(2);
    } finally {
      vi.useRealTimers();
    }
  });

  it('rejects an outstanding acknowledged request when disconnected', async () => {
    const h = harness(expectedId, diagnosticStatus, () => {}, true);
    await h.client.choose();
    const verification = h.client.verifyIdentity();
    await Promise.resolve();
    await Promise.resolve();
    expect(h.characteristic.writeValue).toHaveBeenCalledTimes(2);
    h.disconnected();
    await expect(verification).rejects.toThrow('DISCONNECTED');
    expect(h.client.state).toMatchObject({ connected: false, verifiedId: null, status: null });
  });

  it('clears identity and diagnostic status on a real disconnect event', async () => {
    const h = harness();
    await h.client.choose();
    await h.client.verifyIdentity();
    await h.client.readStatus();
    h.disconnected();
    expect(h.client.state).toMatchObject({ connected: false, verifiedId: null, status: null });
  });

  it('reconnects only on request and requires fresh identity verification', async () => {
    const h = harness();
    await h.client.choose();
    await h.client.verifyIdentity();
    h.client.disconnect();
    expect(h.requestDevice).toHaveBeenCalledTimes(1);
    await h.client.reconnect();
    expect(h.requestDevice).toHaveBeenCalledTimes(1);
    expect(h.client.state.verifiedId).toBeNull();
    await expect(h.client.readStatus()).rejects.toThrow('IDENTITY_REQUIRED');
    expect(h.commands).toEqual(['GET_DEVICE_ID']);
  });

  it('rejects another device identity without allowing a status read', async () => {
    const h = harness('0000000000000001');
    await h.client.choose();
    await expect(h.client.verifyIdentity()).rejects.toThrow('IDENTITY_MISMATCH');
    expect(h.client.state.verifiedId).toBeNull();
    expect(h.server.disconnect).toHaveBeenCalled();
    await expect(h.client.readStatus()).rejects.toThrow('IDENTITY_REQUIRED');
  });

  it('does not retain an arbitrary response as diagnostic status', async () => {
    const h = harness(expectedId, 'untrusted response content');
    await h.client.choose();
    await h.client.verifyIdentity();
    await expect(h.client.readStatus()).rejects.toThrow('UNEXPECTED_DIAGNOSTIC_STATUS');
    expect(JSON.stringify(h.client.state)).not.toContain('untrusted response content');
    expect(h.client.state.status).toBeNull();
  });
});
