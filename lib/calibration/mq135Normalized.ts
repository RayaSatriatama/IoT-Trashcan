const ROOT_KEYS = ['schema', 'sensor', 'quality', 'reason', 'adc', 'electrical', 'response', 'nh3', 'profiles', 'source', 'reference'];
const GROUP_KEYS = {
  adc: ['raw_counts', 'available', 'pin_mv', 'ao_mv', 'method', 'same_sample', 'failure', 'resolution_bits', 'attenuation_db', 'adc_unit', 'adc_channel'],
  electrical: ['circuit_mv', 'divider_top_ohm', 'divider_bottom_ohm', 'topology', 'hardware_confirmed', 'hardware_identity'],
  response: ['q', 'q0', 'ratio', 'reference_kind', 'baseline_valid'],
  nh3: ['ppm', 'status', 'curve_id', 'curve_version', 'source_sha256', 'method', 'production_eligible'],
  profiles: ['state', 'version', 'hash', 'previous_version', 'previous_hash', 'adc_version', 'adc_hash'],
  source: ['boot_id', 'sequence', 'monotonic_us', 'response_monotonic_us', 'age_us', 'max_age_us', 'fresh', 'quality'],
  reference: ['environment', 'temperature_centi_c', 'humidity_centi_pct', 'conditions_source', 'heating_duration_us', 'preparation_confirmed', 'preparation_method', 'sample_count', 'variation_percent', 'calibration_epoch_ms', 'calibration_time_quality'],
} as const;
const CURVE_ID = 20040;
const CURVE_VERSION = 2;
const CURVE_SHA256 = 'sha256:4aa984238efa7d253b77ac828babd40e3a8f10ef14a3a86f8b9f1da0e6dd414b';
const RATIO_MIN = 0.0893;
const RATIO_MAX = 0.6559;
const MIN_CLEAN_AIR_PREHEAT_US = 172800000000;
const UINT32_MAX = 4294967295n;
const UINT64_MAX = 18446744073709551615n;
const INT64_MIN = -(1n << 63n);
const INT64_MAX = (1n << 63n) - 1n;

type Quality = 'READY' | 'CALIBRATING' | 'UNCALIBRATED' | 'STALE' | 'INVALID' | 'OUT_OF_RANGE' | 'DISCONNECTED' | 'TIMEOUT' | 'UNAVAILABLE' | 'WARMING_UP' | 'HEATER_PHASE' | 'CRC_ERROR';
type Nh3Status = 'UNCALIBRATED' | 'REFERENCE_NOT_CLEAN_AIR' | 'PREHEAT_UNCONFIRMED' | 'ADC_CALIBRATION_UNAVAILABLE' | 'SOURCE_NOT_READY' | 'SOURCE_STALE' | 'PROFILE_INVALID' | 'IN_RANGE' | 'BELOW_RANGE' | 'ABOVE_RANGE' | 'CURVE_UNAVAILABLE' | 'CURVE_INVALID' | 'INVALID_RATIO';
type Uint64 = string;
type NullableUint64 = Uint64 | null;
type HashOrNull = `sha256:${string}` | null;
type PositiveOrNull = number | null;

