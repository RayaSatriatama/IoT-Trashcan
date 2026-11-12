import { afterEach, describe, expect, it, vi } from 'vitest';
import { CALIBRATION_SENSOR_IDS, type SensorDiagnostic } from '../../lib/api/calibration';

const runtime = vi.hoisted(() => ({ frame: null as any }));
const changed = (a: readonly unknown[] | undefined, b: readonly unknown[]) => !a || a.length !== b.length || b.some((v, i) => !Object.is(v, a[i]));
vi.mock('react', async load => { const React = await load<any>(); return { ...React,
  useState(initial: any) { const f=runtime.frame, i=f.si++; if (!(i in f.state)) f.state[i]=typeof initial==='function'?initial():initial; return [f.state[i],(v:any)=>{const n=typeof v==='function'?v(f.state[i]):v;if(!Object.is(n,f.state[i])){f.state[i]=n;f.dirty=true;}}]; },
  useRef(initial:any) { const f=runtime.frame; return f.refs[f.ri++] ??= {current:initial}; },
  useEffect(setup:any,deps:any[]) { const f=runtime.frame,i=f.ei++,old=f.effects[i];if(changed(old?.deps,deps))f.effects[i]={deps,setup,cleanup:old?.cleanup,changed:true}; },
}; });
vi.mock('tamagui',()=>{const node=(props:any)=>({props});return {Button:node,Card:node,Input:node,Text:node,XStack:node,YStack:node};});
vi.mock('../../components/SectionHeader',()=>({DescriptionHint:()=>null}));
const api = vi.hoisted(() => ({ getDiagnostics: vi.fn() }));
vi.mock('../../lib/api/calibration', async load => ({...await load<any>(), createCalibrationApi:()=>api}));
import { FirmwareSensorsPanel, sensorValueRows } from '../../components/device/FirmwareSensorsPanel';

const find=(tree:any,predicate:(p:any)=>boolean):any=>{if(!tree||typeof tree!=='object')return;if(tree.props&&predicate(tree.props))return tree;for(const v of Object.values(tree)){const x=find(v,predicate);if(x)return x;}};
const setup=(deviceCode='SIGAP-01')=>{const f:any={state:[],refs:[],effects:[],si:0,ri:0,ei:0,dirty:false};let current=deviceCode;const render=(next=current)=>{current=next;let limit=20;do{if(!limit--)throw Error('effects did not settle');f.si=f.ri=f.ei=0;f.dirty=false;runtime.frame=f;try{f.tree=FirmwareSensorsPanel({deviceCode:current});}finally{runtime.frame=null;}const pending=f.effects.filter((x:any)=>x.changed);pending.forEach((x:any)=>{x.cleanup?.();x.cleanup=undefined;});pending.forEach((x:any)=>{x.changed=false;x.cleanup=x.setup()||undefined;});}while(f.dirty);};render();const findButton=()=>find(f.tree,p=>p.children==='Baca 13 sensor'||p.children==='Membaca…');const submit=()=>findButton().props.onPress();const input=()=>find(f.tree,p=>p['aria-label']==='Otorisasi operator sementara');const unmount=()=>f.effects.forEach((x:any)=>{x.cleanup?.();x.cleanup=undefined;});return {f,render,submit,input,unmount};};
const turns=async()=>{for(let i=0;i<30;i++)await Promise.resolve();};
afterEach(()=>{api.getDiagnostics.mockReset();runtime.frame=null;});

