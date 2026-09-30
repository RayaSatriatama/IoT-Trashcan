import { useEffect, useState } from 'react';
import { Button, Popover, PopoverContent, PopoverTrigger, Text, XStack, YStack } from 'tamagui';

import { apiClient } from '../lib/api/client';
import type { RuntimeResponse } from '../lib/api/types';
import { FlaskConical } from '../lib/ui/icons';
import { textStyles } from '../lib/ui/typography';
import { useReducedMotion } from '../lib/ui/motionContext';

export function simulationIndicatorVisible(runtime: RuntimeResponse): boolean {
  return runtime.simulation && runtime.simulationIndicator;
}

export function SimulationIndicator() {
  const [runtime, setRuntime] = useState<RuntimeResponse | null>(null);
  const reduceMotion = useReducedMotion();

  useEffect(() => {
    const controller = new AbortController();
    apiClient.fetchRuntime(controller.signal)
      .then(setRuntime)
      .catch(() => undefined);
    return () => controller.abort();
  }, []);

  if (!runtime || !simulationIndicatorVisible(runtime)) return null;

  return (
    <Popover placement="bottom-end" allowFlip>
      <PopoverTrigger asChild>
        <Button
          minHeight={44}
          minWidth={44}
          paddingHorizontal="$3"
          borderRadius="$3"
          borderWidth={1}
          borderColor="$statusWaspada"
          backgroundColor="$orangeSoft"
          color="#B86200"
          hoverStyle={{ backgroundColor: '$statusWaspadaSoft' }}
          pressStyle={{ opacity: 0.78 }}
          aria-label="Mode simulasi aktif"
          type="button"
          transition={reduceMotion ? undefined : 'quick'}
        >
          <XStack alignItems="center" gap="$2">
            <FlaskConical size={17} color="#B86200" strokeWidth={2.2} aria-hidden={true} />
            <Text color="#B86200" {...textStyles.caption} fontWeight="800" $compact={{ display: 'none' }}>
              SIMULASI
            </Text>
          </XStack>
        </Button>
      </PopoverTrigger>
      <PopoverContent elevate bordered borderColor="$borderColor" backgroundColor="$surface" padding="$3" width={220}>
        <YStack gap="$1">
          <Text color="$color" {...textStyles.bodySecondary} fontWeight="800">Mode simulasi aktif</Text>
          <Text color="$colorMuted" {...textStyles.caption}>Data sintetis</Text>
        </YStack>
      </PopoverContent>
    </Popover>
  );
}
