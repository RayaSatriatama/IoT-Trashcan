import { useEffect, useMemo, useState, useSyncExternalStore } from 'react';
import { Card, Input, Text, XStack, YStack } from 'tamagui';
import { AppButton } from '../AppButton';
import type { DebugViewController } from '../../lib/debug/poller';
import type { DebugLevel } from '../../lib/debug/types';
import { DescriptionHint } from '../SectionHeader';
import { textStyles } from '../../lib/ui/typography';
import { DebugEventList } from './DebugEventList';
export function DeviceDebugPanel({ controller, deviceId, busy = false, trace = [], hideToggle = false, visible: controlledVisible, onVisibleChange }: { controller: DebugViewController; deviceId: string; busy?: boolean; hideToggle?: boolean; visible?: boolean; onVisibleChange?: (visible: boolean) => void; trace?: readonly { type: string; operation?: string; elapsedMs?: number; outcome?: string; category?: string; reason?: string }[] }) {
 const snapshot = useSyncExternalStore(controller.subscribe, controller.getSnapshot, controller.getSnapshot);
 const [filter,setFilter]=useState<DebugLevel|'ALL'>('ALL'),[moduleFilter,setModuleFilter]=useState<number|null>(null),[token,setToken]=useState(''),[localVisible,setLocalVisible]=useState(false),[advanced,setAdvanced]=useState(false),[view,setView]=useState<'firmware'|'trace'>('firmware');
 const visible = controlledVisible === undefined ? localVisible : controlledVisible;
 const toggleVisible = () => { if (onVisibleChange) onVisibleChange(!visible); else setLocalVisible(!visible); };
 const [pageVisible,setPageVisible]=useState(typeof document === 'undefined' || !document.hidden);
 useEffect(()=>{controller.setDevice(deviceId);setToken('')},[controller,deviceId]);
 useEffect(()=>{if(typeof document==='undefined')return;const changed=()=>setPageVisible(!document.hidden);document.addEventListener('visibilitychange',changed);return()=>document.removeEventListener('visibilitychange',changed)},[]);
 useEffect(()=>{controller.setVisible(visible&&!busy&&pageVisible&&view==='firmware');return()=>controller.setVisible(false)},[controller,visible,busy,pageVisible,view]);
 useEffect(()=>()=>controller.dispose(),[controller]);

 const events=useMemo(()=>snapshot.events.filter(item=>(filter==='ALL'||item.event?.level===filter)&&(moduleFilter===null||item.event?.module===moduleFilter)),[snapshot.events,filter,moduleFilter]);
 const modules=Array.from(new Set(snapshot.events.map(item=>item.event?.module).filter((value):value is number=>value!==undefined&&value!==null)));
 const traceRows = advanced ? trace.slice(-60) : trace.filter(item => item.type === 'request_complete' || item.type.includes('timeout') || item.type.includes('rejected')).slice(-5);
 if (hideToggle && !visible) return null;
 return <Card borderWidth={1} borderColor="$borderColor" backgroundColor="$surface" borderRadius="$4" padding="$4"><YStack gap="$3">
  <XStack alignItems="center" justifyContent="space-between" flexWrap="wrap" gap="$2"><XStack alignItems="center" flex={1} minWidth={180}><Text color="$color" {...textStyles.cardTitle}>Debug perangkat</Text><DescriptionHint description="Log firmware berasal dari perangkat. Client trace mencatat permintaan browser, bukan bukti sensor. Pembacaan debug dijeda selama kalibrasi." ariaLabel="Info sumber debug" /></XStack><AppButton size="sm" variant="secondary" onPress={toggleVisible} ariaExpanded={visible}>{visible?'Sembunyikan debug':'Tampilkan debug'}</AppButton></XStack>
  {visible ? <>
  <XStack gap="$2"><AppButton size="sm" variant={view==='firmware'?'secondary':'ghost'} onPress={()=>setView('firmware')}>Firmware</AppButton><AppButton size="sm" variant={view==='trace'?'secondary':'ghost'} onPress={()=>setView('trace')}>Client trace</AppButton></XStack>
  {view==='trace'?<YStack gap="$2"><AppButton size="sm" variant="ghost" onPress={()=>setAdvanced(value=>!value)} ariaExpanded={advanced}>{advanced?'Ringkas trace':'Detail frame'}</AppButton><Text color="$colorMuted" {...textStyles.caption}>Permintaan browser · {advanced?'detail frame':'hasil terbaru'}</Text><YStack maxHeight={260} overflowY="auto" gap="$2" role="log" aria-label="Client trace browser" tabIndex={0}>{traceRows.length===0?<Text role="status" color="$colorMuted">Belum ada hasil permintaan.</Text>:traceRows.map((item,index)=><Text key={`${index}:${item.type}`} color="$colorMuted" {...textStyles.bodySecondary}>{item.operation??'Operasi'} · {item.outcome??item.type}{item.elapsedMs===undefined?'':` · ${item.elapsedMs} ms`}{advanced?` · ${item.type}${item.category?` · ${item.category}`:''}${item.reason?` · ${item.reason}`:''}`:''}</Text>)}</YStack></YStack>:<>
  <XStack gap="$2" flexWrap="wrap"><AppButton size="sm" variant={snapshot.source==='ble'?'secondary':'ghost'} onPress={()=>controller.selectSource('ble')}>BLE</AppButton><AppButton size="sm" variant={snapshot.source==='https'?'secondary':'ghost'} onPress={()=>controller.selectSource('https')}>HTTPS</AppButton><AppButton size="sm" variant="ghost" disabled={snapshot.busy||busy} onPress={()=>void controller.refresh()}>Perbarui</AppButton><AppButton size="sm" variant="ghost" onPress={()=>controller.clearView()}>Bersihkan tampilan</AppButton></XStack>
  {snapshot.source==='https'?<YStack gap="$2"><XStack alignItems="center"><Text color="$color" {...textStyles.caption}>Token operator</Text><DescriptionHint description="Otorisasi hanya disimpan dalam memori sesi, terpisah dari BLE. Nilai input dikosongkan setelah dikirim; token tidak dicatat pada trace." ariaLabel="Info otorisasi riwayat server" /></XStack><XStack gap="$2" flexWrap="wrap"><Input aria-label="Operator bearer token (memory only)" type="password" secureTextEntry value={token} onChangeText={setToken} placeholder="Token operator" minHeight={44} fontSize={16} flex={1} minWidth={180}/><AppButton size="sm" disabled={!token.trim()} onPress={()=>{controller.authorizeOperator(token);setToken('')}}>Otorisasi HTTPS</AppButton><AppButton size="sm" variant="ghost" onPress={()=>{controller.clearOperatorAuthorization();setToken('')}}>Hapus otorisasi</AppButton></XStack></YStack>:null}
  <AppButton size="sm" variant="ghost" onPress={()=>setAdvanced(v=>!v)} ariaExpanded={advanced}>{advanced?'Sembunyikan lanjutan':'Pengaturan lanjutan'}</AppButton>
  {busy?<Text role="status" color="$colorMuted">Debug dijeda selama operasi perangkat.</Text>:null}
  {snapshot.error?<Text role="alert" color="$statusSafetyAlert">{snapshot.error}</Text>:null}
  {snapshot.source==='https'&&!snapshot.operatorAuthorized?<Text role="status" color="$colorMuted">Otorisasi operator diperlukan.</Text>:null}
  {snapshot.source==='ble'&&!snapshot.meta&&!snapshot.error?<Text role="status" color="$colorMuted">Hubungkan perangkat untuk membaca log.</Text>:null}
  {snapshot.source==='ble'&&snapshot.firmwareGap?<Text role="status" color="$statusWaspada">Sebagian log lama perangkat tidak tersedia.</Text>:null}
  {snapshot.source==='ble'&&snapshot.bootChanged?<Text role="status" color="$statusWaspada">Boot perangkat berubah. Metadata diperbarui.</Text>:null}
  {snapshot.source==='https'&&snapshot.history?.retention_gap?<Text role="status" color="$statusWaspada">Sebagian riwayat server telah dihapus oleh retensi.</Text>:null}
  {advanced?<><XStack gap="$2" flexWrap="wrap">{(['ALL','ERROR','WARN','INFO','DEBUG'] as const).map(level=><AppButton key={level} size="sm" variant={filter===level?'secondary':'ghost'} onPress={()=>setFilter(level)}>{level}</AppButton>)}</XStack>
  {modules.length?<XStack gap="$2" flexWrap="wrap"><AppButton size="sm" variant={moduleFilter===null?'secondary':'ghost'} onPress={()=>setModuleFilter(null)}>All modules</AppButton>{modules.map(module=><AppButton key={module} size="sm" variant={moduleFilter===module?'secondary':'ghost'} onPress={()=>setModuleFilter(module)}>{`Module ${module}`}</AppButton>)}</XStack>:null}
  {snapshot.source==='ble'?<XStack gap="$2" flexWrap="wrap">{(['ERROR','WARN','INFO','DEBUG'] as const).map(level=><AppButton key={`level-${level}`} size="sm" variant="ghost" disabled={snapshot.source!=='ble'||snapshot.busy||!visible||busy} onPress={()=>void controller.setLocalLevel(level)}>{`Set local ${level}`}</AppButton>)}</XStack>:<Text color="$colorMuted">Server history levels are captured metadata, read-only.</Text>}

  {snapshot.source==='ble'&&snapshot.meta?<Text color="$colorMuted">Build {snapshot.meta.build_id} · boot {snapshot.meta.boot_id} · local level {snapshot.level} · lost {snapshot.meta.delivery_lost} · overwritten {snapshot.meta.overwritten}</Text>:null}
  <AppButton size="sm" variant="ghost" disabled={busy} onPress={()=>controller.disconnect()}>Putuskan BLE</AppButton>
  </>:null}
  {advanced?<Text color="$colorMuted" {...textStyles.caption}>Cursor {snapshot.after}{snapshot.source==='https'&&snapshot.history?` · tertua ${snapshot.history.oldest_available}`:''}{snapshot.source==='https'&&snapshot.history?.has_more?' · masih ada':''}</Text>:null}
  <DebugEventList events={events} advanced={advanced} source={snapshot.source} />
  </>}
  </>:null}
 </YStack></Card>;
}
