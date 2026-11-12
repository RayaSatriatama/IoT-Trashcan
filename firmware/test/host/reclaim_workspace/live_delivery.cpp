#define main prior_reclaim_main
#include "reclaim.cpp"
#undef main
#include <map>
#include "storage/live_archive_store.h"
#include "telemetry_archive/live_batch_runtime.h"
namespace {
constexpr const char* batch_path="/sigap/live-v3.batches";
constexpr const char* journal_path="/sigap/live-v3.acks";
struct LiveHarness {
  Fixture old;
  LiveArchiveStore store;
  FsImmutableBatchStore batches;
  FsAckJournalStore journal;
  LiveBatchRuntime runtime;
  Eligibility eligibility;
  std::map<std::string,std::vector<uint8_t>> retained;
  LiveHarness() {
    const uint8_t sentinel[]={0x13,0x37,0x56};
    for (const auto* p:{"/sigap/archive.ckp","/sigap/gaps.bin","/sigap/batches.bin",
                       "/sigap/batches.ckp","/sigap/acks.bin","/sigap/acks.ckp"}) {
      old.disk.append_raw(p,sentinel,sizeof(sentinel));retained[p]=old.disk.bytes(p);
    }
    retained[archive_path]=old.original;
    old.disk.set_capacity(old.disk.used_bytes()+131072U);
    store.set_capacity_probe(Probe::read,&old.probe);
    store.set_selection_context(device(),boot(),Eligibility::check,&eligibility);
    assert(store.begin(old.disk,65536));
    assert(batches.begin(old.disk,65536,batch_path,"/sigap/live-v3.batches.ckp"));
    assert(journal.begin(old.disk,journal_path,"/sigap/live-v3.acks.ckp"));
    runtime.bind(&store,&batches,&journal);
  }
  Ack enqueue(uint64_t sequence) {
    assert(store.append(sample(sequence))==AppendStatus::Durable);
    assert(runtime.poll()==BatchRuntimeStatus::Durable);
    return ack_for(runtime.batch());
  }
  void preserved() {
    for (const auto& item:retained) assert(old.disk.bytes(item.first.c_str())==item.second);
    assert(old.disk.active_handles==0);
  }
};
void bounded() {
  LiveHarness h;
  for(unsigned i=0;i<320;++i) {
    const auto ack=h.enqueue(1000+i);
    assert(h.runtime.apply_ack(ack)==AckPersistStatus::Durable);
    if(h.batches.pending_bytes()!=0 || !h.old.disk.bytes(journal_path).empty()) {
      std::puts("FAIL completed delivery leaves unbounded batch or ACK bytes");std::exit(1);
    }
    assert(h.store.pending_bytes()==0 && !h.runtime.pending());h.preserved();
  }
}
void restarted(unsigned phase,bool corrupt=false) {
  LiveHarness h;
  const auto ack=h.enqueue(1000);
  const auto next=sample(1001);
  assert(h.store.append(next)==AppendStatus::Durable);
  if(phase>=1) { assert(h.journal.apply_ack(ack)==AckPersistStatus::Durable);assert(h.journal.checkpoint()); }
  if(phase>=2 && phase!=6) assert(h.store.reclaim_pending(sample(1000).identity,sample(1000).hash));
  if(phase>=3) assert(h.journal.reclaim(ack)==AckPersistStatus::Durable);
  if(phase>=4 && phase!=6) assert(h.batches.retire_completed(ack));
  if(phase>=5 && phase!=6) assert(h.journal.clear_completed());
  if(corrupt) { const uint8_t broken[]={0xff,0x01};h.old.disk.append_raw(journal_path,broken,sizeof(broken)); }
  const auto journal_original=h.old.disk.bytes(journal_path);
  LiveArchiveStore store;
  store.set_capacity_probe(Probe::read,&h.old.probe);
  store.set_selection_context(device(),boot(),Eligibility::check,&h.eligibility);
  assert(store.begin(h.old.disk,65536));
  FsImmutableBatchStore batches; FsAckJournalStore journal;
  assert(batches.begin(h.old.disk,65536,batch_path,"/sigap/live-v3.batches.ckp"));
  assert(journal.begin(h.old.disk,journal_path,"/sigap/live-v3.acks.ckp"));
  LiveBatchRuntime runtime;runtime.bind(&store,&batches,&journal);
  ImmutableBatch workspace;
  const auto before=h.old.disk.bytes(batch_path);
  const bool recovered=runtime.recover_completed(workspace);
  if(phase==6) { // HOST FAULT: impossible ordering, receipt says reclaimed but source remains.
    assert(!recovered && runtime.poll()==BatchRuntimeStatus::Retryable);
    assert(h.old.disk.bytes(batch_path)==before && store.contains(sample(1000).identity,sample(1000).hash));
  } else {
    assert(recovered);
    if(phase==0) { // No receipt must never become a fabricated ACK.
      assert(store.contains(sample(1000).identity,sample(1000).hash));
      assert(runtime.poll()==BatchRuntimeStatus::Durable);
      assert(runtime.batch().body_hash==ack.body_hash && h.old.disk.bytes(batch_path)==before);
      assert(runtime.apply_ack(ack)==AckPersistStatus::Durable);
    }
    assert(!store.contains(sample(1000).identity,sample(1000).hash));
    assert(store.contains(next.identity,next.hash) && store.pending_bytes()==next.size);
    assert(batches.pending_bytes()==0 && h.old.disk.bytes(journal_path).empty());
  }
  if(corrupt) assert(h.old.disk.bytes("/sigap/live-v3.acks.tail")==journal_original);
  h.preserved();
}
void invalid_ack() {
  LiveHarness h;auto ack=h.enqueue(1000);
  const auto body=std::string(h.runtime.batch().exact_body());
  const auto record=sample(1000);
  const auto journal=h.old.disk.bytes(journal_path);
  ack.body_hash[0]^=1U;
  assert(h.runtime.apply_ack(ack)!=AckPersistStatus::Durable);
  assert(h.store.contains(record.identity,record.hash));
  assert(h.old.disk.bytes(journal_path)==journal);
  assert(h.runtime.poll()==BatchRuntimeStatus::Durable && h.runtime.batch().exact_body()==body);
  h.preserved();
}
void short_batch() {
  LiveHarness h;
  const auto record=sample(1000);
  assert(h.store.append(record)==AppendStatus::Durable);
  h.old.disk.short_write_once(2);
  assert(h.runtime.poll()==BatchRuntimeStatus::Retryable);
  assert(h.store.contains(record.identity,record.hash));
  assert(h.runtime.poll()==BatchRuntimeStatus::Durable);
  const auto ack=ack_for(h.runtime.batch());
  assert(h.runtime.apply_ack(ack)==AckPersistStatus::Durable);
  // Reopen verifies no partial frame was hidden before acknowledged bytes.
  FsImmutableBatchStore reopened;
  assert(reopened.begin(h.old.disk,65536,batch_path,"/sigap/live-v3.batches.ckp"));
  assert(reopened.pending_bytes()==0);h.preserved();
}
void short_journal() {
  LiveHarness h;
  auto ack=h.enqueue(1000);
  h.old.disk.short_write_once(2);
  assert(h.runtime.apply_ack(ack)==AckPersistStatus::Retryable);
  const auto interrupted=h.old.disk.bytes(journal_path);
  assert(h.store.contains(sample(1000).identity,sample(1000).hash));
  assert(h.runtime.apply_ack(ack)==AckPersistStatus::Durable);
  FsAckJournalStore reopened;
  assert(reopened.begin(h.old.disk,journal_path,"/sigap/live-v3.acks.ckp"));
  assert(reopened.empty() && h.batches.pending_bytes()==0);
  assert(h.old.disk.bytes("/sigap/live-v3.acks.tail")==interrupted);
  h.preserved();
}
}
#include "crc_replay_cases.inc"
int main(int argc,char** argv) {
  assert(argc==2);const std::string scenario=argv[1];
  if(scenario=="bounded") bounded();
  else if(scenario=="restart") for(unsigned phase=0;phase<=6;++phase) restarted(phase);
  else if(scenario=="corrupt_tail") restarted(3,true);
  else if(scenario=="invalid_ack") invalid_ack();
  else if(scenario=="short_batch") short_batch();
  else if(scenario=="short_journal") short_journal();
  else if(scenario.rfind("crc_",0)==0) crc_replay_case(scenario);
  else return 2;
  std::printf("PASS live-delivery %s\n",scenario.c_str());return 0;
}
