import type { SensorHistoryPoint } from '../api/types';
export type SensorMetric={key:string;label:string;unit:string;source:'converted'|'web_estimate';scale?:number;fallbackKeys?:readonly string[]};
export type PlotPoint={x:number;y:number;bootId:string;origin:string;t:number;value:number};
export type PlotSegment=PlotPoint[];
export type PlotResult={segments:PlotSegment[];omittedTime:number;omittedQuality:number;window:{from:number;to:number}|null;domain:{min:number;max:number}|null};
const metric=(key:string,label:string,unit:string,source:SensorMetric['source'],extra?:Pick<SensorMetric,'scale'|'fallbackKeys'>):SensorMetric=>({key,label,unit,source,...extra});
// Single source of truth: the ADC profile declares 1:1, so AO equals pin mV.
export { MQ135_DIVIDER } from './mq135Nh3Estimate';
import { MQ135_DIVIDER } from './mq135Nh3Estimate';
export const SENSOR_METRICS: Record<string, SensorMetric[]> = {
 hx711_organic:[metric('kg','Berat','kg','converted')], hx711_inorganic:[metric('kg','Berat','kg','converted')], hx711_b3:[metric('kg','Berat','kg','converted')],
 mq4:[metric('adc_mv','Tegangan ADC','mV','converted')], mq7:[metric('adc_mv','Tegangan ADC','mV','converted')], mq8:[metric('adc_mv','Tegangan ADC','mV','converted')], mq135:[metric('adc_pin_mv','Tegangan AO','mV','converted',{scale:MQ135_DIVIDER,fallbackKeys:['adc_mv','sensor_ao_mv']})],
 ultrasonic_organic:[metric('distance_mm','Jarak','mm','converted'),metric('fill_percent','Keterisian','%','converted')], ultrasonic_inorganic:[metric('distance_mm','Jarak','mm','converted'),metric('fill_percent','Keterisian','%','converted')], ultrasonic_b3:[metric('distance_mm','Jarak','mm','converted'),metric('fill_percent','Keterisian','%','converted')],
 ds18b20_b3:[metric('temperature_c','Suhu','°C','converted')],
 sht31_44:[metric('temperature_c','Suhu','°C','converted'),metric('humidity_percent','Kelembapan','%RH','converted')], sht31_45:[metric('temperature_c','Suhu','°C','converted'),metric('humidity_percent','Kelembapan','%RH','converted')]
};
const decimal=/^(0|[1-9][0-9]*)$/;
export function sensorMetricValue(converted:Record<string,number>|null|undefined,m:SensorMetric):number|null {
 // Old uncalibrated estimates cannot become monitoring or history values.
 if(m.source!=='converted')return null;
 const raw=converted?.[m.key] ?? m.fallbackKeys?.map(key=>converted?.[key]).find(value=>typeof value==='number') ?? null;
 if(typeof raw!=='number'||!Number.isFinite(raw))return null;
 return typeof m.scale==='number'?raw*m.scale:raw;
}
const bigint=(s:string)=>decimal.test(s)?BigInt(s):null;
export function buildSensorPlot(points:SensorHistoryPoint[],m:SensorMetric):PlotResult{
 const ordered=[...points].sort((a,b)=>{if(a.boot_id!==b.boot_id)return a.boot_id<b.boot_id?-1:1;const x=bigint(a.monotonic_us),y=bigint(b.monotonic_us);if(x!==null&&y!==null&&x!==y)return x<y?-1:1;const sx=bigint(a.sequence),sy=bigint(b.sequence);return sx!==null&&sy!==null?(sx<sy?-1:sx>sy?1:0):a.sequence.localeCompare(b.sequence);});
 const values:Array<{p:SensorHistoryPoint;t:number;y:number;breakBefore:boolean}>=[];let omittedTime=0,omittedQuality=0,pendingBreak=false;
 for(const p of ordered){const t=p.recorded_at&&['SYNCED','RECONSTRUCTED'].includes(p.time_quality)?Date.parse(p.recorded_at):NaN;if(!Number.isFinite(t)){omittedTime++;pendingBreak=true;continue;}const quality=p.quality==='READY',source=sensorMetricValue(p.converted,m),y=m.unit==='kg'&&typeof source==='number'?Math.abs(source):source;if(!quality||typeof y!=='number'||!Number.isFinite(y)){omittedQuality++;pendingBreak=true;continue;}values.push({p,t,y,breakBefore:pendingBreak});pendingBreak=false;}
 const ts=values.map(v=>v.t),ys=values.map(v=>v.y),window=ts.length?{from:Math.min(...ts),to:Math.max(...ts)}:null,domain=ys.length?{min:Math.min(...ys),max:Math.max(...ys)}:null;const segments:PlotSegment[]=[];let seg:PlotSegment=[],prior:typeof values[number]|undefined;
 for(const v of values){if(prior&&(v.breakBefore||v.p.boot_id!==prior.p.boot_id||v.p.sample_origin!==prior.p.sample_origin||v.t<=prior.t)){if(seg.length)segments.push(seg);seg=[];}const x=window&&window.to!==window.from?16+(v.t-window.from)/(window.to-window.from)*288:160;const range=domain?domain.max-domain.min:0;const y=range===0?60:104-(v.y-domain!.min)/range*88;seg.push({x,y,bootId:v.p.boot_id,origin:v.p.sample_origin,t:v.t,value:v.y});prior=v;}if(seg.length)segments.push(seg);
 return{segments,omittedTime,omittedQuality,window,domain};}
