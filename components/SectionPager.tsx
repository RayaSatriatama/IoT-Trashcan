import { ArrowLeft, ArrowRight } from '../lib/ui/icons';
import { Button, Text, XStack } from 'tamagui';

export interface SectionPagerProps {
  page: number;
  totalPages: number;
  onPrevious: () => void;
  onNext: () => void;
}

export function SectionPager({ page, totalPages, onPrevious, onNext }: SectionPagerProps) {
  const safeTotal = Math.max(1, totalPages);
  const safePage = Math.min(safeTotal, Math.max(1, page));
  if (safeTotal <= 1) return null;

  return (
    <XStack alignItems="center" gap="$1" aria-label={`Halaman ${safePage} dari ${safeTotal}`}>
      <Button
        width={40}
        height={40}
        padding={0}
        borderRadius="$3"
        backgroundColor="$surface"
        borderWidth={1}
        borderColor="$borderColor"
        disabled={safePage <= 1}
        opacity={safePage <= 1 ? 0.45 : 1}
        onPress={onPrevious}
        aria-label="Halaman sebelumnya"
        type="button"
      >
        <ArrowLeft size={17} color="#10275B" strokeWidth={2.2} aria-hidden={true} />
      </Button>
      <Text minWidth={54} textAlign="center" fontSize={14} lineHeight={20} fontWeight="700" color="$color" fontVariant={['tabular-nums']}>
        {safePage} / {safeTotal}
      </Text>
      <Button
        width={40}
        height={40}
        padding={0}
        borderRadius="$3"
        backgroundColor="$surface"
        borderWidth={1}
        borderColor="$borderColor"
        disabled={safePage >= safeTotal}
        opacity={safePage >= safeTotal ? 0.45 : 1}
        onPress={onNext}
        aria-label="Halaman berikutnya"
        type="button"
      >
        <ArrowRight size={17} color="#10275B" strokeWidth={2.2} aria-hidden={true} />
      </Button>
    </XStack>
  );
}
