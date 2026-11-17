export type Mq135ReferenceField = {
  name: string;
  label: string;
  unit: string;
  scale: number;
  group: 'reference' | 'electrical' | 'criteria';
  confirmation?: boolean;
  text?: boolean;
  optional?: boolean;
  choices?: readonly string[];
};

export type Mq135NormalizedCriteria = {
  response_mode: 'normalized';
  samples: number;
  min_duration_ms: number;
  max_age_ms: number;
  max_variation_milli_percent: number;
  circuit_mv: number;
  divider_top_ohm: number;
  divider_bottom_ohm: number;
  topology: 1 | 2;
  hardware_confirmed: 1;
  reference_confirmed: 1;
  hardware_identity: string;
  reference_environment: string;
  reference_kind: 'operational' | 'clean_air_confirmed';
  preparation_confirmed: 0 | 1;
  preparation_method: 'unknown' | 'operator_declared';
  heating_duration_us: number;
  reference_temperature_centi_c?: number;
  reference_humidity_centi_pct?: number;
};

export const MQ135_REFERENCE_FIELDS: readonly Mq135ReferenceField[] = [
  {name:'hardware_identity',label:'Identitas sensor',unit:'kode tanpa spasi',scale:1,group:'reference',text:true},
  {name:'reference_environment',label:'Kondisi referensi',unit:'kode tanpa spasi',scale:1,group:'reference',text:true},
  {name:'reference_kind',label:'Jenis referensi',unit:'',scale:1,group:'reference',choices:['operational','clean_air_confirmed']},
  {name:'reference_temperature_centi_c',label:'Suhu referensi',unit:'°C',scale:100,group:'reference',optional:true},
  {name:'reference_humidity_centi_pct',label:'Kelembapan referensi',unit:'% RH',scale:100,group:'reference',optional:true},
  {name:'reference_confirmed',label:'Kondisi referensi telah dikonfirmasi',unit:'',scale:1,group:'reference',confirmation:true},
  {name:'preparation_method',label:'Metode persiapan',unit:'',scale:1,group:'reference',choices:['unknown','operator_declared']},
  {name:'preparation_confirmed',label:'Persiapan telah dikonfirmasi operator',unit:'',scale:1,group:'reference',confirmation:true},
  {name:'heating_duration_us',label:'Pemanasan aktual',unit:'jam',scale:3600000000,group:'reference'},
  {name:'circuit_mv',label:'Tegangan rangkaian',unit:'mV',scale:1,group:'electrical'},
  {name:'divider_top_ohm',label:'Resistor pembagi atas',unit:'Ω',scale:1,group:'electrical'},
  {name:'divider_bottom_ohm',label:'Resistor pembagi bawah',unit:'Ω',scale:1,group:'electrical'},
  {name:'topology',label:'Topologi AO',unit:'',scale:1,group:'electrical',choices:['1','2']},
  {name:'hardware_confirmed',label:'Rangkaian dan nilai resistor telah diverifikasi',unit:'',scale:1,group:'electrical',confirmation:true},
  {name:'samples',label:'Jumlah sampel referensi',unit:'3–64 sampel',scale:1,group:'criteria'},
  {name:'min_duration_ms',label:'Durasi pengambilan minimum',unit:'ms',scale:1,group:'criteria'},
  {name:'max_age_ms',label:'Umur sampel maksimum',unit:'ms',scale:1,group:'criteria'},
  {name:'max_variation_milli_percent',label:'Variasi maksimum',unit:'%',scale:1000,group:'criteria'},
];

const UINT32_MAX = 4294967295;
const MAX_DURATION_MS = Number.MAX_SAFE_INTEGER;
const MIN_CLEAN_AIR_PREHEAT_US = 172800000000;
const LABEL = /^[A-Za-z0-9_.:-]{1,31}$/u;
const DECIMAL = /^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?$/u;