describe('firmware sensor value presentation', () => {
  it('renders schema-shaped converted values with actual units and keeps zero', () => {
    expect(sensorValueRows('sht31_44', { temperature_c: 0, humidity_percent: 0 }, { temperature_ticks: 0, humidity_ticks: 0, temperature_crc_valid: true, humidity_crc_valid: true })).toEqual([
      ['Suhu', '0 °C'], ['Kelembapan', '0 %RH'], ['Tick mentah suhu', '0 ticks'], ['Tick mentah RH', '0 ticks'],
    ]);
  });
  it('covers every canonical MCU sensor identity without converting gas counts to ppm', () => {
    expect(CALIBRATION_SENSOR_IDS).toHaveLength(13);
    expect(sensorValueRows('mq7', { adc_mv: 0 }, { adc_counts: 0 })).toEqual([['ADC', '0 mV'], ['Hitungan ADC', '0 counts']]);
  });
  it('renders all 13 IDs using production telemetry-v3 fixture raw/converted shapes', () => {
    const cases = [
      ['hx711_organic', { kg: 1 }, { counts: 1500 }, [['Massa', '1 kg'], ['Hitungan HX711', '1500 counts']]],
      ['hx711_inorganic', { kg: 0 }, { counts: 0 }, [['Massa', '0 kg'], ['Hitungan HX711', '0 counts']]],
      ['hx711_b3', { kg: 2 }, { counts: -2 }, [['Massa', '2 kg'], ['Hitungan HX711', '-2 counts']]],
      ['ultrasonic_organic', { distance_mm: 1700, fill_percent: 50 }, { echo_us: 1000 }, [['Jarak', '1700 mm'], ['Echo', '1000 µs']]],
      ['ultrasonic_inorganic', { distance_mm: 0 }, { echo_us: 0 }, [['Jarak', '0 mm'], ['Echo', '0 µs']]],
      ['ultrasonic_b3', null, null, [['Jarak', '—'], ['Echo', '—']]],
      ['mq4', null, { adc_counts: 111 }, [['ADC', '—'], ['Hitungan ADC', '111 counts']]],
      ['mq135', { adc_mv: 0 }, { adc_counts: 0 }, [['ADC', '0 mV'], ['Hitungan ADC', '0 counts']]],
      ['mq7', null, { adc_counts: 900, heater_phase: 'PREHEAT' }, [['ADC', '—'], ['Hitungan ADC', '900 counts']]],
      ['mq8', { adc_mv: 1234 }, { adc_counts: 1234 }, [['ADC', '1234 mV'], ['Hitungan ADC', '1234 counts']]],
      ['ds18b20_b3', { temperature_c: 85 }, { conversion_complete: true, crc_valid: true, rom: '280102030405069e' }, [['Suhu', '85 °C'], ['Konversi selesai', 'true '], ['CRC scratchpad', 'true '], ['Identitas ROM', '280102030405069e ']]],
      ['sht31_44', { temperature_c: 26, humidity_percent: 61 }, { temperature_ticks: 30000, humidity_ticks: 40000, temperature_crc_valid: true, humidity_crc_valid: true }, [['Suhu', '26 °C'], ['Kelembapan', '61 %RH'], ['Tick mentah suhu', '30000 ticks'], ['Tick mentah RH', '40000 ticks']]],
      ['sht31_45', { temperature_c: 0, humidity_percent: 0 }, { temperature_ticks: 0, humidity_ticks: 0, temperature_crc_valid: false, humidity_crc_valid: true }, [['Suhu', '0 °C'], ['Kelembapan', '0 %RH'], ['Tick mentah suhu', '0 ticks'], ['Tick mentah RH', '0 ticks']]],
    ] as const;
    expect(cases.map(([sensor, converted, raw]) => [sensor, sensorValueRows(sensor, converted, raw)])).toEqual(
      cases.map(([sensor, , , expected]) => [sensor, expected]),
    );
    expect(cases.map(([sensor]) => sensor)).toEqual(CALIBRATION_SENSOR_IDS);
  });
  it('does not render mismatched value keys as a measurement', () => {
    expect(sensorValueRows('mq7', { temperature_c: 25 }, { counts: 12 })).toEqual([['ADC', '—'], ['Hitungan ADC', '—']]);
  });
});