export type Mq135NormalizedResult = {
  schema: 'MQG2'; sensor: 'mq135'; quality: Quality;
  reason: 'NO_NORMALIZED_PROFILE' | 'NORMALIZED_PROFILE_INVALID' | 'ADC_CALIBRATION_UNAVAILABLE' | 'NO_SOURCE' | 'SOURCE_IDENTITY_INVALID' | 'SOURCE_NOT_READY' | 'SOURCE_CLOCK_UNKNOWN' | 'SOURCE_STALE' | 'ADC_PROFILE_CHANGED' | 'AO_OUT_OF_RANGE' | 'BASELINE_MISSING' | 'IN_RANGE' | 'BELOW_RANGE' | 'ABOVE_RANGE' | 'INVALID_RATIO' | 'CURVE_INVALID' | 'CURVE_UNAVAILABLE' | 'REFERENCE_NOT_CLEAN_AIR' | 'PREHEAT_UNCONFIRMED';
  adc: { raw_counts: number | null; available: boolean; pin_mv: number | null; ao_mv: number | null; method: 'EFUSE_TP_FIT' | 'UNAVAILABLE'; same_sample: boolean; failure: string; resolution_bits: number | null; attenuation_db: number | null; adc_unit: number | null; adc_channel: number | null };
  electrical: { circuit_mv: number | null; divider_top_ohm: number | null; divider_bottom_ohm: number | null; topology: 1 | 2 | null; hardware_confirmed: boolean; hardware_identity: string | null };
  response: { q: PositiveOrNull; q0: PositiveOrNull; ratio: PositiveOrNull; reference_kind: 'NONE' | 'OPERATIONAL' | 'CLEAN_AIR_CONFIRMED'; baseline_valid: boolean };
  nh3: { ppm: number | null; status: Nh3Status; curve_id: 20040 | null; curve_version: 2 | null; source_sha256: HashOrNull; method: 'PIECEWISE_LOG_LOG'; production_eligible: false };
  profiles: { state: 'MISSING' | 'INVALID' | 'TEST_ONLY' | 'VALID'; version: string | null; hash: HashOrNull; previous_version: string | null; previous_hash: HashOrNull; adc_version: string | null; adc_hash: HashOrNull };
  source: { boot_id: string | null; sequence: NullableUint64; monotonic_us: NullableUint64; response_monotonic_us: Uint64; age_us: NullableUint64; max_age_us: NullableUint64; fresh: boolean | null; quality: Quality | null };
  reference: { environment: string | null; temperature_centi_c: number | null; humidity_centi_pct: number | null; conditions_source: 'UNKNOWN' | 'DECLARED'; heating_duration_us: NullableUint64; preparation_confirmed: boolean; preparation_method: 'UNKNOWN' | 'OPERATOR_DECLARED'; sample_count: number | null; variation_percent: number | null; calibration_epoch_ms: string | null; calibration_time_quality: 'SYNCHRONIZED' | 'ESTIMATED' | 'UNSYNCED' };
};

function object(value: unknown, keys: readonly string[]): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value) &&
    (Object.getPrototypeOf(value) === Object.prototype || Object.getPrototypeOf(value) === null) &&
    Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
}
const isChoice = (value: unknown, choices: readonly string[]): value is string => typeof value === 'string' && choices.includes(value);
const finite = (value: unknown): value is number => typeof value === 'number' && Number.isFinite(value);
const boundedInt = (value: unknown, min: number, max: number): value is number => finite(value) && Number.isInteger(value) && value >= min && value <= max;
const positiveOrNull = (value: unknown): value is PositiveOrNull => value === null || (finite(value) && value > 0);
const uintString = (value: unknown, max = UINT64_MAX): value is string => typeof value === 'string' && /^(?:0|[1-9][0-9]{0,19})$/u.test(value) && BigInt(value) <= max;
const int64String = (value: unknown): value is string => typeof value === 'string' && /^(?:0|-?[1-9][0-9]{0,18})$/u.test(value) && BigInt(value) >= INT64_MIN && BigInt(value) <= INT64_MAX;
const nullableUint = (value: unknown): value is NullableUint64 => value === null || uintString(value);
const profileVersion = (value: unknown): value is string | null => value === null || (typeof value === 'string' && /^(?:[1-9][0-9]{0,9})$/u.test(value) && BigInt(value) <= UINT32_MAX);
const hashOrNull = (value: unknown): value is HashOrNull => value === null || (typeof value === 'string' && /^sha256:[0-9a-f]{64}$/u.test(value));
const labelOrNull = (value: unknown): value is string | null => value === null || (typeof value === 'string' && /^[A-Za-z0-9_.:-]{1,31}$/u.test(value));

