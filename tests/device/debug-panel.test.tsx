import React from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { DeviceBleSession } from '../../lib/ble/device-session';
import { DebugViewController } from '../../lib/debug/poller';
const hooks = vi.hoisted(() => ({ current: null as any }));
vi.mock('react', async original => { const r=await original<any>();return {...r,useState(initial:any){const f=hooks.current,i=f.si++;return[f.state[i]??(f.state[i]=initial),(v:any)=>{f.state[i]=typeof v==='function'?v(f.state[i]):v;f.dirty=true}]},useEffect(setup:()=>any,deps:any[]){const f=hooks.current,i=f.ei++,old=f.effects[i];if(!old||deps.some((x:any,j:number)=>!Object.is(x,old.deps[j])))f.effects[i]={deps,setup,cleanup:old?.cleanup,changed:true}},useMemo(fn:()=>any,deps:any[]){const f=hooks.current,i=f.mi++,old=f.memos[i];if(!old||deps.some((x:any,j:number)=>!Object.is(x,old.deps[j])))f.memos[i]={deps,value:fn()};return f.memos[i].value},useSyncExternalStore(_:any,get:()=>any){return get()}}});
vi.mock('tamagui',()=>({Card:'card',Input:'input',Text:'text',XStack:'div',YStack:'section'}));vi.mock('../../components/AppButton',()=>({AppButton:(p:any)=>React.createElement('button',{onClick:p.onPress,disabled:p.disabled},p.children)}));
import { DeviceDebugPanel } from '../../components/device/DeviceDebugPanel';
function mount(controller:DebugViewController,deviceId='dev'){const f={si:0,ei:0,mi:0,state:[] as any[],effects:[] as any[],memos:[] as any[],tree:null as any,dirty:false,mounted:true};const render=()=>{let n=10;do{if(!--n)throw Error('effects unsettled');f.si=f.ei=f.mi=0;f.dirty=false;hooks.current=f;try{f.tree=DeviceDebugPanel({controller,deviceId})}finally{hooks.current=null}const changed=f.effects.filter(x=>x.changed);changed.forEach(x=>{x.cleanup?.();x.cleanup=undefined});changed.forEach(x=>{x.changed=false;x.cleanup=x.setup()||undefined})}while(f.dirty)};const unmount=()=>{f.mounted=false;f.effects.forEach(x=>x.cleanup?.())};render();return{f,render,unmount}}
function setup(){const client:any={debugMeta:vi.fn(),debugPage:vi.fn(),setDebugLevel:vi.fn(),close:vi.fn(),connect:vi.fn(),verifyDeviceIdentity:vi.fn()};const owner=new DeviceBleSession('dev',{createClient:()=>client});const c=new DebugViewController({deviceId:'dev',session:owner,history:async()=>({device_id:'dev',events:[],next_after:'0',oldest_available:'0',retention_gap:false,has_more:false})});return{c,owner,client}}
afterEach(()=>vi.restoreAllMocks());
describe('DeviceDebugPanel effect lifecycle',()=>{it('starts only when visible and clears authorization on unmount',()=>{const{c,owner}=setup(),v=mount(c);expect(c.getSnapshot().visible).toBe(false);v.f.state[3]=true;v.render();expect(c.getSnapshot().visible).toBe(true);c.selectSource('https');c.authorizeOperator('memory-only');expect(c.getSnapshot().operatorAuthorized).toBe(true);v.unmount();expect(c.getSnapshot().visible).toBe(false);expect(c.getSnapshot().operatorAuthorized).toBe(false);owner.dispose()})});

it('shows HTTPS capture metadata as history and does not offer local level mutation', () => {
  const { c, owner } = setup(); c.selectSource('https');
  const view = mount(c);
  view.f.state[3] = true; view.f.state[4] = true; view.render();
  const tree = JSON.stringify(view.f.tree);
  expect(tree).not.toContain('Set local ');
  expect(tree).toContain('Server history');
  view.unmount(); owner.dispose();
});

it('wraps the HTTPS token row so clear authorization remains reachable on mobile', () => {
  const { c, owner } = setup(); c.selectSource('https');
  const view = mount(c);
  view.f.state[3] = true; view.render();
  const nodes: any[] = [];
  const visit = (node: any) => {
    if (Array.isArray(node)) { node.forEach(visit); return; }
    if (!node?.props) return;
    nodes.push(node); visit(node.props.children);
  };
  visit(view.f.tree);
  const row = nodes.find(node => Array.isArray(node.props.children) &&
    node.props.children.some((child: any) => child?.type === 'input'));
  expect(row?.props.flexWrap).toBe('wrap');
  expect(nodes.find(node => node.type === 'input')?.props.type).toBe('password');
  view.unmount(); owner.dispose();
});

it('aborts a pending request on actual panel hide/unmount and ignores late completion', async () => {
  const { c: unused, owner } = setup(); unused.dispose();
  let finish!: (value: any) => void;
  let signal!: AbortSignal;
  const c = new DebugViewController({ deviceId: 'dev', session: owner,
    history: (_device, _after, _limit, inputSignal) => {
      signal = inputSignal!;
      return new Promise(resolve => { finish = resolve; });
    },
  });
  c.selectSource('https'); c.authorizeOperator('TEST-only');
  const view = mount(c); view.f.state[3] = true; view.render();
  expect(signal.aborted).toBe(false);
  view.f.state[3] = false; view.render();
  expect(signal.aborted).toBe(true);
  view.unmount();
  finish({ device_id: 'dev', events: [], next_after: '0', oldest_available: '0', retention_gap: false, has_more: false });
  await Promise.resolve(); await Promise.resolve();
  expect(c.getSnapshot()).toMatchObject({ visible: false, operatorAuthorized: false, events: [] });
  owner.dispose(); vi.unstubAllGlobals();
});
