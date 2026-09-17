import campus from '../../assets/locations/gedung_kampus.png';
import canteen from '../../assets/locations/kantin.png';
import laboratory from '../../assets/locations/laboratorium.png';
import library from '../../assets/locations/perpustakaan.png';
import dormitory from '../../assets/locations/asrama.png';

export const LOCATION_IMAGES = {
  campus,
  canteen,
  laboratory,
  library,
  dormitory,
} as const;

export function resolveLocationImage(name: string) {
  const normalized = name.toLowerCase();
  if (normalized.includes('kantin')) return LOCATION_IMAGES.canteen;
  if (normalized.includes('lab')) return LOCATION_IMAGES.laboratory;
  if (normalized.includes('perpustakaan')) return LOCATION_IMAGES.library;
  if (normalized.includes('asrama')) return LOCATION_IMAGES.dormitory;
  return LOCATION_IMAGES.campus;
}
