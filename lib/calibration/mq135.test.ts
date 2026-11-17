import { describe, expect, it } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import fixture from '../../contracts/calibration/mq135-relative/v1/fixtures/ready-test-only.json';
import { decodeMq135GasResult, mq135Nh3EstimateCurrent, mq135RatioCurrent, parseMq135GasJson } from './mq135';
import mqg2Fixture from '../../contracts/calibration/mq135-nh3/v2/fixtures/in-range.json';
import { assembleBleFrames, decodeBleResponse, decodeBleFrame, encodeBleFrames, encodeCalibrationCommand } from '../ble/protocol';

const decodeMqg1 = (value: unknown) => {
 const result = decodeMq135GasResult(value);
 if (!result || result.schema !== 'MQG1') throw new Error('MQG1 fixture invalid');
 return result;
};

const normalizedFixtureDir = new URL('../../contracts/calibration/mq135-nh3/v2/fixtures/', import.meta.url);
const mqg2Fixtures = [
  'above-range.json', 'adc-unavailable.json', 'age-at-inclusive-boundary.json', 'below-range.json',
  'in-range.json', 'invalid-ratio.json', 'operational-baseline.json', 'stale-source.json', 'worst-case-valid.json',
  'source-identity-invalid.json', 'source-identity-unavailable.json',
  'negative/adc-saturated.json', 'negative/age-over-boundary.json', 'negative/raw-absent.json',
  'negative/sequence-zero.json', 'negative/source-profile-mismatch.json',
];

describe('MQG1 device-authored relative response', () => {
 it('accepts the actual native serializer fixture and preserves present zero', () => {
  const result = decodeMqg1(fixture);
  expect(result.raw_counts).toBe(1500);
  expect(decodeMqg1({ ...fixture, raw_counts: 0, rs_ohm: null, rs_over_r0: null, quality: 'OUT_OF_RANGE', reason: 'ADC_SATURATED' }).raw_counts).toBe(0);
  expect(result.nh3_ppm).toBeNull();
 });
 it('rejects fabricated concentration, missing fields and unsafe identity types', () => {
  expect(decodeMq135GasResult({ ...fixture, nh3_ppm: 42 })).toBeNull();
  expect(decodeMq135GasResult({ ...fixture, source_sequence: 3 })).toBeNull();
  const missing = { ...fixture } as Record<string, unknown>; delete missing.r0_ohm;
  expect(decodeMq135GasResult(missing)).toBeNull();
  expect(decodeMq135GasResult({ ...fixture, ratio: 1 })).toBeNull();
 });
 it('expires a response without retimestamping it or using a last-good ratio', () => {
  const value = decodeMqg1(fixture);
  expect(mq135RatioCurrent(value, 0)).toBe(true);
  expect(mq135RatioCurrent(value, 2)).toBe(false);
  expect(mq135RatioCurrent({ ...value, fresh: null },0)).toBe(false);
  expect(mq135RatioCurrent(value,-1)).toBe(false);
 });
 it('carries real MQG1 JSON in bounded CAL1 response frames and the full reference command', () => {
  const line = `CAL1|op=EVIDENCE|status=ACCEPTED|session=1|sensor=mq135_gas|data=${JSON.stringify(fixture)}`;
  const frames = encodeBleFrames('42',new TextEncoder().encode(line),'R1').map(decodeBleFrame);
  expect(frames.length).toBeGreaterThan(128);
  const decoded = decodeBleResponse(assembleBleFrames(frames));
  expect(decoded.calibration?.gas).toEqual(fixture);
  const encoded = encodeCalibrationCommand('CRITERIA','mq135_gas',{response_mode:'normalized',samples:3,min_duration_ms:2,max_age_ms:1,max_variation_milli_percent:10,circuit_mv:5000,divider_top_ohm:10000,divider_bottom_ohm:15000,topology:1,hardware_confirmed:1,reference_confirmed:1,hardware_identity:'SYNTHETIC',reference_environment:'SYNTHETIC',reference_kind:'operational',preparation_confirmed:0,preparation_method:'unknown',heating_duration_us:10000,reference_temperature_centi_c:2500,reference_humidity_centi_pct:5000});
  expect(encoded.length).toBeGreaterThan(254);
  expect(encoded.length).toBeLessThanOrEqual(1024);
 });
});