describe('FirmwareSensorsPanel submit/effect/handler lifecycle (controlled hook driver, not DOM)',()=>{
  it('does not autofetch, masks and clears authorization on submit, and requests exactly all canonical IDs',async()=>{
    api.getDiagnostics.mockResolvedValue({diagnostic:null});const ui=setup();
    expect(api.getDiagnostics).not.toHaveBeenCalled();expect(ui.input().props.secureTextEntry).toBe(true);
    ui.input().props.onChangeText('temporary-secret');ui.render();ui.submit();await turns();ui.render();
    expect(ui.input().props.value).toBe('');expect(api.getDiagnostics).toHaveBeenCalledTimes(13);
    expect(api.getDiagnostics.mock.calls.map((x:any[])=>x.slice(0,2))).toEqual(CALIBRATION_SENSOR_IDS.map(sensor=>['SIGAP-01',sensor]));
    expect(api.getDiagnostics.mock.calls.every((x:any[])=>x[2] instanceof AbortSignal)).toBe(true);
  });
  it('cleans busy after an admitted 401 aborts sibling requests',async()=>{
    let reject!: (e:any)=>void;api.getDiagnostics.mockImplementation((_d:string,_s:string,signal:AbortSignal)=>new Promise((_resolve,r)=>{if(_s==='mq4')reject=r;else signal.addEventListener('abort',()=>{}, {once:true});}));
    const ui=setup();ui.input().props.onChangeText('token');ui.render();ui.submit();await turns();reject({status:401});await turns();ui.render();
    expect(find(ui.f.tree,p=>p.role==='alert')?.props.children).toMatch(/Otorisasi/);
    expect(find(ui.f.tree,p=>p.children==='Baca 13 sensor')).toBeTruthy();
  });
  it('retained old submit closure cannot consume replacement token or dispatch',async()=>{
    const ui=setup();const old=find(ui.f.tree,p=>p.children==='Baca 13 sensor').props.onPress;ui.render('SIGAP-02');ui.input().props.onChangeText('new-token');ui.render();old();await turns();
    expect(api.getDiagnostics).not.toHaveBeenCalled();expect(ui.input().props.value).toBe('new-token');
  });
  it('replacement cleanup aborts old work without clearing new context input',async()=>{
    let signal!:AbortSignal;api.getDiagnostics.mockImplementation((_d:string,_s:string,s:AbortSignal)=>new Promise((_resolve,reject)=>{signal=s;s.addEventListener('abort',()=>reject(new Error('aborted')),{once:true});}));
    const ui=setup();ui.input().props.onChangeText('old-token');ui.render();ui.submit();await turns();
    ui.render('SIGAP-02');ui.input().props.onChangeText('new-token');ui.render();expect(signal.aborted).toBe(true);await turns();ui.render();
    expect(ui.input().props.value).toBe('new-token');expect(find(ui.f.tree,p=>p.children==='Baca 13 sensor')).toBeTruthy();
  });
  it.each([
    ['network failure', async()=>{throw new Error('offline');}],
    ['server failure', async()=>{throw {status:503};}],
    ['sensor identity mismatch', async(_d:string,sensor:string)=>({diagnostic:{sensor_id:sensor==='mq4'?'mq135':sensor,quality:'TEST_ONLY'}})],
  ])('surfaces %s as a read failure rather than unavailable',async(_label,implementation)=>{
    api.getDiagnostics.mockImplementation(implementation as any);
    const ui=setup();ui.input().props.onChangeText('token');ui.render();ui.submit();await turns();ui.render();
    expect(find(ui.f.tree,p=>p.role==='alert')?.props.children).toContain('Pembacaan gagal');
    expect(find(ui.f.tree,p=>p.children==='Tidak tersedia')).toBeUndefined();
    expect(find(ui.f.tree,p=>p.children==='Baca 13 sensor')).toBeTruthy();
  });
  it('renders visible archive age and uncertainty on each sensor row', async()=>{
    api.getDiagnostics.mockImplementation(async(_device:string,sensor:string)=>({diagnostic:{sensor_id:sensor,quality:sensor==='mq4'?'STALE':'READY',reason:'TEST',raw:{adc_counts:0},converted_value:{adc_mv:0},fresh:sensor==='mq4'?false:null,age_ms:1250,ordering_unknown:sensor==='mq4',future_clock_skew:false,sample_origin:'UNKNOWN'} as any}));
    const ui=setup();ui.input().props.onChangeText('token');ui.render();ui.submit();await turns();ui.render();
    expect(find(ui.f.tree,p=>p.children==='Urutan lintas boot tidak diketahui')).toBeTruthy();
    expect(find(ui.f.tree,p=>p.children==='Kualitas tidak segar · 1250 ms')).toBeTruthy();
    expect(find(ui.f.tree,p=>p.children==='Kesegaran belum ditetapkan · 1250 ms')).toBeTruthy();
    expect(find(ui.f.tree,p=>Array.isArray(p.children)&&p.children.join('')==='ADC: 0 mV')).toBeTruthy();
  });
  it('keeps one missing 404 sensor local and does not promote simulated metadata to physical evidence',async()=>{
    api.getDiagnostics.mockImplementation(async(_d:string,sensor:string)=>{if(sensor==='mq4')throw {status:404};return {diagnostic:{sensor_id:sensor,quality:'TEST_ONLY',sample_origin:'SIMULATED',raw:{adc_counts:0},converted_value:{adc_mv:0}} as any};});
    const ui=setup();ui.input().props.onChangeText('token');ui.render();ui.submit();await turns();ui.render();
    expect(find(ui.f.tree,p=>p.children==='Tidak tersedia')).toBeTruthy();
    expect(find(ui.f.tree,p=>Array.isArray(p.children)&&p.children.join('')==='ADC: 0 mV')).toBeTruthy();
    expect(find(ui.f.tree,p=>String(p.children).includes('Simulasi sensor firmware'))).toBeTruthy();
    expect(find(ui.f.tree,p=>String(p.children).includes('Fisik (metadata firmware)'))).toBeUndefined();
  });
  it('aborts on unmount and ignores late success',async()=>{
    let signal!:AbortSignal;let finish!: (v:any)=>void;api.getDiagnostics.mockImplementation((_d:string,_s:string,s:AbortSignal)=>{signal=s;return new Promise(r=>{finish=r;});});
    const ui=setup();ui.input().props.onChangeText('token');ui.render();ui.submit();await turns();ui.unmount();
    expect(signal.aborted).toBe(true);const before=[...ui.f.state];finish({diagnostic:null});await turns();
    expect(ui.f.state).toEqual(before);
  });
});
