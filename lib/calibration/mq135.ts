import type { CalibrationQuality } from '../api/calibration';
import {
 decodeMq135NormalizedResult,
 normalizedEstimateCurrent,
 normalizedProfileReferences,
 normalizedRatioCurrent,
 parseJsonUnique,
 type Mq135NormalizedResult,
} from './mq135Normalized';

/** Device-authored control evidence. This is not telemetry-v3 or NH3 ppm. */
export type Mq135LegacyGasResult = {
 schema: 'MQG1'; sensor: 'mq135'; quality: CalibrationQuality; reason: string;
 raw_counts: number | null; adc_method: 'PROFILE_LINEAR'; adc_pin_mv: number | null; ao_mv: number | null;
 rs_ohm: number | null; r0_ohm: number | null; rs_over_r0: number | null;
 nh3_ppm: null; nh3_status: 'UNCALIBRATED'; profile_state: 'MISSING' | 'INVALID' | 'TEST_ONLY' | 'VALID';
 profile_version: string | null; profile_hash: string | null; previous_profile_version: string | null; previous_profile_hash: string | null;
 adc_profile_version: string | null; adc_profile_hash: string | null;
 source_sequence: string | null; source_boot_id: string | null; source_monotonic_us: string | null;
 response_monotonic_us: string; age_us: string | null; max_age_us: string | null; fresh: boolean | null;
 hardware_identity: string | null; reference_environment: string | null;
 reference_temperature_centi_c: number | null; reference_humidity_centi_pct: number | null;
 calibration_epoch_ms: string | null; calibration_time_quality: 'UNSYNCED' | 'SYNCHRONIZED' | 'ESTIMATED';
 sample_count: number | null; variation_percent: number | null;
 circuit_mv: number | null; module_load_ohm: number | null; divider_top_ohm: number | null; divider_bottom_ohm: number | null; topology: 1 | 2 | null;
};
export type Mq135GasResult = Mq135LegacyGasResult | Mq135NormalizedResult;
const FIELDS = 'schema sensor quality reason raw_counts adc_method adc_pin_mv ao_mv rs_ohm r0_ohm rs_over_r0 nh3_ppm nh3_status profile_state profile_version profile_hash previous_profile_version previous_profile_hash adc_profile_version adc_profile_hash source_sequence source_boot_id source_monotonic_us response_monotonic_us age_us max_age_us fresh hardware_identity reference_environment reference_temperature_centi_c reference_humidity_centi_pct calibration_epoch_ms calibration_time_quality sample_count variation_percent circuit_mv module_load_ohm divider_top_ohm divider_bottom_ohm topology'.split(' ');
const HASH = /^sha256:[0-9a-f]{64}$/u;
const LABEL = /^[A-Za-z0-9_.:-]{1,31}$/u;
function uint(value: unknown, bits = 64): value is string {
 return typeof value === 'string' && /^(?:0|[1-9][0-9]{0,19})$/u.test(value) && BigInt(value) < (1n << BigInt(bits));
}
function number(value: unknown, min: number, max: number, integer = false): value is number {
 return typeof value === 'number' && Number.isFinite(value) && value >= min && value <= max && (!integer || Number.isInteger(value));
}
function choice(value: unknown, allowed: string): boolean { return typeof value === 'string' && allowed.split('|').includes(value); }
export function decodeMq135GasResult(value: unknown): Mq135GasResult | null {
 if (typeof value !== 'object' || value === null || Array.isArray(value)) return null;
 const schema = (value as Record<string, unknown>).schema;
 if (schema === 'MQG1') return decodeMq135LegacyGasResult(value);
 if (schema === 'MQG2') return decodeMq135NormalizedResult(value);
 return null;
}

