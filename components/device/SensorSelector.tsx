import { Adapt, Select, Sheet, YStack } from 'tamagui';
import { CALIBRATION_SENSOR_IDS, type CalibrationSensorId } from '../../lib/api/calibration';
import { calibrationSensorLabel } from '../../lib/ui/deviceTools';
import { ChevronDown } from '../../lib/ui/icons';
import { textStyles } from '../../lib/ui/typography';

export function SensorSelector({ value, disabled = false, onChange }: { value: CalibrationSensorId; disabled?: boolean; onChange: (value: CalibrationSensorId) => void }) {
  return <YStack width="100%" minWidth={0}>
    <Select value={value} onValueChange={next => { if (!disabled && CALIBRATION_SENSOR_IDS.includes(next as CalibrationSensorId)) onChange(next as CalibrationSensorId); }} name="calibration-sensor">
      <Select.Trigger id="calibration-sensor" aria-label="Pilih sensor" disabled={disabled} height={44} minHeight={44} width="100%" borderRadius={10} borderColor="$borderColor" backgroundColor="$surface">
        <Select.Value placeholder="Pilih sensor" fontFamily="$body" fontSize={16} fontWeight="400" />
        <Select.Icon><ChevronDown size={16} color="#10275B" aria-hidden /></Select.Icon>
      </Select.Trigger>
      <Select.Content><Select.Viewport minWidth={280}>
        {CALIBRATION_SENSOR_IDS.map((id, index) => <Select.Item key={id} value={id} index={index} minHeight={44}><Select.ItemText>{calibrationSensorLabel(id)}</Select.ItemText><Select.ItemIndicator /></Select.Item>)}
      </Select.Viewport></Select.Content>
      <Select.Adapt when="compact"><Sheet modal dismissOnSnapToBottom snapPoints={[60]}><Sheet.Frame padding="$4" backgroundColor="$surface" borderTopLeftRadius="$5" borderTopRightRadius="$5"><Sheet.Handle /><Sheet.ScrollView><Adapt.Contents /></Sheet.ScrollView></Sheet.Frame><Sheet.Overlay backgroundColor="rgba(10,31,70,0.24)" /></Sheet></Select.Adapt>
    </Select>
  </YStack>;
}