export function mq135ReferenceParameters(values: Record<string,string>): Mq135NormalizedCriteria {
  const allowed = new Set([...MQ135_REFERENCE_FIELDS.map(field => field.name), 'response_mode']);
  if (Object.keys(values).some(name => !allowed.has(name))) throw new Error('Kriteria MQ135 memuat field yang tidak didukung.');
  if (values.response_mode !== 'normalized') throw new Error('Mode respons MQ135 harus normalized.');
  const read = (name: string): string => {
    const raw = values[name];
    if (typeof raw !== 'string' || raw === '') throw new Error(`${MQ135_REFERENCE_FIELDS.find(field=>field.name===name)?.label ?? name} wajib diisi.`);
    return raw;
  };
  const text = (name: string): string => {
    const raw = read(name);
    if (!LABEL.test(raw)) throw new Error(`${name}: gunakan 1–31 huruf, angka, titik, garis bawah, titik dua, atau tanda hubung.`);
    return raw;
  };
  const choice = <T extends string>(name: string, choices: readonly T[]): T => {
    const raw = read(name);
    if (!choices.includes(raw as T)) throw new Error(`${MQ135_REFERENCE_FIELDS.find(field=>field.name===name)?.label ?? name} tidak valid.`);
    return raw as T;
  };
  const number = (name: string, min: number, max: number, optional = false): number | undefined => {
    const raw = values[name];
    if ((raw === '' || raw === undefined) && optional) return undefined;
    if (typeof raw !== 'string' || !DECIMAL.test(raw)) throw new Error(`${MQ135_REFERENCE_FIELDS.find(field=>field.name===name)?.label ?? name} wajib diisi.`);
    const field = MQ135_REFERENCE_FIELDS.find(item=>item.name===name)!;
    const scaled = Number(raw) * field.scale;
    if (!Number.isSafeInteger(scaled) || scaled < min || scaled > max) throw new Error(`${field.label} tidak valid atau di luar batas.`);
    return scaled;
  };
  const confirmation = (name: string): 0 | 1 => {
    const raw = read(name);
    if (raw !== '0' && raw !== '1') throw new Error(`${MQ135_REFERENCE_FIELDS.find(field=>field.name===name)?.label ?? name} tidak valid.`);
    return Number(raw) as 0 | 1;
  };
  const referenceKind = choice('reference_kind', ['operational','clean_air_confirmed'] as const);
  const preparationMethod = choice('preparation_method', ['unknown','operator_declared'] as const);
  const preparationConfirmed = confirmation('preparation_confirmed');
  const heatingDuration = number('heating_duration_us',0,Number.MAX_SAFE_INTEGER)!;
  const hardwareConfirmed = confirmation('hardware_confirmed');
  const referenceConfirmed = confirmation('reference_confirmed');
  if (hardwareConfirmed !== 1) throw new Error('Verifikasi rangkaian wajib dikonfirmasi.');
  if (referenceConfirmed !== 1) throw new Error('Kondisi referensi wajib dikonfirmasi.');
  if (preparationConfirmed === 1 && preparationMethod !== 'operator_declared') throw new Error('Persiapan hanya dapat dikonfirmasi sebagai deklarasi operator.');
  if (referenceKind === 'clean_air_confirmed' &&
      (preparationConfirmed !== 1 || preparationMethod !== 'operator_declared' || heatingDuration <= MIN_CLEAN_AIR_PREHEAT_US)) {
    throw new Error('Referensi udara bersih memerlukan persiapan operator dan pemanasan aktual lebih dari 48 jam.');
  }

  const parameters: Mq135NormalizedCriteria = {
    response_mode: 'normalized',
    samples: number('samples',3,64)!,
    min_duration_ms: number('min_duration_ms',1,MAX_DURATION_MS)!,
    max_age_ms: number('max_age_ms',1,MAX_DURATION_MS)!,
    max_variation_milli_percent: number('max_variation_milli_percent',1,100000)!,
    circuit_mv: number('circuit_mv',1,UINT32_MAX)!,
    divider_top_ohm: number('divider_top_ohm',1,UINT32_MAX)!,
    divider_bottom_ohm: number('divider_bottom_ohm',1,UINT32_MAX)!,
    topology: Number(choice('topology',['1','2'] as const)) as 1 | 2,
    hardware_confirmed: hardwareConfirmed as 1,
    reference_confirmed: referenceConfirmed as 1,
    hardware_identity: text('hardware_identity'),
    reference_environment: text('reference_environment'),
    reference_kind: referenceKind,
    preparation_confirmed: preparationConfirmed,
    preparation_method: preparationMethod,
    heating_duration_us: heatingDuration,
  };
  const temperature = number('reference_temperature_centi_c',-2147483648,2147483647,true);
  const humidity = number('reference_humidity_centi_pct',0,10000,true);
  if (temperature !== undefined) parameters.reference_temperature_centi_c = temperature;
  if (humidity !== undefined) parameters.reference_humidity_centi_pct = humidity;
  return parameters;
}

