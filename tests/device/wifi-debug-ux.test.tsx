import React from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { WifiSettingsDialog } from '../../components/device/WifiSettingsDialog';
import { DeviceBleSession } from '../../lib/ble/device-session';
import { WifiSettingsController } from '../../lib/ble/wifi-settings';
import { DeviceDebugPanel } from '../../components/device/DeviceDebugPanel';
const ReactMock = vi.hoisted(() => ({ state: [] as any[], index: 0 }));
vi.mock('react', async original => { const r=await original<any>(); return {...r,useSyncExternalStore(_s:any,get:any){return get()},useState(initial:any){const index=ReactMock.index++;return[index in ReactMock.state?ReactMock.state[index]:initial,(value:any)=>{ReactMock.state[index]=value}]},useEffect(){},useMemo(fn:any){return fn()}} });
vi.mock('tamagui',()=>({Card:'card',Input:'input',ScrollView:'div',Text:'text',XStack:'div',YStack:'section'}));
vi.mock('../../components/AppButton',()=>({AppButton:(p:any)=>React.createElement('button',{...p,onClick:p.onPress},p.children)}));
vi.mock('../../components/SectionHeader',()=>({DescriptionHint:(p:any)=>React.createElement('span',{'data-hint':p.description})}));
import { DescriptionHint } from '../../components/SectionHeader';
const walk=(node:any,all:any[]=[]):any[]=>{if(Array.isArray(node))node.forEach(n=>walk(n,all));else if(node?.props){all.push(node);walk(node.props.children,all)}return all};
const text=(node:any):string=>typeof node==='string'||typeof node==='number'?String(node):Array.isArray(node)?node.map(text).join(' '):node?.props?text(node.props.children):'';
const render=(Component:any,props:any)=>{ReactMock.index=0;return Component(props)};
const fixture=()=>({
 controller:{subscribe:()=>()=>{},getSnapshot:()=>({open:true,busy:false,status:null,error:null,selectedSsid:'TEST-NETWORK-LONG-LABEL',password:'',saved:[],nearby:[{ssid:'TEST-NETWORK-LONG-LABEL',rssi:-52,secure:true}],rawCount:1}),open:vi.fn(),close:vi.fn(),selectSsid:vi.fn(),setPassword:vi.fn(),submit:vi.fn(),scan:vi.fn()},
 session:{subscribe:()=>()=>{},getSnapshot:()=>({connected:true,deviceId:'peer',deviceName:'TEST-BOARD',busy:null as string|null,controlActive:false}),close:vi.fn()}
});
const debugFixture=()=>({subscribe:()=>()=>{},getSnapshot:()=>({events:[],source:'ble',busy:false,after:'0'}),setDevice:vi.fn(),setVisible:vi.fn(),dispose:vi.fn()});
afterEach(()=>{ReactMock.state=[];ReactMock.index=0});
describe('compact device Wi-Fi and debug views',()=>{
 it('renders disconnect recovery after an actual readonly request survives close/reopen',async()=>{
  let resolve!: (value:any)=>void;let callbacks:any;
  const session=new DeviceBleSession('0000000000000001',{createClient:(c:any)=>{callbacks=c;return{connect:async()=>c.onConnectionChange(true),verifyDeviceIdentity:async(id:string)=>{c.onVerifiedDeviceIdentityChange(id);return id},close:()=>c.onConnectionChange(false),scanDetailed:()=>new Promise(r=>resolve=r)} as any}});
  const controller=new WifiSettingsController(session);await session.connect();controller.open();const scan=controller.scan();controller.close();controller.open();
  const tree=render(WifiSettingsDialog,{deviceId:'fixture',bleDeviceId:'peer',session,controller});
  expect(controller.getSnapshot().readonlyBusy).toBe(true);
  expect(walk(tree).find(n=>n.props.ariaLabel==='Putuskan BLE bersama').props.disabled).toBe(false);
  session.close();resolve({networks:[],rawCount:0});await scan;controller.dispose();session.dispose();expect(callbacks).toBeTruthy();
 });
 it('renders only the three canonical status cards, not redundant device and summary labels',()=>{
  const {controller,session}=fixture();const peer=session as any;peer.getSnapshot=()=>({connected:true,deviceId:'peer',deviceName:'SIGAP-A398',busy:null,controlActive:false});
  const connectedController={...controller,getSnapshot:()=>({...fixture().controller.getSnapshot(),status:{state:'CONNECTED',ssid:'TEST-NETWORK-LONG-LABEL'}})};
  const tree=render(WifiSettingsDialog,{deviceId:'fixture-device',bleDeviceId:'peer',session,controller:connectedController});
  const renderedText=text(tree);
  expect(renderedText).toContain('BLE');expect(renderedText).toContain('Identitas perangkat');expect(renderedText).toContain('Wi-Fi');
  expect(renderedText).toContain('Terhubung');expect(renderedText).toContain('Identitas cocok');expect(renderedText).toContain('Terhubung ke TEST-NETWORK-LONG-LABEL');
  expect(renderedText).not.toContain('SIGAP-A398');
  expect(renderedText.replace(/\s+/g,' ')).not.toContain('Wi-Fi terhubung · BLE aktif · identitas cocok');
 });
 it('keeps Wi-Fi choices bounded, literal and selected with a labelled masked input',()=>{
  const {controller,session}=fixture();const tree=render(WifiSettingsDialog,{deviceId:'fixture-device',bleDeviceId:'peer',session,controller});const nodes=walk(tree);
  expect(nodes.some(n=>n.props.overflowY==='auto'&&n.props.maxHeight===260)).toBe(true);
  const input=nodes.find(n=>n.type==='input');expect(input.props['aria-label']).toBe('Password Wi-Fi');expect(input.props.secureTextEntry).toBe(true);
  expect(input.props.type).toBe('password');
  expect(text(tree)).toContain('TEST-NETWORK-LONG-LABEL');expect(text(tree)).toContain('Dipilih');
  const networkLabel=nodes.find(n=>n.type==='text'&&n.props.flexShrink===1);
  expect(networkLabel.props.width).toBe('100%');
  expect(networkLabel.props.minWidth).toBe(0);
  expect(text(tree).replace(/\s+/g,' ')).toContain('Wi-Fi Belum terhubung');
  expect(nodes.some(n=>n.props['aria-label']==='Tahapan proses')).toBe(false);
  expect(nodes.some(n=>n.props.ariaLabel==='Hubungkan BLE')).toBe(false);
 });
 it('keeps debug controls, credential fields and logs out of the closed view',()=>{
  const tree=render(DeviceDebugPanel,{controller:debugFixture(),deviceId:'fixture'});
  expect(text(tree)).toContain('Tampilkan debug');expect(walk(tree).some(n=>n.props.role==='log')).toBe(false);
  expect(walk(tree).some(n=>n.type==='input')).toBe(false);expect(text(tree)).not.toContain('Set local');
 });
 it('renders actual opened trace details in a bounded viewport, retaining the newest sixty',()=>{
  ReactMock.state[3]=true;ReactMock.state[4]=true;ReactMock.state[5]='trace';
  const tree=render(DeviceDebugPanel,{controller:debugFixture(),deviceId:'fixture',trace:Array.from({length:65},(_,elapsedMs)=>({type:'request_complete',operation:'GET_DEBUG',elapsedMs,outcome:'accepted'}))});
  const log=walk(tree).find(n=>n.props.role==='log');expect(log.props.maxHeight).toBe(260);expect(log.props.children).toHaveLength(60);
  expect(text(log)).toContain('· 64 ms');expect(text(log)).not.toContain('· 4 ms');expect(text(tree)).toContain('Permintaan browser');
 });
 it.each(['debug','wifi','connect'])('blocks incompatible Wi-Fi actions while %s owns BLE',busy=>{
  const {controller,session}=fixture();const snapshot={...session.getSnapshot(),busy};session.getSnapshot=()=>snapshot;
  const tree=render(WifiSettingsDialog,{deviceId:'fixture',bleDeviceId:'peer',session,controller});
  const scan=walk(tree).find(n=>n.props.ariaLabel==='Pindai Wi-Fi');expect(scan.props.disabled).toBe(true);scan.props.onPress();expect(controller.scan).not.toHaveBeenCalled();
  const submit=walk(tree).find(n=>n.props.ariaLabel==='Simpan dan hubungkan Wi-Fi');expect(submit.props.disabled).toBe(true);submit.props.onPress();expect(controller.submit).not.toHaveBeenCalled();
 });
 it('keeps explicit BLE disconnect available to recover a stalled readonly owner only',()=>{
  const {controller,session}=fixture();
  const peer=session as any;
  peer.getSnapshot=()=>({connected:true,deviceId:'peer',deviceName:'TEST-BOARD',busy:'debug',controlActive:false});
  const tree=render(WifiSettingsDialog,{deviceId:'fixture',bleDeviceId:'peer',session,controller});
  const disconnect=walk(tree).find(n=>n.props.ariaLabel==='Putuskan BLE bersama');
  expect(disconnect.props.disabled).toBeFalsy();disconnect.props.onPress();expect(session.close).toHaveBeenCalledTimes(1);
  peer.getSnapshot=()=>({connected:true,deviceId:'peer',deviceName:'TEST-BOARD',busy:'wifi',controlActive:false});
  controller.getSnapshot=()=>({...fixture().controller.getSnapshot(),busy:true,readonlyBusy:true});
  const wifiRead=render(WifiSettingsDialog,{deviceId:'fixture',bleDeviceId:'peer',session,controller});
  const wifiDisconnect=walk(wifiRead).find(n=>n.props.ariaLabel==='Putuskan BLE bersama');
  expect(wifiDisconnect.props.disabled).toBeFalsy();wifiDisconnect.props.onPress();expect(session.close).toHaveBeenCalledTimes(2);
  controller.getSnapshot=()=>({...fixture().controller.getSnapshot(),busy:true,readonlyBusy:false});
  const mutation=render(WifiSettingsDialog,{deviceId:'fixture',bleDeviceId:'peer',session,controller});
  expect(walk(mutation).find(n=>n.props.ariaLabel==='Putuskan BLE bersama').props.disabled).toBe(true);
  peer.getSnapshot=()=>({connected:true,deviceId:'peer',deviceName:'TEST-BOARD',busy:'calibration',controlActive:true});
  const protectedTree=render(WifiSettingsDialog,{deviceId:'fixture',bleDeviceId:'peer',session,controller});
  expect(walk(protectedTree).find(n=>n.props.ariaLabel==='Putuskan BLE bersama').props.disabled).toBe(true);
 });
 it('does not hide an operation error or a sequence gap behind advanced settings',()=>{
  ReactMock.state[3]=true;const controller={...debugFixture(),getSnapshot:()=>({...debugFixture().getSnapshot(),error:'TEST_ERROR',firmwareGap:true})};
  const tree=render(DeviceDebugPanel,{controller,deviceId:'fixture'});
  expect(walk(tree).find(n=>n.props.role==='alert')?.props.children).toBe('TEST_ERROR');expect(text(tree)).toContain('Sebagian log lama');expect(text(tree)).not.toContain('Set local');
 });
});
