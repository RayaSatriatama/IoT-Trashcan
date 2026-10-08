#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>

#include <unity.h>

#include "telemetry_archive/immutable_batch.h"
#include "telemetry_archive/batch_store.h"
#include "telemetry_archive/ack_journal_store.h"

namespace {

using namespace sigap::telemetry_archive;

sigap::acquisition::DeviceId device() {
  sigap::acquisition::DeviceId value{};
  value.present = true;
  value.bytes[0] = 0xD1U;
  return value;
}

sigap::acquisition::BootId boot() {
  sigap::acquisition::BootId value{};
  value.present = true;
  value.bytes[0] = 0xB1U;
  return value;
}

ImmutableBatch make_batch() {
  ImmutableBatch batch{};
  constexpr std::array<uint8_t, 5U> body{{'{', '1', '}', '\n', 0U}};
  (void)batch.prepare("batch-1", body.data(), 4U, 2U);
  batch.set_device_id(device());
  sigap::config::ConfigHash source_hash{};
  source_hash[0] = 0xA5U;
  (void)batch.bind_source(
      device(), boot(), 17U,
      static_cast<uint8_t>(sigap::storage::ArchiveRecordKind::Result),
      source_hash);
  return batch;
}

Ack valid_ack(const ImmutableBatch& batch) {
  Ack ack{};
  ack.batch_id_size = batch.batch_id_size;
  for (std::size_t index = 0U; index < ack.batch_id_size; ++index) {
    ack.batch_id[index] = batch.batch_id[index];
  }
  ack.body_hash = batch.body_hash;
  ack.record_count = batch.record_count;
  ack.device_id = batch.device_id;
  ack.stored_count = batch.record_count;
  ack.status = AckStatus::Durable;
  return ack;
}

void test_batch_bytes_and_hash_are_immutable_after_prepare() {
  auto batch = make_batch();
  TEST_ASSERT_TRUE(batch.attempted);
  const auto hash = batch.body_hash;
  TEST_ASSERT_FALSE(batch.prepare("batch-2", reinterpret_cast<const uint8_t*>("x"),
                                  1U, 1U));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(hash.data(), batch.body_hash.data(), hash.size());
  TEST_ASSERT_FALSE(batch.bind_source(
      device(), boot(), 18U,
      static_cast<uint8_t>(sigap::storage::ArchiveRecordKind::Result), hash));
  TEST_ASSERT_EQUAL_UINT32(7U, batch.id().size());
  TEST_ASSERT_EQUAL_INT(0, std::memcmp(batch.id().data(), "batch-1", 7U));
}

void test_batch_store_rejects_body_mutation_with_stale_hash() {
  auto batch = make_batch();
  batch.body[0U] ^= 0x01U;
  MemoryImmutableBatchStore<2U> store(64U * 1024U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchStoreStatus::Invalid),
                        static_cast<int>(store.append(batch)));
  TEST_ASSERT_EQUAL_UINT32(0U, store.pending_bytes());
}

void test_batch_prepare_rejects_record_count_above_contract_limit() {
  ImmutableBatch batch{};
  constexpr std::array<uint8_t, 1U> body{{0x7FU}};
  TEST_ASSERT_FALSE(batch.prepare("too-many", body.data(), body.size(),
                                  static_cast<uint16_t>(kMaxBatchRecords + 1U)));
  TEST_ASSERT_FALSE(batch.attempted);
}

void test_only_valid_ack_advances_and_checkpoint_allows_reclaim() {
  auto batch = make_batch();
  AckJournal<2U> journal{};
  TEST_ASSERT_TRUE(journal.append(batch));
  auto invalid = valid_ack(batch);
  invalid.status = AckStatus::SummaryV2;
  TEST_ASSERT_FALSE(journal.apply_ack(invalid));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(JournalState::Pending),
                        static_cast<int>(journal.at(0U)->state));
  auto ack = valid_ack(batch);
  TEST_ASSERT_TRUE(journal.apply_ack(ack));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(JournalState::ServerAcked),
                        static_cast<int>(journal.at(0U)->state));
  TEST_ASSERT_EQUAL_UINT32(0U, journal.reclaim_checkpointed());
  TEST_ASSERT_TRUE(journal.checkpoint());
  TEST_ASSERT_EQUAL_UINT32(1U, journal.reclaim_checkpointed());
}

