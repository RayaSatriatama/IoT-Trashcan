#define main prior_reclaim_main
#include "reclaim.cpp"
#undef main
#include "storage/live_archive_store.h"
int main() {
  Fixture old;
  old.disk.set_capacity(old.disk.used_bytes()+131072U);
  LiveArchiveStore live;
  live.set_capacity_probe(Probe::read,&old.probe);
  assert(live.begin(old.disk,old.original.size()));
  live.set_selection_context(device(),boot(),[](const ImmutableRecordBytes&,void*) noexcept { return true; },nullptr);
  const auto fresh=sample(999);
  assert(live.append(fresh)==AppendStatus::Durable);
  assert(live.contains(fresh.identity,fresh.hash));
  assert(live.retained_legacy_bytes()==old.original.size());
  assert(live.health()==ArchiveHealth::Degraded);
  assert(old.disk.bytes(archive_path)==old.original);
  assert(live.reclaim_pending(fresh.identity,fresh.hash));
  assert(live.pending_bytes()==0);
  assert(old.disk.bytes(archive_path)==old.original);
  assert(old.disk.active_handles==0);
  // Admission is before every mutating call, including an exact boundary.
  const auto next=sample(1000);
  const uint64_t required=LiveArchiveStore::kWorkspaceReserveBytes+2U*next.size;
  old.disk.set_capacity(old.disk.used_bytes()+required-1U);
  old.disk.reset_mutations();
  assert(live.append(next)==AppendStatus::LoggingBlocked);
  assert(old.disk.mutations.total()==0 && live.pending_bytes()==0);
  old.disk.set_capacity(old.disk.used_bytes()+required);
  assert(live.append(next)==AppendStatus::Durable);
  old.disk.set_capacity(old.disk.used_bytes());
  old.disk.reset_mutations();
  assert(live.append(next)==AppendStatus::Durable); // immutable retry needs no new space
  assert(old.disk.mutations.total()==0);
  assert(live.append(sample(1001))==AppendStatus::LoggingBlocked);
  assert(old.disk.mutations.total()==0);
  old.probe.known=false;
  assert(live.append(sample(1001))==AppendStatus::Retryable);
  assert(old.disk.mutations.total()==0);
  LiveArchiveStore no_probe;
  assert(no_probe.begin(old.disk,65536));
  old.disk.reset_mutations();
  assert(no_probe.append(sample(1002))==AppendStatus::Retryable);
  assert(old.disk.mutations.total()==0);
  assert(old.disk.bytes(archive_path)==old.original);
  assert(live.contains(next.identity,next.hash) && old.disk.active_handles==0);
  std::puts("PASS isolated append/reclaim, exact threshold, zero/unknown/absent capacity, idempotent retry and historical bytes");
}
