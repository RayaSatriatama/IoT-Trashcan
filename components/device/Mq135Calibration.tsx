import React, { useEffect, useState } from 'react';
import { Input, Text, XStack, YStack } from 'tamagui';
import { AppButton } from '../AppButton';
import { DescriptionHint } from '../SectionHeader';
import { textStyles } from '../../lib/ui/typography';
import { mq135Nh3EstimateCurrent, mq135ProfileReferences, mq135RatioCurrent, type Mq135GasResult } from '../../lib/calibration/mq135';
import { MQ135_REFERENCE_FIELDS } from '../../lib/calibration/mq135Reference';

const display = (value:number|null|undefined,unit='') => value === null || value === undefined ? 'Tidak tersedia' : `${value.toLocaleString('id-ID',{maximumFractionDigits:3})}${unit ? ` ${unit}`:''}`;
const qualityLabels:Record<string,string>={READY:'Respons relatif siap',UNCALIBRATED:'Referensi belum tersedia',INVALID:'Pembacaan tidak valid',WARMING_UP:'Pemanasan',CALIBRATING:'Mengambil referensi',STALE:'Data kedaluwarsa',DISCONNECTED:'Sensor tidak terhubung',TIMEOUT:'Waktu pembacaan habis',CRC_ERROR:'Bukti tidak valid',OUT_OF_RANGE:'Di luar rentang',HEATER_PHASE:'Fase pemanasan',UNAVAILABLE:'Data tidak tersedia'};
const reasonLabels:Record<string,string>={
  ADC_UNVERIFIED:'Profil ADC dengan bukti pengukuran belum tersedia.',ADC_PROFILE_CHANGED:'Profil ADC berubah. Referensi perlu diperbarui.',
  ELECTRICAL_UNVERIFIED:'Rangkaian listrik belum diverifikasi.',DIVIDER_MISMATCH:'Konfigurasi pembagi tidak cocok.',
  SOURCE_CLOCK_UNKNOWN:'Domain waktu sumber tidak dapat dikonfirmasi.',ADC_SATURATED:'ADC mencapai batas ukur.',
  AO_OUT_OF_RANGE:'Tegangan AO di luar rentang rangkaian.',GAS_PROFILE_MISSING:'Referensi R0 belum tersimpan.',
  GAS_PROFILE_INVALID:'Profil gas tidak valid.',RAW_MISSING:'Bukti raw belum tersedia.',SOURCE_IDENTITY_INVALID:'Identitas sumber tidak valid.',
  NO_SOURCE:'Sampel perangkat belum tersedia.',SOURCE_NOT_READY:'Sumber belum siap untuk perhitungan.',
  SOURCE_STALE:'Umur sampel melewati batas yang ditetapkan.',REFERENCE_PENDING:'Referensi sedang dikumpulkan.',
  ELECTRICAL_OR_SOURCE_INVALID:'Periksa rangkaian dan kualitas sumber.',NO_NORMALIZED_PROFILE:'Profil respons belum tersedia.',
  NORMALIZED_PROFILE_INVALID:'Profil respons tidak valid.',ADC_CALIBRATION_UNAVAILABLE:'Kalibrasi ADC tidak tersedia.',
  BASELINE_MISSING:'Baseline q0 belum tersedia.',REFERENCE_NOT_CLEAN_AIR:'Referensi bukan udara bersih terkonfirmasi.',
  PREHEAT_UNCONFIRMED:'Persiapan dan pemanasan udara bersih belum memenuhi syarat.',INVALID_RATIO:'Rasio respons tidak valid.',
  CURVE_INVALID:'Kurva riset tidak valid.',CURVE_UNAVAILABLE:'Kurva riset tidak tersedia.',BELOW_RANGE:'Respons di luar rentang bawah kurva riset.',
  ABOVE_RANGE:'Respons di luar rentang atas kurva riset.',IN_RANGE:'Respons berada dalam rentang kurva riset.',
};
function Metric({label,value}:{label:string;value:string}) { return <YStack minWidth={140} flex={1} gap="$1"><Text color="$colorMuted" {...textStyles.caption}>{label}</Text><Text color="$color" {...textStyles.bodySecondary}>{value}</Text></YStack>; }

