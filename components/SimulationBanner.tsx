import { useEffect, useState } from 'react';
import { apiClient } from '../lib/api/client';
import { SimulationModeBadge } from './SimulationModeBadge';

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

  return <SimulationModeBadge description={copy.description.replace('Data sensor pada halaman ini dibuat secara sintetis.', 'Data sintetis')} />;
}