function decodeMq135LegacyGasResult(value: unknown): Mq135LegacyGasResult | null {
 if (typeof value !== 'object' || value === null || Array.isArray(value)) return null;
 const v = value as Record<string, unknown>;
 if (Object.keys(v).length !== FIELDS.length || FIELDS.some(key => !Object.hasOwn(v,key))) return null;
 if (v.schema !== 'MQG1' || v.sensor !== 'mq135' || v.adc_method !== 'PROFILE_LINEAR' || v.nh3_ppm !== null || v.nh3_status !== 'UNCALIBRATED') return null;
 if (!choice(v.quality,'READY|CALIBRATING|WARMING_UP|HEATER_PHASE|UNCALIBRATED|STALE|TIMEOUT|DISCONNECTED|CRC_ERROR|OUT_OF_RANGE|INVALID') ||
     !choice(v.reason,'NONE|GAS_PROFILE_MISSING|NO_SOURCE|RAW_MISSING|SOURCE_IDENTITY_INVALID|ADC_UNVERIFIED|ADC_PROFILE_CHANGED|SOURCE_NOT_READY|SOURCE_CLOCK_UNKNOWN|SOURCE_STALE|GAS_PROFILE_INVALID|REFERENCE_PENDING|ADC_SATURATED|DIVIDER_MISMATCH|AO_OUT_OF_RANGE|ELECTRICAL_UNVERIFIED|ELECTRICAL_OR_SOURCE_INVALID') ||
     !choice(v.profile_state,'MISSING|INVALID|TEST_ONLY|VALID') || !choice(v.calibration_time_quality,'UNSYNCED|SYNCHRONIZED|ESTIMATED')) return null;
 for (const key of ['raw_counts','adc_pin_mv','ao_mv','rs_ohm','r0_ohm','rs_over_r0','variation_percent','circuit_mv','module_load_ohm','divider_top_ohm','divider_bottom_ohm']) {
  if (v[key] !== null && !number(v[key],0,Number.MAX_VALUE)) return null;
 }
 if (v.raw_counts !== null && !number(v.raw_counts,0,65535,true)) return null;
 for (const key of ['rs_ohm','r0_ohm','rs_over_r0','circuit_mv','module_load_ohm','divider_top_ohm','divider_bottom_ohm']) {
  if (v[key] !== null && (v[key] as number) <= 0) return null;
 }
 for (const [version,hash] of [['profile_version','profile_hash'],['previous_profile_version','previous_profile_hash'],['adc_profile_version','adc_profile_hash']]) {
  if ((v[version] === null) !== (v[hash] === null)) return null;
  if (v[version] !== null && (!uint(v[version],32) || v[version] === '0' || typeof v[hash] !== 'string' || !HASH.test(v[hash]))) return null;
 }
 for (const key of ['source_sequence','source_monotonic_us','response_monotonic_us','age_us','max_age_us']) if (v[key] !== null && !uint(v[key])) return null;
 if (!uint(v.response_monotonic_us)) return null;
 if (v.source_boot_id !== null && (typeof v.source_boot_id !== 'string' || !/^[0-9a-f]{32}$/u.test(v.source_boot_id))) return null;
 if ((v.source_sequence === null) !== (v.source_monotonic_us === null)) return null;
 if (v.fresh !== null) {
  if (typeof v.fresh !== 'boolean' || !uint(v.age_us) || !uint(v.max_age_us) || v.max_age_us === '0' || !uint(v.source_monotonic_us) || v.source_boot_id === null) return null;
  const response = BigInt(v.response_monotonic_us), source = BigInt(v.source_monotonic_us), age = BigInt(v.age_us);
  if (response < source || response-source !== age || v.fresh !== (age <= BigInt(v.max_age_us))) return null;
 } else if (v.max_age_us !== null) return null;
 for (const key of ['hardware_identity','reference_environment']) if (v[key] !== null && (typeof v[key] !== 'string' || !LABEL.test(v[key]))) return null;
 if (v.reference_temperature_centi_c !== null && !number(v.reference_temperature_centi_c,-2147483648,2147483647,true)) return null;
 if (v.reference_humidity_centi_pct !== null && !number(v.reference_humidity_centi_pct,0,10000,true)) return null;
 if (v.sample_count !== null && !number(v.sample_count,3,64,true)) return null;
 if (v.topology !== null && ![1,2].includes(v.topology as number)) return null;
 if (v.calibration_epoch_ms !== null) {
  if (typeof v.calibration_epoch_ms !== 'string' || !/^(?:0|-?[1-9][0-9]{0,18})$/u.test(v.calibration_epoch_ms) || v.calibration_time_quality === 'UNSYNCED') return null;
  const epoch = BigInt(v.calibration_epoch_ms); if (epoch < -(1n << 63n) || epoch >= (1n << 63n)) return null;
 }
 const profile = v.profile_state === 'VALID' || v.profile_state === 'TEST_ONLY';
 if (['profile_version','r0_ohm','sample_count','variation_percent'].some(key => (v[key] !== null) !== profile)) return null;
 if (v.quality === 'READY' && (v.reason !== 'NONE' || !profile || v.fresh !== true || v.rs_over_r0 === null || v.adc_profile_hash === null)) return null;
 if (v.rs_over_r0 !== null) {
  if (v.quality !== 'READY' || v.rs_ohm === null || v.r0_ohm === null || v.raw_counts === null || v.ao_mv === null || v.adc_pin_mv === null) return null;
  const ratio = v.rs_over_r0 as number;
  if (Math.abs((v.rs_ohm as number)/(v.r0_ohm as number)-ratio) > 1e-5*Math.max(1,ratio)) return null;
 }
 return value as Mq135LegacyGasResult;
}

/** Parse raw JSON before JSON.parse so nested and escaped duplicate keys are rejected. */
export function parseMq135GasJson(text: string): Mq135GasResult | null {
 const parsed = parseJsonUnique(text);
 return parsed.ok ? decodeMq135GasResult(parsed.value) : null;
}

/** Conservative age bound measured from dispatch of a NEW read request.
 * Never call this with the receipt time of an old persisted command. */
export function mq135RatioCurrent(result: Mq135GasResult, elapsedMs: number): boolean {
 if (result.schema === 'MQG2') return normalizedRatioCurrent(result, elapsedMs);
 const elapsedUs = Math.ceil(elapsedMs*1000);
 return result.quality === 'READY' && result.rs_over_r0 !== null && result.fresh === true &&
  result.age_us !== null && result.max_age_us !== null && Number.isSafeInteger(elapsedUs) && elapsedUs >= 0 &&
  BigInt(result.age_us)+BigInt(elapsedUs) <= BigInt(result.max_age_us);
}

export function mq135Nh3EstimateCurrent(result: Mq135GasResult, elapsedMs: number): boolean {
 return result.schema === 'MQG2' && normalizedEstimateCurrent(result, elapsedMs);
}

export function mq135EvidenceReady(result: Mq135GasResult): boolean {
 return result.schema === 'MQG1'
  ? result.raw_counts !== null && result.adc_profile_hash !== null && ['READY','UNCALIBRATED','CALIBRATING'].includes(result.quality) && result.fresh !== false
  : result.adc.raw_counts !== null && result.profiles.adc_hash !== null && ['READY','UNCALIBRATED','CALIBRATING'].includes(result.quality) && result.source.fresh !== false;
}

export function mq135ProfileReferences(result: Mq135GasResult) {
 return result.schema === 'MQG1'
  ? {activeHash:result.profile_hash,activeVersion:result.profile_version,previousHash:result.previous_profile_hash,previousVersion:result.previous_profile_version}
  : normalizedProfileReferences(result);
}