export function Mq135GasDiagnostics({value,startedAtMs}:{value:Mq135GasResult;startedAtMs:number}) {
  const [now,setNow]=useState(()=>performance.now());
  useEffect(()=>{const timer=globalThis.setInterval(()=>setNow(performance.now()),250);return()=>globalThis.clearInterval(timer);},[]);
  const elapsed=Math.max(0,now-startedAtMs);
  const elapsedUs=Math.ceil(elapsed*1000);
  const ageUs=value.schema==='MQG1'?value.age_us:value.source.age_us;
  const maxAgeUs=value.schema==='MQG1'?value.max_age_us:value.source.max_age_us;
  const sourceFresh=value.schema==='MQG1'?value.fresh:value.source.fresh;
  const currentAgeUs=ageUs!==null&&Number.isSafeInteger(elapsedUs)?(BigInt(ageUs)+BigInt(elapsedUs)).toString():null;
  const withinAge=ageUs!==null&&maxAgeUs!==null&&Number.isSafeInteger(elapsedUs)&&elapsedUs>=0&&BigInt(ageUs)+BigInt(elapsedUs)<=BigInt(maxAgeUs);
  const current=mq135RatioCurrent(value,elapsed);
  const expired=sourceFresh===true&&ageUs!==null&&maxAgeUs!==null&&!withinAge;
  const quality=expired?'Data kedaluwarsa':qualityLabels[value.quality]??value.quality;
  const epochToken=value.schema==='MQG1'?value.calibration_epoch_ms:value.reference.calibration_epoch_ms;
  const timeQuality=value.schema==='MQG1'?value.calibration_time_quality:value.reference.calibration_time_quality;
  const epoch=epochToken===null?null:Number(epochToken);
  const date=epoch!==null&&Number.isSafeInteger(epoch)&&Math.abs(epoch)<=8640000000000000?new Date(epoch).toLocaleString('id-ID'):null;
  const profileVersion=value.schema==='MQG1'?value.profile_version:value.profiles.version;
  const referenceEnvironment=value.schema==='MQG1'?value.reference_environment:value.reference.environment;
  const temperature=value.schema==='MQG1'?value.reference_temperature_centi_c:value.reference.temperature_centi_c;
  const humidity=value.schema==='MQG1'?value.reference_humidity_centi_pct:value.reference.humidity_centi_pct;
  const sourceQuality=value.schema==='MQG2'?value.source.quality:null;
  const conditionsSource=value.schema==='MQG2'?value.reference.conditions_source:null;
  const heatingDuration=value.schema==='MQG2'?value.reference.heating_duration_us:null;
  const preparationConfirmed=value.schema==='MQG2'?value.reference.preparation_confirmed:null;
  const preparationMethod=value.schema==='MQG2'?value.reference.preparation_method:null;
  const sampleCount=value.schema==='MQG1'?value.sample_count:value.reference.sample_count;
  const variation=value.schema==='MQG1'?value.variation_percent:value.reference.variation_percent;
  const rawCounts=value.schema==='MQG1'?value.raw_counts:value.adc.raw_counts;
  const pinMv=value.schema==='MQG1'?value.adc_pin_mv:value.adc.pin_mv;
  const aoMv=value.schema==='MQG1'?value.ao_mv:value.adc.ao_mv;
  const nh3Estimate=value.schema==='MQG2'&&mq135Nh3EstimateCurrent(value,elapsed)?value.nh3.ppm:null;
  const references=value.schema==='MQG2'?mq135ProfileReferences(value):null;
  return <YStack gap="$3">
    <XStack justifyContent="space-between" alignItems="center" gap="$2"><Text role="status" color={current?'$statusNormal':'$statusWaspada'}>{quality}</Text><DescriptionHint ariaLabel="Info respons gas relatif" description="Rs/R0 dan q/q0 adalah respons relatif, bukan konsentrasi gas. Winsen MQ-135 v1.6 Figure 3 memplot H2, Air, NH3, dan Toluene; manual juga mencatat respons terhadap sulfida dan uap seri benzena. Ini menunjukkan sensitivitas lintas-gas, bukan selektivitas NH3. PPM hanya ditampilkan jika firmware memberi status IN_RANGE dan bukti masih segar; hasil tetap TEST_ONLY."/></XStack>
    {value.reason!=='NONE'?<Text color="$colorMuted" {...textStyles.bodySecondary}>{reasonLabels[value.reason]??'Periksa kualitas bukti perangkat.'}</Text>:null}
    {value.schema==='MQG1'?<Text color="$statusWaspada">MQG1 legasi: bukti Rs/R0 perangkat lama, bukan MQG2.</Text>:null}
    {value.schema==='MQG1'&&value.profile_state==='TEST_ONLY'?<Text color="$statusWaspada">TEST_ONLY, bukan kalibrasi produksi.</Text>:null}
    {value.schema==='MQG2'&&value.profiles.state==='TEST_ONLY'?<Text color="$statusWaspada">TEST_ONLY, bukan kalibrasi produksi.</Text>:null}
    <Text color="$colorMuted" {...textStyles.caption}>Snapshot perangkat. Umur sekarang menambahkan umur sumber dengan waktu sejak permintaan baru dikirim; respons cache tidak diberi cap waktu baru.</Text>
    <Text color="$color" {...textStyles.sectionTitle}>Data Raw</Text>
    <XStack gap="$3" flexWrap="wrap"><Metric label="ADC" value={display(rawCounts,'counts')}/><Metric label="Tegangan pin ADC" value={display(pinMv,'mV')}/><Metric label="Tegangan AO" value={display(aoMv,'mV')}/>{value.schema==='MQG2'?<Metric label="Kualitas sumber" value={sourceQuality===null?'Tidak tersedia':qualityLabels[sourceQuality]??sourceQuality}/>:null}<Metric label="Umur sumber + waktu sejak permintaan" value={currentAgeUs===null?'Tidak tersedia':`${currentAgeUs} µs`}/></XStack>
    <Text color="$color" {...textStyles.sectionTitle}>{value.schema==='MQG1'?'Kalibrasi Listrik · MQG1':'Kalibrasi Listrik · MQG2'}</Text>
    {value.schema==='MQG1'?<XStack gap="$3" flexWrap="wrap"><Metric label="Resistansi Sensor" value={display(current?value.rs_ohm:null,'Ω')}/><Metric label="Tegangan rangkaian" value={display(value.circuit_mv,'mV')}/><Metric label="Pembagi atas / bawah" value={`${display(value.divider_top_ohm,'Ω')} / ${display(value.divider_bottom_ohm,'Ω')}`}/><Metric label="Topologi AO" value={value.topology===1?'Langsung':value.topology===2?'Buffered':'Belum diverifikasi'}/></XStack>
      :<XStack gap="$3" flexWrap="wrap"><Metric label="Tegangan rangkaian" value={display(value.electrical.circuit_mv,'mV')}/><Metric label="Pembagi atas / bawah" value={`${display(value.electrical.divider_top_ohm,'Ω')} / ${display(value.electrical.divider_bottom_ohm,'Ω')}`}/><Metric label="Topologi AO" value={value.electrical.topology===1?'Langsung':value.electrical.topology===2?'Buffered':'Belum diverifikasi'}/><Metric label="Verifikasi listrik" value={value.electrical.hardware_confirmed?'Dikonfirmasi':'Belum dikonfirmasi'}/></XStack>}
    <Text color="$color" {...textStyles.sectionTitle}>{value.schema==='MQG1'?'Referensi Gas · MQG1':'Respons Dimensionless · MQG2'}</Text>
    {value.schema==='MQG1'?<XStack gap="$3" flexWrap="wrap"><Metric label="Resistansi Referensi R0" value={display(value.r0_ohm,'Ω')}/><Metric label="Rasio Rs/R0" value={display(current?value.rs_over_r0:null)}/><Metric label="Kondisi referensi" value={referenceEnvironment??'Tidak tersedia'}/></XStack>
      :<XStack gap="$3" flexWrap="wrap"><Metric label="q (dimensionless)" value={display(current?value.response.q:null)}/><Metric label="q0 (dimensionless)" value={display(value.response.baseline_valid?value.response.q0:null)}/><Metric label="Rasio q/q0" value={display(current?value.response.ratio:null)}/><Metric label="Jenis referensi" value={value.response.reference_kind==='OPERATIONAL'?'Operasional · rasio saja':value.response.reference_kind==='CLEAN_AIR_CONFIRMED'?'Udara bersih terkonfirmasi':'Tidak ditetapkan'}/><Metric label="Baseline" value={value.response.baseline_valid?'Tersedia dari perangkat':'Tidak tersedia'}/></XStack>}
    <Text color="$color" {...textStyles.sectionTitle}>Kondisi Referensi</Text>
    <XStack gap="$3" flexWrap="wrap"><Metric label="Lingkungan" value={referenceEnvironment??'Tidak tersedia'}/><Metric label="Sumber kondisi" value={conditionsSource===null?'MQG1 legacy':conditionsSource==='DECLARED'?'Deklarasi operator':'Tidak diketahui'}/><Metric label="Suhu / kelembapan" value={`${display(temperature===null?null:temperature/100,'°C')} / ${display(humidity===null?null:humidity/100,'% RH')}`}/><Metric label="Sampel / variasi" value={`${display(sampleCount)} / ${display(variation,'%')}`}/><Metric label="Persiapan operator" value={preparationConfirmed===null?'MQG1 legacy':`${preparationMethod==='OPERATOR_DECLARED'?'Dinyatakan operator':'Belum diketahui'} · ${preparationConfirmed?'dikonfirmasi':'belum dikonfirmasi'}`}/><Metric label="Pemanasan aktual" value={heatingDuration===null?'Tidak tersedia':`${heatingDuration} µs`}/><Metric label="Versi profil" value={profileVersion??'Belum tersimpan'}/><Metric label="Kalibrasi terakhir" value={date===null?'Waktu UTC belum tersedia':`${date}${timeQuality==='ESTIMATED'?' (perkiraan)':''}`}/></XStack>
    <XStack gap="$2" alignItems="center"><Text color="$color" {...textStyles.sectionTitle}>{value.schema==='MQG1'?'Kalibrasi NH3 · MQG1':'NH3 · TEST_ONLY'}</Text><DescriptionHint ariaLabel="Info estimasi riset NH3" description="Estimasi ppm hanya berasal dari hasil firmware MQG2, tidak dihitung ulang di JavaScript. Harus berstatus IN_RANGE dan masih segar. Kurva manufacturer bukan validasi selektivitas atau akurasi fisik."/></XStack>
    {value.schema==='MQG2'&&nh3Estimate!==null?<Metric label="NH3 firmware · TEST_ONLY" value={display(nh3Estimate,'ppm')}/>:<Text color="$colorMuted">NH3 non tersedia{value.schema==='MQG2'?` · ${reasonLabels[value.nh3.status]??value.nh3.status}`:''}</Text>}
    <Text color="$colorMuted" {...textStyles.caption}>Riset sumber: Winsen MQ-135 v1.6 Figure 3, kurva Rs/R0 untuk H2, Air, NH3, dan Toluene. Respons lintas-gas ini bukan bukti selektivitas atau akurasi NH3.</Text>
    {value.schema==='MQG2'&&references?.previousVersion!==null&&references?.previousVersion!==undefined?<Text color="$colorMuted" {...textStyles.caption}>Profil perangkat sebelumnya: versi {references.previousVersion}.</Text>:null}
  </YStack>;
}

