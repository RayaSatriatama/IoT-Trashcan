import type { LucideIcon } from 'lucide-react-native';
import { Card, Text, XStack, YStack } from 'tamagui';
import { textStyles } from '../lib/ui/typography';

export type KpiTone = 'green' | 'blue' | 'orange' | 'red';

const tones: Record<KpiTone, { color: string; background: string }> = {
  green: { color: '#0A9B62', background: '#E1F6EC' },
  blue: { color: '#2487E8', background: '#E6F1FF' },
  orange: { color: '#EF7D20', background: '#FFF0E3' },
  red: { color: '#EF3D4A', background: '#FFE9EB' },
};

export interface KpiCardProps {
  label: string;
  value: number | string;
  description?: string;
  icon: LucideIcon;
  tone?: KpiTone;
  selected?: boolean;
  onPress?: () => void;
}

export function KpiCard({ label, value, description, icon: Icon, tone = 'green', selected = false, onPress }: KpiCardProps) {
  const palette = tones[tone];
  return (
    <Card
      flex={1}
      minWidth={0}
      minHeight={132}
      padding="$4"
      gap="$3"
      borderColor={selected ? palette.color : '$borderColor'}
      backgroundColor={palette.background}
      borderRadius="$4"
      borderWidth={selected ? 2 : 1}
      pressStyle={onPress ? { opacity: 0.84, scale: 0.99 } : undefined}
      hoverStyle={onPress ? { borderColor: palette.color, shadowColor: palette.color, shadowOpacity: 0.12, shadowRadius: 8 } : undefined}
      focusStyle={onPress ? { borderColor: palette.color, borderWidth: 2, outlineColor: palette.color, outlineWidth: 2, outlineStyle: 'solid' } : undefined}
      onPress={onPress}
      role={onPress ? 'button' : 'group'}
      aria-label={label}
      aria-pressed={onPress ? selected : undefined}
      tabIndex={onPress ? 0 : undefined}
      cursor={onPress ? 'pointer' : undefined}
    >
      <XStack alignItems="center" justifyContent="space-between" gap="$3">
        <YStack gap="$1" flex={1}>
          <Text color="$colorMuted" {...textStyles.caption} fontWeight="700" minHeight={20} numberOfLines={2}>{label}</Text>
          <Text color={palette.color} {...textStyles.numericKpi} $compact={textStyles.numericKpiCompact} letterSpacing={-1} fontVariant={['tabular-nums']}>{value}</Text>
        </YStack>
        <YStack width={44} height={44} borderRadius="$6" backgroundColor="$surface" alignItems="center" justifyContent="center">
          <Icon size={22} color={palette.color} strokeWidth={2.2} aria-hidden={true} />
        </YStack>
      </XStack>
      {description ? <Text color="$colorMuted" {...textStyles.caption} $compact={{ display: 'none' }}>{description}</Text> : null}
    </Card>
  );
}
