import { describe, expect, it } from 'vitest';
import mqg2Fixture from '../../contracts/calibration/mq135-nh3/v2/fixtures/in-range.json';
import {
  assembleBleFrames,
  BLE_ATT_FRAME_BYTES,
  BLE_CHUNK_ACK,
  BLE_FRAME_MAX_BYTES,
  decodeBleFrame,
  decodeBleResponse,
  encodeBleCommand,
  encodeCalibrationCommand,
  encodeBleFrames,
  isBleChunkAcknowledgement,
  isWebBluetoothAvailable,
} from './protocol';

describe('BLE provisioning protocol', () => {
  it('encodes bounded commands without retaining credentials', () => {
    const bytes = encodeBleCommand('1', 'CONNECT', { ssid: 'Lab', password: 'secret' });
    expect(new TextDecoder().decode(bytes)).toBe('CONNECT_WIFI Lab secret\n');
    expect(bytes.byteLength).toBeLessThanOrEqual(BLE_FRAME_MAX_BYTES);
  });

  it('frames requests within the ATT payload and preserves UTF-8 payload bytes', () => {
    const logical = encodeBleCommand('ignored', 'CONNECT', { ssid: 'Lab', password: 'x'.repeat(64) });
    const frames = encodeBleFrames('7', logical);
    expect(frames.length).toBeGreaterThan(1);
    expect(frames.every((frame) => frame.byteLength <= BLE_ATT_FRAME_BYTES)).toBe(true);
    const parsed = frames.map(decodeBleFrame);
    expect(parsed.every((frame) => frame.requestId === '7')).toBe(true);
    expect(new TextDecoder().decode(assembleBleFrames(parsed, 'F1'))).toBe(`CONNECT_WIFI Lab ${'x'.repeat(64)}\n`);
  });

  it('preserves a UTF-8 codepoint split across ATT frames', () => {
    const logical = new TextEncoder().encode(`CONNECT_WIFI Cafe\u0301 ${'x'.repeat(40)}\n`);
    const frames = encodeBleFrames('8', logical).map(decodeBleFrame);
    expect(assembleBleFrames(frames, 'F1')).toEqual(logical);
  });

  it('rejects reordered, duplicate, or mismatched response chunks', () => {
    const logical = new TextEncoder().encode(`OK SCAN READY\n${'a'.repeat(80)}`);
    const parsed = encodeBleFrames('1', logical, 'R1').map(decodeBleFrame);
    expect(assembleBleFrames(parsed, 'R1')).toEqual(logical);
    expect(() => assembleBleFrames([{ ...parsed[0], requestId: '2' }, ...parsed.slice(1)])).toThrow();
    expect(() => assembleBleFrames([parsed[1], parsed[1], ...parsed.slice(2)])).toThrow();
  });

  it('enforces firmware F1/R1 chunk counts, index relations, and decimal tokens', () => {
    for (const version of ['F1', 'R1'] as const) {
      for (const count of [127, 128, 511, 512]) {
        expect(decodeBleFrame(new TextEncoder().encode(`${version}|9|${count - 1}|${count}|x`)))
          .toMatchObject({ version, chunkIndex: count - 1, chunkCount: count });
      }
      for (const envelope of [
        `${version}|9|0|0|x`, `${version}|9|0|513|x`,
        `${version}|9|1|1|x`, `${version}|9|128|128|x`,
        `${version}|9||1|x`, `${version}|9|+0|1|x`, `${version}|9|0|+1|x`,
        `${version}|9|0.0|1|x`, `${version}|9|0|1e0|x`, `${version}|9|0|0x1|x`,
        `${version}|9| |1|x`, `${version}|9|0| |x`,
      ]) expect(() => decodeBleFrame(new TextEncoder().encode(envelope))).toThrow();
      expect(decodeBleFrame(new TextEncoder().encode(`${version}|9|000|001|x`)))
        .toMatchObject({ chunkIndex: 0, chunkCount: 1 });
    }
  });

  it('matches firmware uint32 request IDs across F1/R1 encode, decode, and assembly', () => {
    for (const version of ['F1', 'R1'] as const) {
      for (const id of ['0', '9', '10', '0000000009', '4294967295']) {
        const frames = encodeBleFrames(id, new Uint8Array([0x41]), version);
        expect(decodeBleFrame(frames[0]).requestId).toBe(id);
        expect(assembleBleFrames(frames.map(decodeBleFrame), version)).toEqual(new Uint8Array([0x41]));
      }
      for (const id of ['4294967296', '9999999999']) {
        expect(() => encodeBleFrames(id, new Uint8Array([0x41]), version)).toThrow('BLE request id invalid');
        expect(() => decodeBleFrame(new TextEncoder().encode(`${version}|${id}|0|1|A`)))
          .toThrow('BLE request id invalid');
        expect(() => assembleBleFrames([{
          version, requestId: id, chunkIndex: 0, chunkCount: 1, payload: new Uint8Array([0x41]),
        }], version)).toThrow('BLE request id invalid');
      }
    }
  });

  it('rejects unsupported split counts while preserving supported ID widths and body bounds', () => {
    for (const id of ['9', '10', '123456789', '1234567890']) {
      for (const version of ['F1', 'R1'] as const) {
        for (const size of [0, 1, 18, 19, 20, 127, 128, 500]) {
          const body = new Uint8Array(size);
          let frames: Uint8Array[];
          try {
            frames = encodeBleFrames(id, body, version);
          } catch {
            continue;
          }
          expect(frames.length).toBeGreaterThanOrEqual(1);
          expect(frames.length).toBeLessThanOrEqual(128);
          expect(frames.every((frame) => frame.byteLength <= BLE_ATT_FRAME_BYTES)).toBe(true);
          expect(assembleBleFrames(frames.map(decodeBleFrame), version)).toEqual(body);
        }
        if (id.length >= 9) expect(() => encodeBleFrames(id, new Uint8Array(512), version)).toThrow();
        else {
          const body = new Uint8Array(512);
          const frames = encodeBleFrames(id, body, version);
          expect(frames.length).toBeLessThanOrEqual(128);
          expect(assembleBleFrames(frames.map(decodeBleFrame), version)).toEqual(body);
        }
      }
    }
  });

  it('rejects direct assembly above the declared or actual frame bounds before allocation', () => {
    const oversizedCount = Array.from({ length: 513 }, (_, chunkIndex) => ({
      version: 'R1' as const, requestId: '9', chunkIndex, chunkCount: 513, payload: new Uint8Array([1]),
    }));
    expect(() => assembleBleFrames([{
      version: 'R1', requestId: '9', chunkIndex: 0, chunkCount: 513, payload: new Uint8Array([1]),
    }])).toThrow();
    expect(() => assembleBleFrames(oversizedCount)).toThrow();
    expect(() => assembleBleFrames([{
      version: 'R1', requestId: '9', chunkIndex: 0, chunkCount: 1, payload: new Uint8Array(BLE_FRAME_MAX_BYTES + 1),
    }])).toThrow();
  });

  it('recognizes only a single-frame R1 chunk acknowledgement without making it terminal data', () => {
    const bytes = new TextEncoder().encode(BLE_CHUNK_ACK);
    const acknowledgement = decodeBleFrame(encodeBleFrames('12', bytes, 'R1')[0]);
    expect(isBleChunkAcknowledgement(acknowledgement)).toBe(true);
    expect(() => decodeBleResponse(bytes)).toThrow('BLE response envelope is invalid');
    expect(isBleChunkAcknowledgement(decodeBleFrame(encodeBleFrames('12', bytes, 'F1')[0]))).toBe(false);
    expect(isBleChunkAcknowledgement(decodeBleFrame(new TextEncoder().encode('R1|12|0|2|OK CHUNK')))).toBe(false);
    expect(isBleChunkAcknowledgement(decodeBleFrame(encodeBleFrames('12', new TextEncoder().encode('OK CHUNK extra'), 'R1')[0]))).toBe(false);
  });

  it('rejects oversized and malformed responses', () => {
    expect(() => decodeBleResponse(new Uint8Array(BLE_FRAME_MAX_BYTES + 1))).toThrow();
    expect(() => decodeBleResponse(new TextEncoder().encode('[]'))).toThrow();
    expect(decodeBleResponse(new TextEncoder().encode('OK SCAN PENDING')).scanPending).toBe(true);
    expect(decodeBleResponse(new TextEncoder().encode('OK SCAN READY\nLab,-42,1')).networks?.[0].secure).toBe(true);
    expect(decodeBleResponse(new TextEncoder().encode('OK SCAN READY count=1\nLab,-42,1')).networks).toHaveLength(1);
    expect(() => decodeBleResponse(new TextEncoder().encode('OK SCAN READY count=2\nLab,-42,1'))).toThrow('count');
    expect(() => decodeBleResponse(new TextEncoder().encode('OK LIST_KNOWN_WIFI count=1\nLab,1\nOther,0'))).toThrow('count');
  });

  it('keeps valid hidden scan rows in the raw count but omits them from selectable networks', () => {
    expect(decodeBleResponse(new TextEncoder().encode(
      'OK SCAN READY count=2\nCafe,-42,1\n,-88,0',
    )).networks).toEqual([{ ssid: 'Cafe', rssi: -42, secure: true }]);
  });

  it('accepts an all-hidden scan as a successful empty selectable result with raw count', () => {
    expect(decodeBleResponse(new TextEncoder().encode(
      'OK SCAN READY count=2\n,-88,0\n,-91,1',
    ))).toEqual({ ok: true, networks: [], rawCount: 2 });
  });

  it('still rejects malformed scan rows and count mismatches including hidden rows', () => {
    for (const row of ['Cafe,,1', 'Cafe,nope,1', 'Cafe,-42,2']) {
      expect(() => decodeBleResponse(new TextEncoder().encode(`OK SCAN READY count=1\n${row}`))).toThrow('BLE scan entry invalid');
    }
    expect(() => decodeBleResponse(new TextEncoder().encode(
      'OK SCAN READY count=3\nCafe,-42,1\n\nHidden,-88,0',
    ))).toThrow('BLE scan entry invalid');
    expect(() => decodeBleResponse(new TextEncoder().encode(
      'OK SCAN READY count=1\nCafe,-42,1\n,-88,0',
    ))).toThrow('BLE scan count invalid');
  });

  it('preserves nonempty scan SSIDs exactly', () => {
    expect(decodeBleResponse(new TextEncoder().encode(
      'OK SCAN READY count=1\n Cafe,,-42,1',
    )).networks).toEqual([{ ssid: ' Cafe,', rssi: -42, secure: true }]);
  });

  it('decodes only the canonical device identity routing response', () => {
    expect(decodeBleResponse(new TextEncoder().encode(
      'OK DEVICE_ID id=0000000000001234',
    ))).toEqual({ ok: true, deviceId: '0000000000001234' });
    expect(() => decodeBleResponse(new TextEncoder().encode(
      'OK DEVICE_ID id=SIGAP-01',
    ))).toThrow();
    expect(() => decodeBleResponse(new TextEncoder().encode(
      'OK DEVICE_ID id=0000000000001234 extra',
    ))).toThrow();
  });

  it('does not claim support outside a secure Web Bluetooth context', () => {
    expect(isWebBluetoothAvailable()).toBe(false);
  });

  it('encodes bounded calibration operations without changing sensor identity', () => {
    const bytes = encodeCalibrationCommand('CRITERIA', 'ds18b20_b3', {
      reference_c: 25.5,
      stable_samples: 8,
    });
    expect(new TextDecoder().decode(bytes)).toBe(
      'CAL CRITERIA ds18b20_b3 reference_c=25.5 stable_samples=8\n',
    );
    expect(new TextDecoder().decode(encodeCalibrationCommand('CAPTURE', 'ds18b20_b3', { anchor: 'reference' })))
      .toBe('CAL CAPTURE ds18b20_b3 reference\n');
  expect(() => encodeCalibrationCommand('START', 'sensor with spaces')).toThrow();
  expect(() => encodeCalibrationCommand('START', 'ds18b20_b3', { note: 'x y' })).toThrow();
  expect(new TextDecoder().decode(encodeCalibrationCommand('START', 'hx711_organic', {
    reference_mass_mg: 2000,
  }))).toContain('CAL START hx711_organic reference_mass_mg=2000');
  expect(() => encodeCalibrationCommand('START', 'unknown_sensor')).toThrow('tidak dikenal');
  expect(() => encodeCalibrationCommand('START')).toThrow('wajib');
  expect(new TextDecoder().decode(encodeCalibrationCommand('STATUS'))).toBe('CAL STATUS\n');
  expect(new TextDecoder().decode(encodeCalibrationCommand('HISTORY'))).toBe('CAL HISTORY\n');
  expect(new TextDecoder().decode(encodeCalibrationCommand('COMMIT', 'hx711_organic', {
    expected_profile_version: '18446744073709551615',
    expected_profile_hash: 'sha256:' + 'a'.repeat(64),
  }))).toContain('expected_profile_version=18446744073709551615');
});

  it('encodes the exact normalized MQ135 BLE criteria contract and rejects caller constants', () => {
    const criteria = {
      response_mode: 'normalized', samples: 3, min_duration_ms: 1000, max_age_ms: 250, max_variation_milli_percent: 5000,
      circuit_mv: 5000, divider_top_ohm: 10000, divider_bottom_ohm: 15000, topology: 1,
      hardware_confirmed: 1, reference_confirmed: 1, hardware_identity: 'SYNTHETIC-HW', reference_environment: 'LAB-01',
      reference_kind: 'operational', preparation_confirmed: 0, preparation_method: 'unknown', heating_duration_us: 0,
    };
    expect(new TextDecoder().decode(encodeCalibrationCommand('CRITERIA', 'mq135_gas', criteria))).toBe(
      'CAL CRITERIA mq135_gas circuit_mv=5000 divider_bottom_ohm=15000 divider_top_ohm=10000 hardware_confirmed=1 hardware_identity=SYNTHETIC-HW heating_duration_us=0 max_age_ms=250 max_variation_milli_percent=5000 min_duration_ms=1000 preparation_confirmed=0 preparation_method=unknown reference_confirmed=1 reference_environment=LAB-01 reference_kind=operational response_mode=normalized samples=3 topology=1\n',
    );
    const optional = new TextDecoder().decode(encodeCalibrationCommand('CRITERIA', 'mq135_gas', {
      ...criteria, reference_temperature_centi_c: -500, reference_humidity_centi_pct: 0,
    }));
    expect(optional).toContain('reference_temperature_centi_c=-500');
    expect(optional).toContain('reference_humidity_centi_pct=0');
    expect(() => encodeCalibrationCommand('CRITERIA', 'mq135_gas', {...criteria, q0: 1})).toThrow();
    expect(() => encodeCalibrationCommand('CRITERIA', 'mq135_gas', {...criteria, module_load_ohm: 10000})).toThrow();
    expect(() => encodeCalibrationCommand('CRITERIA', 'mq135_gas', {...criteria, response_mode: 'legacy'})).toThrow();
    expect(() => encodeCalibrationCommand('CRITERIA', 'mq135_gas', {samples: 3})).toThrow();
  });

  it('decodes a firmware MQG2 result from the existing CAL1 BLE response envelope', () => {
    const result = decodeBleResponse(new TextEncoder().encode(
      `CAL1|op=EVIDENCE|status=ACCEPTED|session=2|sensor=mq135_gas|data=${JSON.stringify(mqg2Fixture)}`,
    ));
    expect(result.calibration?.gas?.schema).toBe('MQG2');
    if (result.calibration?.gas?.schema === 'MQG2') {
      expect(result.calibration.gas.nh3).toMatchObject({status: 'IN_RANGE', production_eligible: false});
      expect(result.calibration.gas.response.reference_kind).toBe('CLEAN_AIR_CONFIRMED');
    }
  });

  it('decodes calibration response text while preserving bounded errors', () => {
    expect(decodeBleResponse(new TextEncoder().encode(
      'CAL1|op=PREVIEW|status=ACCEPTED|session=12|sensor=ds18b20_b3|data=proposal_hash=abc;profile_version=2',
    ))).toEqual({ ok: true, calibration: {
      version: 'CAL1', operation: 'PREVIEW', status: 'ACCEPTED', sessionId: '12',
      sensor: 'ds18b20_b3', data: { proposal_hash: 'abc', profile_version: '2' },
    } });
    expect(decodeBleResponse(new TextEncoder().encode('ERR CAL_SESSION_BUSY')))
      .toEqual({ ok: false, error: 'CAL_SESSION_BUSY' });
    expect(decodeBleResponse(new TextEncoder().encode(
      'CAL1|op=COMMIT|status=ERROR|session=12|sensor=ds18b20_b3|data=error=CONFLICT',
    ))).toEqual({ ok: false, error: 'CONFLICT', calibration: {
      version: 'CAL1', operation: 'COMMIT', status: 'ERROR', sessionId: '12',
      sensor: 'ds18b20_b3', data: { error: 'CONFLICT' }, error: 'CONFLICT',
    } });
  });
});