describe('MQG1/MQG2 strict calibration result decoding', () => {
  it('decodes every frozen MQG2 result fixture and keeps MQG1 separate', () => {
    for (const name of mqg2Fixtures) {
      const raw = readFileSync(fileURLToPath(new URL(name, normalizedFixtureDir)), 'utf8');
      const decoded = parseMq135GasJson(raw);
      expect(decoded, name).not.toBeNull();
      expect(decoded?.schema, name).toBe('MQG2');
    }
    const mqg1 = readFileSync(fileURLToPath(new URL('legacy-mqg1.json', normalizedFixtureDir)), 'utf8');
    const legacy = parseMq135GasJson(mqg1);
    expect(legacy?.schema).toBe('MQG1');
    if (legacy?.schema === 'MQG1') expect(legacy.nh3_ppm).toBeNull();
  });

  it('rejects the invalid serializer stress fixture without normalizing it', () => {
    const name = 'negative/serializer-stress-invalid.json';
    const raw = readFileSync(fileURLToPath(new URL(name, normalizedFixtureDir)), 'utf8');
    expect(parseMq135GasJson(raw), name).toBeNull();
  });

  it('rejects duplicate decoded member names at root and nested groups', () => {
    const raw = readFileSync(fileURLToPath(new URL('in-range.json', normalizedFixtureDir)), 'utf8');
    const rootDuplicate = raw.replace('"schema":"MQG2",', '"schema":"MQG2","\\u0073chema":"MQG2",');
    const nestedDuplicate = raw.replace('"response":{"q":', '"response":{"q":1,"q":');
    expect(rootDuplicate).not.toBe(raw);
    expect(nestedDuplicate).not.toBe(raw);
    expect(parseMq135GasJson(rootDuplicate)).toBeNull();
    expect(parseMq135GasJson(nestedDuplicate)).toBeNull();
  });

  it('decodes identity-invalid diagnostics without exposing current quantities', () => {
    // Constructed from the immutable in-range fixture; this is not producer-captured output.
    const diagnostic = {
      ...mqg2Fixture,
      quality: 'INVALID',
      reason: 'SOURCE_IDENTITY_INVALID',
      adc: { ...mqg2Fixture.adc, ao_mv: null },
      response: { ...mqg2Fixture.response, q: null, ratio: null },
      nh3: { ...mqg2Fixture.nh3, ppm: null, status: 'SOURCE_NOT_READY' },
    };
    const decoded = decodeMq135GasResult(diagnostic);
    expect(decoded).not.toBeNull();
    if (decoded?.schema !== 'MQG2') throw new Error('constructed MQG2 diagnostic rejected');
    expect(decoded.reason).toBe('SOURCE_IDENTITY_INVALID');
    expect(decoded.quality).toBe('INVALID');
    expect(decoded.source.quality).toBe('READY');
    expect(decoded.source.fresh).toBe(true);
    expect(decoded.adc.ao_mv).toBeNull();
    expect(decoded.response.q).toBeNull();
    expect(decoded.response.q0).toBe(1);
    expect(decoded.response.ratio).toBeNull();
    expect(decoded.nh3.ppm).toBeNull();
    expect(mq135RatioCurrent(decoded, 0)).toBe(false);
    expect(mq135Nh3EstimateCurrent(decoded, 0)).toBe(false);
  });

  it('keeps source identity failure ahead of sample-quality precedence', () => {
    // Constructed from the immutable in-range fixture; this is not producer-captured output.
    const diagnostic = {
      ...mqg2Fixture,
      quality: 'INVALID',
      reason: 'SOURCE_IDENTITY_INVALID',
      adc: { ...mqg2Fixture.adc, ao_mv: null },
      response: { ...mqg2Fixture.response, q: null, ratio: null },
      nh3: { ...mqg2Fixture.nh3, ppm: null, status: 'SOURCE_NOT_READY' },
      source: { ...mqg2Fixture.source, quality: 'WARMING_UP' },
    };
    const decoded = decodeMq135GasResult(diagnostic);
    expect(decoded).not.toBeNull();
    if (decoded?.schema !== 'MQG2') throw new Error('constructed MQG2 diagnostic rejected');
    expect(decoded.quality).toBe('INVALID');
    expect(decoded.reason).toBe('SOURCE_IDENTITY_INVALID');
    expect(decoded.source.quality).toBe('WARMING_UP');
    expect(decoded.response.q).toBeNull();
    expect(decoded.response.ratio).toBeNull();
    expect(decoded.nh3.ppm).toBeNull();
  });

  it('allows known source quality when the joint identity and clock are unavailable', () => {
    // Constructed from the immutable in-range fixture; this is not producer-captured output.
    const diagnostic = {
      ...mqg2Fixture,
      quality: 'INVALID',
      reason: 'SOURCE_CLOCK_UNKNOWN',
      adc: { ...mqg2Fixture.adc, ao_mv: null },
      response: { ...mqg2Fixture.response, q: null, ratio: null },
      nh3: { ...mqg2Fixture.nh3, ppm: null, status: 'SOURCE_NOT_READY' },
      source: {
        ...mqg2Fixture.source,
        boot_id: null,
        sequence: null,
        monotonic_us: null,
        age_us: null,
        max_age_us: null,
        fresh: null,
        quality: 'READY',
      },
    };
    const decoded = decodeMq135GasResult(diagnostic);
    expect(decoded).not.toBeNull();
    if (decoded?.schema !== 'MQG2') throw new Error('constructed MQG2 diagnostic rejected');
    expect(decoded.reason).toBe('SOURCE_CLOCK_UNKNOWN');
    expect(decoded.source.quality).toBe('READY');
    expect(decoded.source.fresh).toBeNull();
    expect(decoded.response.q).toBeNull();
    expect(decoded.response.ratio).toBeNull();
    expect(decoded.nh3.ppm).toBeNull();
    expect(mq135RatioCurrent(decoded, 0)).toBe(false);
    expect(mq135Nh3EstimateCurrent(decoded, 0)).toBe(false);
  });

  it('rejects identity diagnostics forged as ready or carrying current quantities', () => {
    // Constructed from the immutable in-range fixture; this is not producer-captured output.
    const diagnostic = {
      ...mqg2Fixture,
      quality: 'INVALID',
      reason: 'SOURCE_IDENTITY_INVALID',
      adc: { ...mqg2Fixture.adc, ao_mv: null },
      response: { ...mqg2Fixture.response, q: null, ratio: null },
      nh3: { ...mqg2Fixture.nh3, ppm: null, status: 'SOURCE_NOT_READY' },
    };
    expect(decodeMq135GasResult({ ...diagnostic, quality: 'READY' })).toBeNull();
    expect(decodeMq135GasResult({ ...diagnostic, adc: { ...diagnostic.adc, ao_mv: mqg2Fixture.adc.ao_mv } })).toBeNull();
    expect(decodeMq135GasResult({ ...diagnostic, adc: { ...diagnostic.adc, ao_mv: mqg2Fixture.adc.ao_mv }, response: { ...diagnostic.response, q: mqg2Fixture.response.q } })).toBeNull();
    expect(decodeMq135GasResult({ ...diagnostic, response: { ...diagnostic.response, ratio: mqg2Fixture.response.ratio } })).toBeNull();
    expect(decodeMq135GasResult({ ...diagnostic, nh3: { ...diagnostic.nh3, ppm: mqg2Fixture.nh3.ppm } })).toBeNull();
    expect(decodeMq135GasResult({ ...diagnostic, reason: 'IN_RANGE' })).toBeNull();
    expect(decodeMq135GasResult({ ...diagnostic, reason: 'SOURCE_CLOCK_UNKNOWN' })).toBeNull();
    expect(decodeMq135GasResult({
      ...diagnostic,
      reason: 'SOURCE_NOT_READY',
      source: { ...diagnostic.source, boot_id: null, sequence: null, monotonic_us: null, age_us: null, max_age_us: null, fresh: null },
    })).toBeNull();
  });

  it('keeps actual producer identity diagnostics unavailable and rejects forged partial identities', () => {
    for (const name of ['source-identity-invalid.json', 'source-identity-unavailable.json']) {
      const diagnostic = JSON.parse(readFileSync(fileURLToPath(new URL(name, normalizedFixtureDir)), 'utf8'));
      const decoded = decodeMq135GasResult(diagnostic);
      expect(decoded, name).not.toBeNull();
      if (decoded?.schema !== 'MQG2') throw new Error('actual producer diagnostic rejected');
      expect(decoded.quality).toBe('INVALID');
      expect(decoded.nh3.status).toBe('SOURCE_NOT_READY');
      expect(decoded.adc.ao_mv).toBeNull();
      expect(decoded.response.q).toBeNull();
      expect(decoded.response.q0).toBe(1);
      expect(decoded.response.ratio).toBeNull();
      expect(decoded.nh3.ppm).toBeNull();
      expect(mq135RatioCurrent(decoded, 0)).toBe(false);
      expect(mq135Nh3EstimateCurrent(decoded, 0)).toBe(false);
      for (const forged of [
        { ...diagnostic, quality: 'READY' },
        { ...diagnostic, source: { ...diagnostic.source, sequence: null } },
        { ...diagnostic, source: { ...diagnostic.source, monotonic_us: null } },
        { ...diagnostic, adc: { ...diagnostic.adc, ao_mv: 1 } },
        { ...diagnostic, response: { ...diagnostic.response, q: 1 } },
        { ...diagnostic, response: { ...diagnostic.response, ratio: 1 } },
        { ...diagnostic, nh3: { ...diagnostic.nh3, ppm: 10 } },
      ]) expect(decodeMq135GasResult(forged), name).toBeNull();
      if (diagnostic.reason === 'SOURCE_CLOCK_UNKNOWN') {
        expect(decodeMq135GasResult({ ...diagnostic, source: { ...diagnostic.source, quality: 'WARMING_UP' } })).toBeNull();
      }
    }
  });

  it('retains an unknown-clock diagnostic when source time is ahead of response time', () => {
    // Constructed regression for the original producer's future-source-time guard.
    const diagnostic = JSON.parse(readFileSync(fileURLToPath(new URL('source-identity-unavailable.json', normalizedFixtureDir)), 'utf8'));
    diagnostic.source.boot_id = mqg2Fixture.source.boot_id;
    diagnostic.source.monotonic_us = String(BigInt(diagnostic.source.response_monotonic_us) + 1n);
    const decoded = decodeMq135GasResult(diagnostic);
    expect(decoded).not.toBeNull();
    if (decoded?.schema !== 'MQG2') throw new Error('clock diagnostic rejected');
    expect(decoded.source.fresh).toBeNull();
    expect(mq135RatioCurrent(decoded, 0)).toBe(false);
    expect(mq135Nh3EstimateCurrent(decoded, 0)).toBe(false);
    expect(decodeMq135GasResult({ ...mqg2Fixture, source: { ...mqg2Fixture.source, monotonic_us: diagnostic.source.monotonic_us } })).toBeNull();
  });
});
