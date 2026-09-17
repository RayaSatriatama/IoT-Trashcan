import { useEffect, useState } from 'react';
import { Card, Text, YStack } from 'tamagui';

import { apiClient } from '../lib/api/client';

export function simulationBannerCopy(simulation: boolean) {
  return simulation
    ? { title: 'MODE SIMULASI', description: 'Data sensor pada halaman ini dibuat secara sintetis.' }
    : null;
}

export function SimulationBanner() {
  const [simulation, setSimulation] = useState(false);

  useEffect(() => {
    const controller = new AbortController();
    apiClient.fetchRuntime(controller.signal)
      .then((runtime) => setSimulation(runtime.simulation))
      .catch(() => undefined);
    return () => controller.abort();
  }, []);

  const copy = simulationBannerCopy(simulation);
  if (!copy) return null;

  return (
    <Card
      borderWidth={1}
      borderColor="$statusWaspada"
      backgroundColor="$surface"
      borderRadius="$3"
      paddingHorizontal="$4"
      paddingVertical="$3"
      marginBottom="$4"
      role="status"
      aria-label="Mode simulasi aktif"
    >
      <YStack gap="$1">
        <Text color="$statusWaspada" fontSize="$2" fontWeight="800">
          {copy.title}
        </Text>
        <Text color="$colorMuted" fontSize="$2">
          {copy.description}
        </Text>
      </YStack>
    </Card>
  );
}