function validShape(v: Record<string, unknown>): boolean {
  const { adc, electrical, response, nh3, profiles, source, reference } = v;
  if (!object(adc, GROUP_KEYS.adc) || !object(electrical, GROUP_KEYS.electrical) || !object(response, GROUP_KEYS.response) ||
      !object(nh3, GROUP_KEYS.nh3) || !object(profiles, GROUP_KEYS.profiles) || !object(source, GROUP_KEYS.source) || !object(reference, GROUP_KEYS.reference)) return false;
  const quality = ['READY','CALIBRATING','UNCALIBRATED','STALE','INVALID','OUT_OF_RANGE','DISCONNECTED','TIMEOUT','UNAVAILABLE','WARMING_UP','HEATER_PHASE','CRC_ERROR'] as const;
  const reasons = ['NO_NORMALIZED_PROFILE','NORMALIZED_PROFILE_INVALID','ADC_CALIBRATION_UNAVAILABLE','NO_SOURCE','SOURCE_IDENTITY_INVALID','SOURCE_NOT_READY','SOURCE_CLOCK_UNKNOWN','SOURCE_STALE','ADC_PROFILE_CHANGED','AO_OUT_OF_RANGE','BASELINE_MISSING','IN_RANGE','BELOW_RANGE','ABOVE_RANGE','INVALID_RATIO','CURVE_INVALID','CURVE_UNAVAILABLE','REFERENCE_NOT_CLEAN_AIR','PREHEAT_UNCONFIRMED'] as const;
  const statuses = ['UNCALIBRATED','REFERENCE_NOT_CLEAN_AIR','PREHEAT_UNCONFIRMED','ADC_CALIBRATION_UNAVAILABLE','SOURCE_NOT_READY','SOURCE_STALE','PROFILE_INVALID','IN_RANGE','BELOW_RANGE','ABOVE_RANGE','CURVE_UNAVAILABLE','CURVE_INVALID','INVALID_RATIO'] as const;
  if (v.schema !== 'MQG2' || v.sensor !== 'mq135' || !isChoice(v.quality, quality) || !isChoice(v.reason, reasons)) return false;
  if (adc.raw_counts !== null && !boundedInt(adc.raw_counts, 0, 4095)) return false;
  if (typeof adc.available !== 'boolean' || (adc.pin_mv !== null && (!finite(adc.pin_mv) || adc.pin_mv < 0)) || (adc.ao_mv !== null && (!finite(adc.ao_mv) || adc.ao_mv < 0)) ||
      !isChoice(adc.method, ['EFUSE_TP_FIT','UNAVAILABLE']) || typeof adc.same_sample !== 'boolean' || typeof adc.failure !== 'string' || !/^[A-Z][A-Z0-9_]{0,31}$/u.test(adc.failure)) return false;
  for (const key of ['resolution_bits','attenuation_db','adc_unit','adc_channel']) if (adc[key] !== null && !boundedInt(adc[key], key === 'resolution_bits' ? 1 : 0, key === 'resolution_bits' ? 32 : 255)) return false;
  for (const key of ['circuit_mv','divider_top_ohm','divider_bottom_ohm']) if (electrical[key] !== null && !boundedInt(electrical[key], 1, 4294967295)) return false;
  if (electrical.topology !== null && electrical.topology !== 1 && electrical.topology !== 2) return false;
  if (typeof electrical.hardware_confirmed !== 'boolean' || !labelOrNull(electrical.hardware_identity)) return false;
  for (const key of ['q','q0','ratio']) if (!positiveOrNull(response[key])) return false;
  if (!isChoice(response.reference_kind, ['NONE','OPERATIONAL','CLEAN_AIR_CONFIRMED']) || typeof response.baseline_valid !== 'boolean') return false;
  if (nh3.ppm !== null && (!finite(nh3.ppm) || nh3.ppm < 10 || nh3.ppm > 1000)) return false;
  if (!isChoice(nh3.status, statuses) || (nh3.curve_id !== null && nh3.curve_id !== CURVE_ID) ||
      (nh3.curve_version !== null && nh3.curve_version !== CURVE_VERSION) || !hashOrNull(nh3.source_sha256) ||
      nh3.method !== 'PIECEWISE_LOG_LOG' || nh3.production_eligible !== false) return false;
  if (!isChoice(profiles.state, ['MISSING','INVALID','TEST_ONLY','VALID'])) return false;
  for (const key of ['version','previous_version','adc_version']) if (!profileVersion(profiles[key])) return false;
  for (const key of ['hash','previous_hash','adc_hash']) if (!hashOrNull(profiles[key])) return false;
  for (const [version, hash] of [['version','hash'],['previous_version','previous_hash'],['adc_version','adc_hash']]) if ((profiles[version] === null) !== (profiles[hash] === null)) return false;
  if (source.boot_id !== null && (typeof source.boot_id !== 'string' || !/^[0-9a-f]{32}$/u.test(source.boot_id))) return false;
  if (!nullableUint(source.sequence) || !nullableUint(source.monotonic_us) || !uintString(source.response_monotonic_us) ||
      !nullableUint(source.age_us) || !nullableUint(source.max_age_us) || (source.fresh !== null && typeof source.fresh !== 'boolean') ||
      (source.quality !== null && !isChoice(source.quality, quality))) return false;
  if (!labelOrNull(reference.environment) || (reference.temperature_centi_c !== null && !boundedInt(reference.temperature_centi_c,-2147483648,2147483647)) ||
      (reference.humidity_centi_pct !== null && !boundedInt(reference.humidity_centi_pct,0,10000)) || !isChoice(reference.conditions_source,['UNKNOWN','DECLARED']) ||
      !nullableUint(reference.heating_duration_us) || typeof reference.preparation_confirmed !== 'boolean' ||
      !isChoice(reference.preparation_method,['UNKNOWN','OPERATOR_DECLARED']) || (reference.sample_count !== null && !boundedInt(reference.sample_count,0,64)) ||
      (reference.variation_percent !== null && (!finite(reference.variation_percent) || reference.variation_percent < 0)) ||
      (reference.calibration_epoch_ms !== null && !int64String(reference.calibration_epoch_ms)) ||
      !isChoice(reference.calibration_time_quality,['SYNCHRONIZED','ESTIMATED','UNSYNCED'])) return false;
  return true;
}

