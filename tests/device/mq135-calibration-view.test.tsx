import React from 'react';
const { renderToStaticMarkup } = require('react-dom/server') as {renderToStaticMarkup:(node:React.ReactNode)=>string};
import { describe, expect, it, vi } from 'vitest';
import mqg2Fixture from '../../contracts/calibration/mq135-nh3/v2/fixtures/in-range.json';
import mqg2StaleFixture from '../../contracts/calibration/mq135-nh3/v2/fixtures/stale-source.json';
vi.mock('react-native',()=>({Image:({accessibilityLabel}:{accessibilityLabel?:string})=><img alt={accessibilityLabel}/>}));
import { TamaguiProvider } from 'tamagui';
import config from '../../tamagui.config';
import fixture from '../../contracts/calibration/mq135-relative/v1/fixtures/ready-test-only.json';
import { decodeMq135GasResult, mq135Nh3EstimateCurrent, mq135RatioCurrent } from '../../lib/calibration/mq135';
import { Mq135GasDiagnostics, Mq135ReferenceFields } from '../../components/device/Mq135Calibration';
const render = (node:React.ReactNode)=>renderToStaticMarkup(<TamaguiProvider config={config} defaultTheme="light">{node}</TamaguiProvider>);
describe('MQ135 calibration measurement layers',()=>{
 it('shows all four layers without invented ppm or opaque identity walls',()=>{
  const data=decodeMq135GasResult({...fixture,max_age_us:'1000000000'})!;
  const html=render(<Mq135GasDiagnostics value={data} startedAtMs={performance.now()}/>);
  for(const label of ['Data Raw','Kalibrasi Listrik','Referensi Gas','Kalibrasi NH3','NH3 non tersedia','TEST_ONLY','Rasio Rs/R0']) expect(html).toContain(label);
  expect(html).not.toContain('sha256:'); expect(html).not.toContain(fixture.source_boot_id);
 });
 it('shows the MQG2 four measurement layers and only the current device NH3 research result',()=>{
  const data=decodeMq135GasResult(mqg2Fixture)!;
  const html=render(<Mq135GasDiagnostics value={data} startedAtMs={performance.now()}/>);
  for(const label of ['Data Raw','Kalibrasi Listrik · MQG2','Respons Dimensionless · MQG2','q (dimensionless)','q0 (dimensionless)','Rasio q/q0','Kualitas sumber','Pemanasan aktual','NH3 · TEST_ONLY','76,393 ppm','Riset sumber: Winsen MQ-135 v1.6 Figure 3']) expect(html).toContain(label);
  expect(html).not.toContain('sha256:'); expect(html).not.toContain('11111111111111111111111111111111');
 });
 it('withholds firmware ppm when MQG2 source evidence is stale',()=>{
  const data=decodeMq135GasResult(mqg2StaleFixture)!;
  const html=render(<Mq135GasDiagnostics value={data} startedAtMs={performance.now()}/>);
  expect(html).toContain('Data kedaluwarsa');
  expect(html).toContain('NH3 non tersedia');
  expect(html).not.toContain('NH3 firmware');
  expect(html).not.toContain('76,393 ppm');
 });
 it('renders explicit normalized-reference fields without physical defaults or legacy caller constants',()=>{
  const html=render(<Mq135ReferenceFields values={{}} onChange={()=>{}} disabled={false}/>);
  for(const label of ['Operasional · rasio saja','Udara bersih · riset NH3','Metode persiapan','Pemanasan aktual','Kondisi referensi telah dikonfirmasi','Suhu referensi (°C) · opsional','Kelembapan referensi (% RH) · opsional','Tegangan rangkaian (mV)','Resistor pembagi atas (Ω)','AO langsung','AO buffered','Jumlah sampel referensi']) expect(html).toContain(label);
  expect(html).not.toContain('module_load_ohm'); expect(html).not.toContain('q0'); expect(html).not.toContain('value="10000"');
 });
 it('preserves zero while withholding an expired operational ratio',()=>{
  const data=decodeMq135GasResult({...fixture,raw_counts:0,rs_ohm:null,rs_over_r0:null,quality:'OUT_OF_RANGE',reason:'ADC_SATURATED'})!;
  const html=render(<Mq135GasDiagnostics value={data} startedAtMs={performance.now()-1000}/>);
  expect(html).toContain('0 counts'); expect(html).toContain('Tidak tersedia');
 });
 it('renders a source-identity diagnostic with current quantities unavailable',()=>{
  // Constructed from the immutable in-range fixture; this is not producer-captured output.
  const diagnostic={...mqg2Fixture,quality:'INVALID',reason:'SOURCE_IDENTITY_INVALID',adc:{...mqg2Fixture.adc,ao_mv:null},response:{...mqg2Fixture.response,q:null,ratio:null},nh3:{...mqg2Fixture.nh3,ppm:null,status:'SOURCE_NOT_READY'}};
  const data=decodeMq135GasResult(diagnostic);
  expect(data).not.toBeNull();
  if(!data||data.schema!=='MQG2') throw new Error('constructed MQG2 diagnostic rejected');
  expect(data.source.quality).toBe('READY'); expect(data.source.fresh).toBe(true);
  expect(mq135RatioCurrent(data,0)).toBe(false); expect(mq135Nh3EstimateCurrent(data,0)).toBe(false);
  const html=render(<Mq135GasDiagnostics value={data} startedAtMs={performance.now()}/>);
  expect(html).toContain('Identitas sumber tidak valid.');
  expect(html).toContain('NH3 non tersedia'); expect(html).not.toContain('NH3 firmware');
  expect(html).not.toContain('76,393 ppm');
  for(const [label,next] of [['Tegangan AO','Kualitas sumber'],['q (dimensionless)','q0 (dimensionless)'],['Rasio q/q0','Jenis referensi']]){
   const start=html.indexOf(label),end=html.indexOf(next,start+label.length);
   expect(start).toBeGreaterThanOrEqual(0); expect(end).toBeGreaterThan(start);
   expect(html.slice(start,end)).toContain('Tidak tersedia');
  }
 });
});
