import { Text, XStack, YStack, useMedia } from 'tamagui';
import type { WorkflowStep } from '../../lib/ui/deviceTools';
import { useReducedMotion } from '../../lib/ui/motionContext';
import { textStyles } from '../../lib/ui/typography';
import { DescriptionHint } from '../SectionHeader';

/** Informative stages, never navigation or a substitute for a device ACK. */
export function DeviceWorkflowSteps({ steps, current }: { steps: readonly WorkflowStep[]; current: number }) {
  const reduced = useReducedMotion();
  const calibration = steps.some((step) => step.id === 'diagnostics');
  const stages: WorkflowStep[] = calibration ? [
    { id: 'major-1', title: 'Kriteria & Capture', description: 'Periksa kesiapan sensor, masukkan referensi dan kriteria yang diminta, lalu capture melalui ACK perangkat.' },
    { id: 'major-2', title: 'Validasi & Simpan', description: 'Validasi bukti, tinjau proposal dan identitasnya, lalu simpan hanya setelah persetujuan eksplisit.' },
  ] : [...steps];
  const stageIndex = calibration
    ? (current >= steps.findIndex((step) => step.id === 'validate') ? 1 : 0)
    : Math.min(current, stages.length - 1);
  const completed = calibration ? steps[current]?.id === 'done' : current >= steps.length;
  const progress = completed ? 1 : stageIndex;
  return <YStack gap="$3" aria-label="Tahapan proses">
    <XStack height={4} backgroundColor="$primarySoft" borderRadius="$6" overflow="hidden" role="progressbar" aria-label="Kemajuan tahap" aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(progress * 100)}>
      <XStack width="100%" height="100%" backgroundColor="$primary" scaleX={progress} transformOrigin="left center" transition={reduced ? undefined : 'standard'} />
    </XStack>
    <XStack gap="$2" flexWrap="nowrap">
      {stages.map((step, index) => <YStack key={step.id} flex={1} minWidth={0} padding="$2" gap="$2" borderWidth={1} borderColor={index === stageIndex ? '$primary' : '$borderColor'} borderRadius="$3" backgroundColor={index === stageIndex ? '$primarySoft' : '$surface'} aria-current={index === stageIndex && !completed ? 'step' : undefined} aria-label={`${index + 1}. ${step.title}`}>
        <XStack alignItems="center" gap="$2"><Text color={index <= stageIndex ? '$primary' : '$colorMuted'} {...textStyles.caption} fontWeight="800">{index + 1}</Text><Text color="$color" {...textStyles.bodySecondary} fontWeight="700" flex={1} flexShrink={1}>{step.title}</Text><DescriptionHint description={step.description} ariaLabel={`Info tahap ${step.title}`} /></XStack>
      </YStack>)}
    </XStack>
    <XStack borderLeftWidth={3} borderColor="$primary" backgroundColor="$background" padding="$3" borderRadius="$3" alignItems="center" justifyContent="space-between" gap="$2">
      <YStack flex={1} minWidth={0} gap="$1"><Text color="$primary" {...textStyles.overline}>{completed ? 'PROSES SELESAI' : 'TINDAKAN SAAT INI'}</Text><Text color="$color" {...textStyles.cardTitle}>{completed ? 'Profil dikonfirmasi perangkat' : calibration ? steps[current]?.title ?? 'Menunggu konfirmasi perangkat' : stages[stageIndex]?.title}</Text></YStack>
    </XStack>
  </YStack>;
}
