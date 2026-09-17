export type CanonicalStatus =
  | 'Normal'
  | 'Waspada'
  | 'Angkat'
  | 'Safety Alert'
  | 'Offline'
  | 'Data Invalid';

export interface OverviewResponse {
  total_locations: number;
  total_devices: number;
  pickup_required: number;
  safety_alerts: number;
  priority_locations: LocationListItem[];
  recent_activity: EventDTO[];
}

export interface LocationListItem {
  id: string;
  name: string;
  address_text: string | null;
  status: CanonicalStatus | null;
}

export interface LocationListResponse {
  locations: LocationListItem[];
}

export interface LocationDetail extends LocationListItem {
  devices: DeviceDTO[];
}

export interface DeviceDTO {
  id: string;
  location_id: string;
  device_code: string;
  placement_label: string | null;
  online_state: 'Online' | 'Offline' | null;
  last_seen: string | null;
  status: CanonicalStatus | null;
  slots: SlotDTO[];
}

export interface SlotDTO {
  stream: 'organic' | 'inorganic' | 'b3' | string;
  fill: number | null;
  weight: number | null;
  weight_utilization: number | null;
  status: CanonicalStatus | null;
  dominant_reason: string | null;
  safety_state: string | null;
  recorded_at: string | null;
}

export interface ResearchSlotDTO {
  stream: string;
  recorded_at: string | null;
  raw: {
    fill: number | null;
    weight: number | null;
    gas_raw: number | null;
    temperature: number | null;
  };
  normalized: {
    fill_pct: number | null;
    weight_utilization: number | null;
    ogi: number | null;
    accumulation_hours: number | null;
    temperature: number | null;
  };
  sensor_health: unknown;
  fis_score: number | null;
  fis_status: string | null;
  fis_version: string | null;
  dominant_reason: string | null;
  safety_state: string | null;
}

export interface ResearchResponse {
  device_id: string;
  slots: ResearchSlotDTO[];
}

export interface EventDTO {
  id: number;
  device_id: string;
  slot_id: string | null;
  event_type: string;
  severity: string;
  started_at: string;
  ended_at: string | null;
}
