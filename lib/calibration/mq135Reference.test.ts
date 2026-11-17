import { describe, expect, it } from 'vitest';
import { mq135ReferenceParameters } from './mq135Reference';

const operational = {
  response_mode: 'normalized', samples: '3', min_duration_ms: '1000', max_age_ms: '250', max_variation_milli_percent: '5',
  circuit_mv: '5000', divider_top_ohm: '10000', divider_bottom_ohm: '15000', topology: '1', hardware_confirmed: '1',
  reference_confirmed: '1', hardware_identity: 'SYNTHETIC-HW', reference_environment: 'LAB-01', reference_kind: 'operational',
  preparation_confirmed: '0', preparation_method: 'unknown', heating_duration_us: '0',
};

describe('MQ135 normalized reference criteria', () => {
  it('serializes only the released normalized operational contract', () => {
    const parameters = mq135ReferenceParameters(operational);
    expect(parameters).toEqual({
      response_mode: 'normalized', samples: 3, min_duration_ms: 1000, max_age_ms: 250, max_variation_milli_percent: 5000,
      circuit_mv: 5000, divider_top_ohm: 10000, divider_bottom_ohm: 15000, topology: 1, hardware_confirmed: 1,
      reference_confirmed: 1, hardware_identity: 'SYNTHETIC-HW', reference_environment: 'LAB-01', reference_kind: 'operational',
      preparation_confirmed: 0, preparation_method: 'unknown', heating_duration_us: 0,
    });
    expect(parameters).not.toHaveProperty('module_load_ohm');
    expect(parameters).not.toHaveProperty('q0');
    expect(() => mq135ReferenceParameters({...operational, module_load_ohm: '10000'})).toThrow();
    expect(() => mq135ReferenceParameters({...operational, q0: '1'})).toThrow();
  });

  it('omits blank temperature and humidity independently while preserving zero RH', () => {
    expect(mq135ReferenceParameters({...operational, reference_temperature_centi_c: '', reference_humidity_centi_pct: '0'}))
      .not.toHaveProperty('reference_temperature_centi_c');
    expect(mq135ReferenceParameters({...operational, reference_temperature_centi_c: '25', reference_humidity_centi_pct: ''}))
      .toMatchObject({reference_temperature_centi_c: 2500});
    expect(mq135ReferenceParameters({...operational, reference_temperature_centi_c: '', reference_humidity_centi_pct: '0'}))
      .toMatchObject({reference_humidity_centi_pct: 0});
  });

  it('requires explicit physical and sample criteria without inventing defaults', () => {
    expect(() => mq135ReferenceParameters({})).toThrow();
    expect(() => mq135ReferenceParameters({...operational, circuit_mv: ''})).toThrow();
    expect(() => mq135ReferenceParameters({...operational, hardware_confirmed: '0'})).toThrow();
    expect(() => mq135ReferenceParameters({...operational, reference_confirmed: '0'})).toThrow();
    expect(() => mq135ReferenceParameters({...operational, samples: '2'})).toThrow();
  });

  it('requires operator-attested preparation and more than 48 hours for clean-air use', () => {
    const cleanAir = {...operational, reference_kind: 'clean_air_confirmed', preparation_method: 'operator_declared', heating_duration_us: '48'};
    expect(() => mq135ReferenceParameters({...cleanAir, preparation_confirmed: '0'})).toThrow();
    expect(() => mq135ReferenceParameters(cleanAir)).toThrow();
    expect(mq135ReferenceParameters({...cleanAir, heating_duration_us: '48.000001', preparation_confirmed: '1'}))
      .toMatchObject({reference_kind: 'clean_air_confirmed', preparation_confirmed: 1, preparation_method: 'operator_declared'});
    expect(() => mq135ReferenceParameters({...cleanAir, preparation_method: 'unknown', heating_duration_us: '49', preparation_confirmed: '1'})).toThrow();
  });
});