function semanticallyValid(d: Mq135NormalizedResult): boolean {
  const { adc, electrical, response, nh3, profiles, source, reference } = d;
  if (adc.available) {
    if (adc.method !== 'EFUSE_TP_FIT' || !adc.same_sample || adc.failure !== 'NONE' || adc.resolution_bits !== 12 || adc.attenuation_db !== 11 ||
        adc.adc_unit !== 1 || adc.adc_channel !== 4 || adc.raw_counts === null || adc.raw_counts <= 0 || adc.raw_counts >= 4095 || adc.pin_mv === null || adc.pin_mv <= 0) return false;
  } else {
    if (adc.failure === 'NONE' || adc.pin_mv !== null || adc.ao_mv !== null) return false;
    if (adc.method === 'UNAVAILABLE' && [adc.resolution_bits,adc.attenuation_db,adc.adc_unit,adc.adc_channel].some(value => value !== null)) return false;
  }
  const circuit = [electrical.circuit_mv,electrical.divider_top_ohm,electrical.divider_bottom_ohm,electrical.topology];
  if (electrical.hardware_confirmed && (circuit.some(value => value === null) || electrical.hardware_identity === null)) return false;
  if (adc.ao_mv !== null && (!adc.available || adc.pin_mv === null || circuit.some(value => value === null))) return false;
  if (profiles.state === 'MISSING' && profiles.version !== null) return false;
  if ((profiles.state === 'TEST_ONLY' || profiles.state === 'VALID') && profiles.version === null) return false;

  const identity = [source.sequence,source.boot_id,source.monotonic_us];
  const partialBootIdentity = source.boot_id === null && source.sequence !== null && source.monotonic_us !== null &&
    source.quality !== null && ['SOURCE_IDENTITY_INVALID','SOURCE_CLOCK_UNKNOWN'].includes(d.reason);
  if (identity.some(value => value === null) !== identity.every(value => value === null) && !partialBootIdentity) return false;
  const responseTime = BigInt(source.response_monotonic_us);
  if (source.monotonic_us === null) {
    if ([source.age_us,source.max_age_us,source.fresh].some(value => value !== null) ||
        (source.quality !== null && !['SOURCE_IDENTITY_INVALID','SOURCE_CLOCK_UNKNOWN'].includes(d.reason))) return false;
  } else {
    if (source.sequence === '0') return false;
    const sampleTime = BigInt(source.monotonic_us);
    const clockUnavailable = ['SOURCE_IDENTITY_INVALID','SOURCE_CLOCK_UNKNOWN'].includes(d.reason) &&
      [source.age_us,source.max_age_us,source.fresh].every(value => value === null);
    if (responseTime < sampleTime && !clockUnavailable) return false;
    if (source.age_us !== null && BigInt(source.age_us) !== responseTime - sampleTime) return false;
    if (source.age_us !== null && source.max_age_us !== null) {
      if (typeof source.fresh !== 'boolean' || source.fresh !== (BigInt(source.age_us) <= BigInt(source.max_age_us))) return false;
    } else if (source.fresh !== null) return false;
  }
  if (reference.preparation_confirmed && reference.preparation_method !== 'OPERATOR_DECLARED') return false;
  if (reference.conditions_source !== ([reference.environment,reference.temperature_centi_c,reference.humidity_centi_pct].some(value => value !== null) ? 'DECLARED' : 'UNKNOWN')) return false;
  if (reference.calibration_time_quality === 'SYNCHRONIZED' && reference.calibration_epoch_ms === null) return false;
  if (reference.calibration_time_quality === 'UNSYNCED' && reference.calibration_epoch_ms !== null) return false;
  const curve = [nh3.curve_id,nh3.curve_version,nh3.source_sha256];
  const curvePresent = nh3.curve_id === CURVE_ID && nh3.curve_version === CURVE_VERSION && nh3.source_sha256 === CURVE_SHA256;
  if (!curvePresent && curve.some(value => value !== null)) return false;

  const { q,q0,ratio } = response;
  if (response.baseline_valid !== (q0 !== null) || (response.baseline_valid && !['TEST_ONLY','VALID'].includes(profiles.state)) ||
      (response.reference_kind === 'NONE' && response.baseline_valid)) return false;
  if (q !== null && (!adc.available || adc.ao_mv === null || source.fresh !== true || source.quality !== 'READY')) return false;
  if (ratio !== null && (q === null || q0 === null || !response.baseline_valid || Math.abs(ratio - q / q0) > Math.max(1e-9, 1e-6 * Math.abs(ratio)))) return false;
  if (ratio === null && q !== null && response.baseline_valid && ['TEST_ONLY','VALID'].includes(profiles.state) && !['ADC_PROFILE_CHANGED','BASELINE_MISSING','SOURCE_IDENTITY_INVALID','SOURCE_CLOCK_UNKNOWN'].includes(d.reason)) return false;

  const { ppm,status } = nh3;
  const identityDiagnostic = d.reason === 'SOURCE_IDENTITY_INVALID' || d.reason === 'SOURCE_CLOCK_UNKNOWN';
  if (identityDiagnostic && (d.quality !== 'INVALID' || status !== 'SOURCE_NOT_READY' || source.quality === null || adc.raw_counts === null ||
      adc.ao_mv !== null || q !== null || ratio !== null || ppm !== null ||
      (d.reason === 'SOURCE_CLOCK_UNKNOWN' && (source.quality !== 'READY' || [source.age_us,source.max_age_us,source.fresh].some(value => value !== null))))) return false;
  if (['IN_RANGE','BELOW_RANGE','ABOVE_RANGE'].includes(status) && (d.reason !== status || d.quality !== 'READY')) return false;
  if (ppm !== null && status !== 'IN_RANGE') return false;
  if (status === 'IN_RANGE' && (ratio === null || ratio < RATIO_MIN || ratio > RATIO_MAX || ppm === null)) return false;
  if (status === 'BELOW_RANGE' && (ratio === null || ratio <= RATIO_MAX || ppm !== null)) return false;
  if (status === 'ABOVE_RANGE' && (ratio === null || ratio >= RATIO_MIN || ppm !== null)) return false;
  if (ppm !== null) {
    const heating = reference.heating_duration_us === null ? -1n : BigInt(reference.heating_duration_us);
    if (!(d.quality === 'READY' && d.reason === 'IN_RANGE' && adc.available && source.fresh === true && source.quality === 'READY' && q !== null && q0 !== null && ratio !== null && response.baseline_valid &&
      response.reference_kind === 'CLEAN_AIR_CONFIRMED' && ['TEST_ONLY','VALID'].includes(profiles.state) && curvePresent && reference.preparation_confirmed &&
      reference.preparation_method === 'OPERATOR_DECLARED' && heating > BigInt(MIN_CLEAN_AIR_PREHEAT_US) && reference.sample_count !== null && reference.sample_count >= 3 && reference.variation_percent !== null)) return false;
  }
  if (status === 'SOURCE_STALE' && (source.fresh !== false || q !== null || ratio !== null)) return false;
  if (status === 'SOURCE_NOT_READY' && !identityDiagnostic && ((source.quality === 'READY' && source.fresh !== null) || q !== null || ratio !== null)) return false;
  if (status === 'ADC_CALIBRATION_UNAVAILABLE' && adc.available) return false;
  if (status === 'REFERENCE_NOT_CLEAN_AIR' && (response.reference_kind === 'CLEAN_AIR_CONFIRMED' || ratio === null || ppm !== null)) return false;
  if (status === 'PREHEAT_UNCONFIRMED') {
    const heating = reference.heating_duration_us === null ? -1n : BigInt(reference.heating_duration_us);
    if (response.reference_kind !== 'CLEAN_AIR_CONFIRMED' || ratio === null || ppm !== null || (reference.preparation_confirmed && reference.preparation_method === 'OPERATOR_DECLARED' && heating > BigInt(MIN_CLEAN_AIR_PREHEAT_US))) return false;
  }
  if (!identityDiagnostic && ['WARMING_UP','HEATER_PHASE','CRC_ERROR'].includes(source.quality ?? '')) {
    if (d.quality !== source.quality || status !== 'SOURCE_NOT_READY' || d.reason !== 'SOURCE_NOT_READY') return false;
  }
  if (!identityDiagnostic && status === 'SOURCE_NOT_READY' && source.quality !== 'READY' && d.quality !== source.quality) return false;
  const rootMapping: Partial<Record<Nh3Status,{reasons:readonly string[];qualities:readonly string[]}>> = {
    PROFILE_INVALID:{reasons:['NORMALIZED_PROFILE_INVALID','ADC_PROFILE_CHANGED'],qualities:['INVALID']},
    UNCALIBRATED:{reasons:['NO_NORMALIZED_PROFILE','BASELINE_MISSING'],qualities:['UNCALIBRATED']},
    ADC_CALIBRATION_UNAVAILABLE:{reasons:['ADC_CALIBRATION_UNAVAILABLE'],qualities:['UNAVAILABLE']},
    SOURCE_STALE:{reasons:['SOURCE_STALE'],qualities:['STALE']},
    INVALID_RATIO:{reasons:['AO_OUT_OF_RANGE','INVALID_RATIO'],qualities:['OUT_OF_RANGE','INVALID']},
    CURVE_UNAVAILABLE:{reasons:['CURVE_UNAVAILABLE'],qualities:['UNAVAILABLE']},
    CURVE_INVALID:{reasons:['CURVE_INVALID'],qualities:['INVALID']},
    REFERENCE_NOT_CLEAN_AIR:{reasons:['REFERENCE_NOT_CLEAN_AIR'],qualities:['UNCALIBRATED']},
    PREHEAT_UNCONFIRMED:{reasons:['PREHEAT_UNCONFIRMED'],qualities:['UNCALIBRATED']},
  };
  const mapping = rootMapping[status];
  if (mapping && (!mapping.reasons.includes(d.reason) || !mapping.qualities.includes(d.quality))) return false;

  let expected: string | readonly string[];
  if (profiles.state === 'INVALID' || d.reason === 'ADC_PROFILE_CHANGED') expected = 'PROFILE_INVALID';
  else if (profiles.state === 'MISSING') expected = 'UNCALIBRATED';
  else if (!adc.available) expected = 'ADC_CALIBRATION_UNAVAILABLE';
  else if (identityDiagnostic) expected = 'SOURCE_NOT_READY';
  else if (source.quality !== 'READY' || source.sequence === null || source.fresh === null) expected = 'SOURCE_NOT_READY';
  else if (source.fresh === false) expected = 'SOURCE_STALE';
  else if (q === null) expected = 'INVALID_RATIO';
  else if (ratio === null) expected = 'UNCALIBRATED';
  else if (status === 'CURVE_UNAVAILABLE' || status === 'CURVE_INVALID') expected = status;
  else if (!curvePresent) expected = ['CURVE_UNAVAILABLE','CURVE_INVALID'];
  else if (response.reference_kind !== 'CLEAN_AIR_CONFIRMED') expected = 'REFERENCE_NOT_CLEAN_AIR';
  else if (!(reference.preparation_confirmed && reference.preparation_method === 'OPERATOR_DECLARED' && reference.heating_duration_us !== null && BigInt(reference.heating_duration_us) > BigInt(MIN_CLEAN_AIR_PREHEAT_US))) expected = 'PREHEAT_UNCONFIRMED';
  else if (ratio > RATIO_MAX) expected = 'BELOW_RANGE';
  else if (ratio < RATIO_MIN) expected = 'ABOVE_RANGE';
  else expected = 'IN_RANGE';
  return (Array.isArray(expected) ? expected : [expected]).includes(status);
}

