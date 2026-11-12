#pragma once
// Host-only media faults constructed from frames emitted by the production
// journal serializer. No network response or operational data is fabricated.
namespace {
void crc_replay_case(const std::string& scenario) {
  LiveHarness h;
  const auto ack=h.enqueue(1000);
  assert(h.journal.apply_ack(ack)==AckPersistStatus::Durable);
  assert(h.journal.checkpoint());
  const auto source=sample(1000);
  assert(h.store.reclaim_pending(source.identity,source.hash));
  assert(h.journal.reclaim(ack)==AckPersistStatus::Durable);
  const auto legal=h.old.disk.bytes(journal_path);
  assert(legal.size()==3U*kAckJournalFrameBytes);
  std::vector<std::vector<uint8_t>> frame;
  for(unsigned i=0;i<3;++i) frame.emplace_back(
      legal.begin()+i*kAckJournalFrameBytes,legal.begin()+(i+1U)*kAckJournalFrameBytes);
  const auto rehash=[](std::vector<uint8_t>& bytes) {
    const auto crc=sigap::storage::archive_crc32(bytes.data(),kAckJournalChecksumOffset);
    for(unsigned i=0;i<4;++i) bytes[kAckJournalChecksumOffset+i]=static_cast<uint8_t>(crc>>(8U*i));
  };
  std::vector<unsigned> order;
  bool completion=false;
  if(scenario=="crc_orphan_reclaimed") order={2};
  else if(scenario=="crc_orphan_acked") order={1};
  else if(scenario=="crc_pending_reclaimed") order={0,2};
  else if(scenario=="crc_reclaimed_before_acked") order={0,2,1};
  else if(scenario=="crc_duplicate_reclaimed") { order={0,1,2,2};completion=true; }
  else if(scenario=="crc_duplicate_acked") { order={0,1,1,2};completion=true; }
  else if(scenario=="crc_conflicting_hash") {
    frame[1][8U+kAckJournalIdBytes+kAckJournalDeviceIdBytes]^=0x80U;
    rehash(frame[1]);order={0,1,2};
  } else if(scenario=="crc_conflicting_count") {
    assert(ack.record_count==1U);
    frame[1][8U+kAckJournalIdBytes+kAckJournalDeviceIdBytes+32U]=2U;
    rehash(frame[1]);order={0,1,2};
  } else if(scenario=="crc_conflicting_owner") {
    frame[1][8U+kAckJournalIdBytes]^=0x80U;
    rehash(frame[1]);order={0,1,2};
  } else if(scenario=="crc_conflicting_pending") {
    auto conflicting=frame[0];
    conflicting[8U+kAckJournalIdBytes+kAckJournalDeviceIdBytes]^=0x80U;
    rehash(conflicting);frame.push_back(conflicting);order={0,3,1,2};
  } else { assert(false && "unknown CRC replay scenario"); }
  std::vector<uint8_t> faulted;
  for(auto index:order) {
    const auto& value=frame[index];
    const auto computed=sigap::storage::archive_crc32(value.data(),kAckJournalChecksumOffset);
    uint32_t stored=0U;
    for(unsigned i=0;i<4;++i) stored|=static_cast<uint32_t>(value[kAckJournalChecksumOffset+i])<<(8U*i);
    assert(computed==stored); // This is semantic rejection, not CRC rejection.
    faulted.insert(faulted.end(),value.begin(),value.end());
  }
  auto file=h.old.disk.open(journal_path,FILE_WRITE);
  assert(file && file.write(faulted.data(),faulted.size())==faulted.size());file.close();
  const auto immutable=h.old.disk.bytes(batch_path);
  FsImmutableBatchStore batches;FsAckJournalStore journal;
  assert(batches.begin(h.old.disk,65536,batch_path,"/sigap/live-v3.batches.ckp"));
  assert(journal.begin(h.old.disk,journal_path,"/sigap/live-v3.acks.ckp"));
  const std::size_t prefix_frames = scenario=="crc_pending_reclaimed" ? 1U :
      scenario=="crc_reclaimed_before_acked" ? 1U :
      scenario=="crc_duplicate_reclaimed" ? 3U :
      scenario=="crc_duplicate_acked" ? 2U :
      scenario=="crc_conflicting_hash" || scenario=="crc_conflicting_count" ||
      scenario=="crc_conflicting_owner" || scenario=="crc_conflicting_pending" ? 1U : 0U;
  const std::vector<uint8_t> expected_prefix(faulted.begin(), faulted.begin()+prefix_frames*kAckJournalFrameBytes);
  const std::vector<uint8_t> expected_suffix(faulted.begin()+prefix_frames*kAckJournalFrameBytes, faulted.end());
  assert(h.old.disk.bytes(journal_path)==expected_prefix);
  assert(h.old.disk.bytes("/sigap/live-v3.acks.tail")==faulted);
  assert(!h.old.disk.exists("/sigap/live-v3.acks.recover"));
  assert(h.old.disk.active_handles==0);
  const auto proof=journal.completion_proof(ack);
  const std::size_t expected_proof=scenario=="crc_duplicate_reclaimed" ? 2U :
      scenario=="crc_duplicate_acked" ? 1U : 0U;
  assert(proof==expected_proof);
  const auto forensic=h.old.disk.bytes("/sigap/live-v3.acks.tail");
  assert(std::equal(expected_prefix.begin(),expected_prefix.end(),forensic.begin()));
  assert(std::vector<uint8_t>(forensic.begin()+expected_prefix.size(),forensic.end())==expected_suffix);
  assert(h.old.disk.bytes(batch_path)==immutable);
  // Check stable reopen before legitimate completion is allowed to clear proof.
  FsAckJournalStore reopened;
  assert(reopened.begin(h.old.disk,journal_path,"/sigap/live-v3.acks.ckp"));
  assert(h.old.disk.bytes(journal_path)==expected_prefix);
  assert(reopened.completion_proof(ack)==expected_proof);
  assert(h.old.disk.bytes("/sigap/live-v3.acks.tail")==faulted);
  assert(!h.old.disk.exists("/sigap/live-v3.acks.recover"));
  assert(h.old.disk.active_handles==0);
  LiveBatchRuntime runtime;runtime.bind(&h.store,&batches,&journal);
  ImmutableBatch workspace;
  const bool recovered=runtime.recover_completed(workspace);
  assert(recovered==completion);
  if(completion) {
    assert(batches.pending_bytes()==0U && journal.empty());
  } else {
    assert(h.old.disk.bytes(batch_path)==immutable && batches.pending_bytes()!=0U);
    assert(runtime.poll()==BatchRuntimeStatus::Retryable);
  }
  const auto after_prefix=completion ? std::vector<uint8_t>{} : expected_prefix;
  assert(h.old.disk.bytes(journal_path)==after_prefix);
  assert(h.old.disk.bytes("/sigap/live-v3.acks.tail")==faulted);
  assert(!h.old.disk.exists("/sigap/live-v3.acks.recover"));
  FsAckJournalStore after_cleanup;
  assert(after_cleanup.begin(h.old.disk,journal_path,"/sigap/live-v3.acks.ckp"));
  assert(h.old.disk.bytes(journal_path)==after_prefix);
  assert(after_cleanup.completion_proof(ack)==(completion ? 0U : expected_proof));
  assert(h.old.disk.bytes("/sigap/live-v3.acks.tail")==faulted);
  assert(!h.old.disk.exists("/sigap/live-v3.acks.recover"));
  assert(h.old.disk.active_handles==0);
  h.preserved();
}
}