export function Mq135GasDiagnosticsUnavailable() {
  return <YStack gap="$3" role="status">
    <Text color="$colorMuted">Bukti kalibrasi dari perangkat belum tersedia. Data monitoring utama tetap dapat dilihat; nilai Rs, R0, atau ppm tidak dibuat di web.</Text>
    <Text color="$color" {...textStyles.sectionTitle}>Data Raw</Text><Text color="$colorMuted">Menunggu bukti perangkat untuk sesi kalibrasi.</Text>
    <Text color="$color" {...textStyles.sectionTitle}>Kalibrasi Listrik</Text><XStack gap="$3" flexWrap="wrap"><Metric label="Tegangan rangkaian" value="Tidak tersedia"/><Metric label="Rangkaian" value="Belum diverifikasi"/></XStack>
    <Text color="$color" {...textStyles.sectionTitle}>Respons Dimensionless</Text><XStack gap="$3" flexWrap="wrap"><Metric label="q" value="Tidak tersedia"/><Metric label="q0" value="Tidak tersedia"/><Metric label="Rasio" value="Tidak tersedia"/></XStack>
    <Text color="$color" {...textStyles.sectionTitle}>NH3 · TEST_ONLY</Text><Text color="$colorMuted">NH3 non tersedia · menunggu bukti firmware yang valid dan segar.</Text>
  </YStack>;
}