export function decodeMq135NormalizedResult(value: unknown): Mq135NormalizedResult | null {
  if (!object(value, ROOT_KEYS) || value.schema !== 'MQG2' || !validShape(value)) return null;
  const result = value as unknown as Mq135NormalizedResult;
  return semanticallyValid(result) ? result : null;
}

/** Parse JSON structurally before JSON.parse so nested and escaped duplicate keys are not lost. */
export function parseJsonUnique(text: string): { ok: true; value: unknown } | { ok: false } {
  let index = 0;
  const whitespace = () => { while (index < text.length && /[\t\n\r ]/u.test(text[index])) index += 1; };
  const stringEnd = (start: number): number => {
    if (text[start] !== '"') return -1;
    for (let at = start + 1; at < text.length; at += 1) {
      const code = text.charCodeAt(at);
      if (code < 0x20) return -1;
      if (text[at] === '"') return at + 1;
      if (text[at] === '\\') {
        if (at + 1 >= text.length) return -1;
        at += text[at + 1] === 'u' ? 5 : 1;
      }
    }
    return -1;
  };
  const parseValue = (depth: number): boolean => {
    whitespace();
    if (depth > 32 || index >= text.length) return false;
    const token = text[index];
    if (token === '"') { const end = stringEnd(index); if (end < 0) return false; index = end; return true; }
    if (token === '{') {
      index += 1; whitespace(); if (text[index] === '}') { index += 1; return true; }
      const keys = new Set<string>();
      for (;;) {
        whitespace(); const start = index; const end = stringEnd(start); if (end < 0) return false;
        let key: unknown; try { key = JSON.parse(text.slice(start,end)); } catch { return false; }
        if (typeof key !== 'string' || keys.has(key)) return false;
        keys.add(key); index = end; whitespace(); if (text[index] !== ':') return false;
        index += 1; if (!parseValue(depth + 1)) return false; whitespace();
        if (text[index] === '}') { index += 1; return true; }
        if (text[index] !== ',') return false; index += 1;
      }
    }
    if (token === '[') {
      index += 1; whitespace(); if (text[index] === ']') { index += 1; return true; }
      for (;;) { if (!parseValue(depth + 1)) return false; whitespace(); if (text[index] === ']') { index += 1; return true; } if (text[index] !== ',') return false; index += 1; }
    }
    if (text.startsWith('true',index)) { index += 4; return true; }
    if (text.startsWith('false',index)) { index += 5; return true; }
    if (text.startsWith('null',index)) { index += 4; return true; }
    const number = /^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/u.exec(text.slice(index));
    if (!number) return false; index += number[0].length; return true;
  };
  if (!parseValue(0)) return { ok: false };
  whitespace(); if (index !== text.length) return { ok: false };
  try { return { ok: true, value: JSON.parse(text) as unknown }; } catch { return { ok: false }; }
}

export function normalizedRatioCurrent(result: Mq135NormalizedResult, elapsedMs: number): boolean {
  const elapsedUs = Math.ceil(elapsedMs * 1000);
  return result.source.fresh === true && result.source.age_us !== null && result.source.max_age_us !== null &&
    Number.isSafeInteger(elapsedUs) && elapsedUs >= 0 &&
    BigInt(result.source.age_us) + BigInt(elapsedUs) <= BigInt(result.source.max_age_us) &&
    result.source.quality === 'READY' && result.adc.available && result.adc.ao_mv !== null &&
    result.response.q !== null && result.response.ratio !== null;
}

export function normalizedEstimateCurrent(result: Mq135NormalizedResult, elapsedMs: number): boolean {
  return normalizedRatioCurrent(result, elapsedMs) && result.quality === 'READY' && result.nh3.status === 'IN_RANGE' && result.nh3.ppm !== null;
}

export function normalizedProfileReferences(result: Mq135NormalizedResult) {
  return {activeHash:result.profiles.hash,activeVersion:result.profiles.version,previousHash:result.profiles.previous_hash,previousVersion:result.profiles.previous_version};
}
