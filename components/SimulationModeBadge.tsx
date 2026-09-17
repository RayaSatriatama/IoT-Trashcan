import { FlaskConical } from '../lib/ui/icons';
import { Card, Text, XStack } from 'tamagui';

export interface SimulationModeBadgeProps {
  description?: string;
}

export function SimulationModeBadge({ description = 'Data sintetis' }: SimulationModeBadgeProps) {
  return (
    <Card
      alignSelf="flex-start"
      borderWidth={1}
      borderColor="$statusWaspada"
      backgroundColor="$orangeSoft"
      borderRadius="$6"
      marginBottom="$3"
      paddingHorizontal="$3"
      paddingVertical="$2"
      role="status"
      aria-label="Mode simulasi aktif"
    >
      <XStack alignItems="center" gap="$2">
        <FlaskConical size={15} color="#C76A00" strokeWidth={2.3} aria-hidden={true} />
        <Text color="#B86200" fontSize="$1" fontWeight="800" letterSpacing={0.7}>MODE SIMULASI</Text>
        <Text color="$colorMuted" fontSize="$1">· {description}</Text>
      </XStack>
    </Card>
  );
}