function choiceLabel(fieldName:string,choice:string):string {
  if(fieldName==='reference_kind') return choice==='operational'?'Operasional · rasio saja':'Udara bersih · riset NH3';
  if(fieldName==='preparation_method') return choice==='unknown'?'Belum diketahui':'Deklarasi operator';
  if(fieldName==='topology') return choice==='1'?'AO langsung':'AO buffered';
  return choice;
}

export function Mq135ReferenceFields({values,onChange,disabled}:{values:Record<string,string>;onChange:(name:string,value:string)=>void;disabled:boolean}) {
  const [engineering,setEngineering]=useState(true);
  return <YStack gap="$3">
    {(['reference','criteria','electrical'] as const).map(group=><YStack key={group} gap="$2">
      {group==='electrical'?<AppButton variant="ghost" size="sm" ariaLabel="Rincian rangkaian MQ135" ariaExpanded={engineering} onPress={()=>setEngineering(value=>!value)}>{engineering?'Tutup Rangkaian':'Rincian Rangkaian'}</AppButton>:<Text color="$color" fontWeight="700">{group==='reference'?'Kondisi Referensi':'Kriteria Sampel'}</Text>}
      {group==='electrical'&&!engineering?null:MQ135_REFERENCE_FIELDS.filter(field=>field.group===group).map(field=><YStack key={field.name} gap="$1">
        {field.confirmation?<YStack gap="$1"><Text color="$color" {...textStyles.bodySecondary}>{field.label}</Text><XStack gap="$2" flexWrap="wrap"><AppButton variant={values[field.name]==='1'?'primary':'secondary'} size="sm" disabled={disabled} ariaLabel={`${field.label}: Ya${values[field.name]==='1'?' (dipilih)':''}`} onPress={()=>onChange(field.name,'1')}>Ya</AppButton><AppButton variant={values[field.name]==='0'?'primary':'secondary'} size="sm" disabled={disabled} ariaLabel={`${field.label}: Tidak${values[field.name]==='0'?' (dipilih)':''}`} onPress={()=>onChange(field.name,'0')}>Belum dikonfirmasi</AppButton></XStack></YStack>
          :field.choices?<YStack gap="$1"><Text color="$color" {...textStyles.bodySecondary}>{field.label}</Text><XStack gap="$2" flexWrap="wrap">{field.choices.map(option=><AppButton key={option} variant={values[field.name]===option?'primary':'secondary'} size="sm" disabled={disabled} ariaLabel={`${choiceLabel(field.name,option)}${values[field.name]===option?' (dipilih)':''}`} onPress={()=>onChange(field.name,option)}>{choiceLabel(field.name,option)}</AppButton>)}</XStack></YStack>
          :<><Text color="$color" {...textStyles.bodySecondary}>{field.label} ({field.unit}){field.optional?' · opsional':''}</Text><Input minHeight={44} fontSize={16} disabled={disabled} value={values[field.name]??''} onChangeText={value=>onChange(field.name,value)} aria-label={field.label} keyboardType={field.text?'default':'decimal-pad'} maxLength={field.text?31:24}/></>}
      </YStack>)}
    </YStack>)}
    <Text color="$colorMuted" {...textStyles.caption}>Kondisi operational memberi baseline rasio saja. Udara bersih untuk NH3 riset memerlukan deklarasi operator dan pemanasan aktual lebih dari 48 jam. Suhu dan RH boleh dicatat terpisah; nilai listrik tidak diisi otomatis.</Text>
  </YStack>;
}