void test_ack_conflict_timeout_and_hash_mismatch_do_not_advance() {
  auto batch = make_batch();
  AckJournal<2U> journal{};
  TEST_ASSERT_TRUE(journal.append(batch));
  auto ack = valid_ack(batch);
  ack.body_hash[0] ^= 0x01U;
  TEST_ASSERT_FALSE(journal.apply_ack(ack));
  ack.body_hash = batch.body_hash;
  ack.status = AckStatus::Conflict;
  TEST_ASSERT_FALSE(journal.apply_ack(ack));
  ack.status = AckStatus::Timeout;
  TEST_ASSERT_FALSE(journal.apply_ack(ack));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(JournalState::Pending),
                        static_cast<int>(journal.at(0U)->state));
}

void test_ack_count_mismatch_does_not_advance_delivery_state() {
  auto batch = make_batch();
  MemoryAckJournalStore<2U> journal{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.append_pending(batch)));
  auto ack = valid_ack(batch);
  ack.stored_count = static_cast<uint16_t>(batch.record_count - 1U);
  ack.deduplicated_count = 0U;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Invalid),
                        static_cast<int>(journal.apply_ack(ack)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(JournalState::Pending),
                        static_cast<int>(journal.at(0U)->state));
}

void test_batch_bytes_are_durable_and_duplicate_identity_is_not_rewritten() {
  std::printf("memory ImmutableBatch=%zu AckJournal2=%zu BatchStore2=%zu FsBatchStore=%zu\n",
              sizeof(ImmutableBatch), sizeof(AckJournal<2U>),
              sizeof(MemoryImmutableBatchStore<2U>),
              sizeof(FsImmutableBatchStore));
  auto batch = make_batch();
  MemoryImmutableBatchStore<2U> store(64U * 1024U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchStoreStatus::Durable),
                        static_cast<int>(store.append(batch)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(BatchStoreStatus::Durable),
                        static_cast<int>(store.append(batch)));
  TEST_ASSERT_EQUAL_UINT32(1U, store.size());
  TEST_ASSERT_EQUAL_UINT32(batch.body_size, store.pending_bytes());
  TEST_ASSERT_EQUAL_INT(0, std::memcmp(store.at(0U)->exact_body().data(),
                                       batch.exact_body().data(),
                                       batch.body_size));
  ImmutableBatch replay{};
  TEST_ASSERT_TRUE(store.load(0U, replay));
  TEST_ASSERT_EQUAL_UINT32(batch.record_count, replay.record_count);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(batch.device_id.bytes.data(),
                                replay.device_id.bytes.data(),
                                batch.device_id.bytes.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(batch.body_hash.data(), replay.body_hash.data(),
                                batch.body_hash.size());
  TEST_ASSERT_TRUE(replay.source.present);
  TEST_ASSERT_EQUAL_UINT64(17U, replay.source.sequence);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(batch.source.record_hash.data(),
                                replay.source.record_hash.data(),
                                batch.source.record_hash.size());
}

void test_persistent_ack_boundary_requires_checkpoint_before_reclaim() {
  auto batch = make_batch();
  MemoryAckJournalStore<2U> journal{};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.append_pending(batch)));
  auto ack = valid_ack(batch);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(AckPersistStatus::Durable),
                        static_cast<int>(journal.apply_ack(ack)));
  TEST_ASSERT_EQUAL_UINT32(0U, journal.reclaim());
  journal.set_checkpoint_failure(true);
  TEST_ASSERT_FALSE(journal.checkpoint());
  TEST_ASSERT_EQUAL_UINT32(0U, journal.reclaim());
  journal.set_checkpoint_failure(false);
  TEST_ASSERT_TRUE(journal.checkpoint());
  TEST_ASSERT_EQUAL_UINT32(1U, journal.reclaim());
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_batch_bytes_and_hash_are_immutable_after_prepare);
  RUN_TEST(test_batch_store_rejects_body_mutation_with_stale_hash);
  RUN_TEST(test_batch_prepare_rejects_record_count_above_contract_limit);
  RUN_TEST(test_only_valid_ack_advances_and_checkpoint_allows_reclaim);
  RUN_TEST(test_ack_conflict_timeout_and_hash_mismatch_do_not_advance);
  RUN_TEST(test_ack_count_mismatch_does_not_advance_delivery_state);
  RUN_TEST(test_batch_bytes_are_durable_and_duplicate_identity_is_not_rewritten);
  RUN_TEST(test_persistent_ack_boundary_requires_checkpoint_before_reclaim);
  return UNITY_END();
}
