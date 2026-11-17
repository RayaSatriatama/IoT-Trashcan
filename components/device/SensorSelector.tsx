import { Adapt, Select, Sheet, YStack } from 'tamagui';
import { CALIBRATION_SENSOR_IDS, type CalibrationSensorId } from '../../lib/api/calibration';
import { calibrationSensorLabel } from '../../lib/ui/deviceTools';
import { ChevronDown } from '../../lib/ui/icons';
import { textStyles } from '../../lib/ui/typography';

type PhysicalSensorId = typeof CALIBRATION_SENSOR_IDS[number];

export interface SensorSelectorOption { value: CalibrationSensorId; label?: string }

export function SensorSelector({ value, disabled = false, onChange, options = CALIBRATION_SENSOR_IDS.map(sensor => ({ value: sensor })), id = 'calibration-sensor', label = 'Pilih sensor' }: {
  value: CalibrationSensorId;
  disabled?: boolean;
  onChange: (value: PhysicalSensorId) => void;
  options?: readonly SensorSelectorOption[];
  id?: string;
  label?: string;
}) {
  return <YStack width="100%" minWidth={0}>
    <Select value={value} onValueChange={next => { const physical = CALIBRATION_SENSOR_IDS.find(sensor => sensor === next); if (!disabled && physical !== undefined && options.some(option => option.value === physical)) onChange(physical); }} name={id}>
      <Select.Trigger id={id} aria-label={label} disabled={disabled} height={44} minHeight={44} width="100%" borderRadius={10} borderColor="$borderColor" backgroundColor="$surface">
        <Select.Value placeholder="Pilih sensor" fontFamily="$body" fontSize={16} fontWeight="400" />
        <Select.Icon><ChevronDown size={16} color="#10275B" aria-hidden /></Select.Icon>
      </Select.Trigger>
      <Select.Content><Select.Viewport minWidth={280}>
        {options.map((option, index) => <Select.Item key={option.value} value={option.value} index={index} minHeight={44}><Select.ItemText>{option.label ?? calibrationSensorLabel(option.value)}</Select.ItemText><Select.ItemIndicator /></Select.Item>)}
      </Select.Viewport></Select.Content>
      <Select.Adapt when="compact"><Sheet modal dismissOnSnapToBottom snapPoints={[60]}><Sheet.Frame padding="$4" backgroundColor="$surface" borderTopLeftRadius="$5" borderTopRightRadius="$5"><Sheet.Handle /><Sheet.ScrollView><Adapt.Contents /></Sheet.ScrollView></Sheet.Frame><Sheet.Overlay backgroundColor="rgba(10,31,70,0.24)" /></Sheet></Select.Adapt>
    </Select>
  </YStack>;
}
