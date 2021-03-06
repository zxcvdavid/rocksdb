// Copyright (c) 2014, Facebook, Inc. All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#ifndef GFLAGS
#include <cstdio>
int main() {
  fprintf(stderr, "Please install gflags to run this test\n");
  return 1;
}
#else

#include <gflags/gflags.h>
#include <vector>
#include <string>
#include <map>

#include "table/meta_blocks.h"
#include "table/cuckoo_table_builder.h"
#include "table/cuckoo_table_reader.h"
#include "table/cuckoo_table_factory.h"
#include "util/random.h"
#include "util/testharness.h"
#include "util/testutil.h"

using GFLAGS::ParseCommandLineFlags;
using GFLAGS::SetUsageMessage;

DEFINE_string(file_dir, "", "Directory where the files will be created"
    " for benchmark. Added for using tmpfs.");
DEFINE_bool(enable_perf, false, "Run Benchmark Tests too.");

namespace rocksdb {

extern const uint64_t kCuckooTableMagicNumber;

namespace {
const uint32_t kNumHashFunc = 10;
// Methods, variables related to Hash functions.
std::unordered_map<std::string, std::vector<uint64_t>> hash_map;

void AddHashLookups(const std::string& s, uint64_t bucket_id,
        uint32_t num_hash_fun) {
  std::vector<uint64_t> v;
  for (uint32_t i = 0; i < num_hash_fun; i++) {
    v.push_back(bucket_id + i);
  }
  hash_map[s] = v;
}

uint64_t GetSliceHash(const Slice& s, uint32_t index,
    uint64_t max_num_buckets) {
  return hash_map[s.ToString()][index];
}

// Methods, variables for checking key and values read.
struct ValuesToAssert {
  ValuesToAssert(const std::string& key, const Slice& value)
    : expected_user_key(key),
      expected_value(value),
      call_count(0) {}
  std::string expected_user_key;
  Slice expected_value;
  int call_count;
};

bool AssertValues(void* assert_obj,
    const ParsedInternalKey& k, const Slice& v) {
  ValuesToAssert *ptr = reinterpret_cast<ValuesToAssert*>(assert_obj);
  ASSERT_EQ(ptr->expected_value.ToString(), v.ToString());
  ASSERT_EQ(ptr->expected_user_key, k.user_key.ToString());
  ++ptr->call_count;
  return false;
}
}  // namespace

class CuckooReaderTest {
 public:
  CuckooReaderTest() {
    options.allow_mmap_reads = true;
    env = options.env;
    env_options = EnvOptions(options);
  }

  void SetUp(int num_items) {
    this->num_items = num_items;
    hash_map.clear();
    keys.clear();
    keys.resize(num_items);
    user_keys.clear();
    user_keys.resize(num_items);
    values.clear();
    values.resize(num_items);
  }

  void CreateCuckooFile(bool is_last_level) {
    unique_ptr<WritableFile> writable_file;
    ASSERT_OK(env->NewWritableFile(fname, &writable_file, env_options));
    CuckooTableBuilder builder(
        writable_file.get(), keys[0].size(), values[0].size(), 0.9,
        10000, kNumHashFunc, 100, is_last_level, GetSliceHash);
    ASSERT_OK(builder.status());
    for (uint32_t key_idx = 0; key_idx < num_items; ++key_idx) {
      builder.Add(Slice(keys[key_idx]), Slice(values[key_idx]));
      ASSERT_EQ(builder.NumEntries(), key_idx + 1);
      ASSERT_OK(builder.status());
    }
    ASSERT_OK(builder.Finish());
    ASSERT_EQ(num_items, builder.NumEntries());
    file_size = builder.FileSize();
    ASSERT_OK(writable_file->Close());
  }

  void CheckReader() {
    unique_ptr<RandomAccessFile> read_file;
    ASSERT_OK(env->NewRandomAccessFile(fname, &read_file, env_options));
    CuckooTableReader reader(
        options,
        std::move(read_file),
        file_size,
        GetSliceHash);
    ASSERT_OK(reader.status());

    for (uint32_t i = 0; i < num_items; ++i) {
      ValuesToAssert v(user_keys[i], values[i]);
      ASSERT_OK(reader.Get(
            ReadOptions(), Slice(keys[i]), &v, AssertValues, nullptr));
      ASSERT_EQ(1, v.call_count);
    }
  }

  std::vector<std::string> keys;
  std::vector<std::string> user_keys;
  std::vector<std::string> values;
  uint32_t num_items;
  std::string fname;
  uint64_t file_size;
  Options options;
  Env* env;
  EnvOptions env_options;
};

TEST(CuckooReaderTest, WhenKeyExists) {
  SetUp(10);
  fname = test::TmpDir() + "/CuckooReader_WhenKeyExists";
  for (uint32_t i = 0; i < num_items; i++) {
    user_keys[i] = "keys" + std::to_string(i+100);
    ParsedInternalKey ikey(user_keys[i], i + 1000, kTypeValue);
    AppendInternalKey(&keys[i], ikey);
    values[i] = "value" + std::to_string(i+100);
    AddHashLookups(user_keys[i], i * kNumHashFunc, kNumHashFunc);
  }
  CreateCuckooFile(false);
  CheckReader();
  // Last level file.
  CreateCuckooFile(true);
  CheckReader();
  // Test with collision. Make all hash values collide.
  hash_map.clear();
  for (uint32_t i = 0; i < num_items; i++) {
    AddHashLookups(user_keys[i], 0, kNumHashFunc);
  }
  CreateCuckooFile(false);
  CheckReader();
  // Last level file.
  CreateCuckooFile(true);
  CheckReader();
}

TEST(CuckooReaderTest, WhenKeyNotFound) {
  // Add keys with colliding hash values.
  SetUp(kNumHashFunc / 2);
  fname = test::TmpDir() + "/CuckooReader_WhenKeyNotFound";
  for (uint32_t i = 0; i < num_items; i++) {
    user_keys[i] = "keys" + std::to_string(i+100);
    ParsedInternalKey ikey(user_keys[i], i + 1000, kTypeValue);
    AppendInternalKey(&keys[i], ikey);
    values[i] = "value" + std::to_string(i+100);
    // Make all hash values collide.
    AddHashLookups(user_keys[i], 0, kNumHashFunc);
  }
  CreateCuckooFile(false);
  CheckReader();
  unique_ptr<RandomAccessFile> read_file;
  ASSERT_OK(env->NewRandomAccessFile(fname, &read_file, env_options));
  CuckooTableReader reader(
      options,
      std::move(read_file),
      file_size,
      GetSliceHash);
  ASSERT_OK(reader.status());
  // Search for a key with colliding hash values.
  std::string not_found_user_key = "keys" + std::to_string(num_items + 100);
  std::string not_found_key;
  AddHashLookups(not_found_user_key, 0, kNumHashFunc);
  ParsedInternalKey ikey(not_found_user_key, 1000, kTypeValue);
  AppendInternalKey(&not_found_key, ikey);
  ValuesToAssert v("", "");
  ASSERT_OK(reader.Get(
        ReadOptions(), Slice(not_found_key), &v, AssertValues, nullptr));
  ASSERT_EQ(0, v.call_count);
  ASSERT_OK(reader.status());
  // Search for a key with an independent hash value.
  std::string not_found_user_key2 = "keys" + std::to_string(num_items + 101);
  std::string not_found_key2;
  AddHashLookups(not_found_user_key2, kNumHashFunc, kNumHashFunc);
  ParsedInternalKey ikey2(not_found_user_key2, 1000, kTypeValue);
  AppendInternalKey(&not_found_key2, ikey2);
  ASSERT_OK(reader.Get(
        ReadOptions(), Slice(not_found_key2), &v, AssertValues, nullptr));
  ASSERT_EQ(0, v.call_count);
  ASSERT_OK(reader.status());

  // Test read with corrupted key.
  not_found_key2.pop_back();
  ASSERT_TRUE(!ParseInternalKey(not_found_key2, &ikey));
  ASSERT_TRUE(reader.Get(
        ReadOptions(), Slice(not_found_key2), &v,
        AssertValues, nullptr).IsCorruption());
  ASSERT_EQ(0, v.call_count);
  ASSERT_OK(reader.status());

  // Test read when key is unused key.
  std::string unused_user_key = "keys10:";
  // Add hash values that map to empty buckets.
  AddHashLookups(unused_user_key, kNumHashFunc, kNumHashFunc);
  std::string unused_key;
  ParsedInternalKey ikey3(unused_user_key, 1000, kTypeValue);
  AppendInternalKey(&unused_key, ikey3);
  ASSERT_OK(reader.Get(
        ReadOptions(), Slice(unused_key), &v, AssertValues, nullptr));
  ASSERT_EQ(0, v.call_count);
  ASSERT_OK(reader.status());
}

// Performance tests
namespace {
bool DoNothing(void* arg, const ParsedInternalKey& k, const Slice& v) {
  // Deliberately empty.
  return false;
}

bool CheckValue(void* cnt_ptr, const ParsedInternalKey& k, const Slice& v) {
  ++*reinterpret_cast<int*>(cnt_ptr);
  std::string expected_value;
  AppendInternalKey(&expected_value, k);
  ASSERT_EQ(0, v.compare(Slice(&expected_value[0], v.size())));
  return false;
}

// Create last level file as we are interested in measuring performance of
// last level file only.
void BM_CuckooRead(uint64_t num, uint32_t key_length,
    uint32_t value_length, uint64_t num_reads, double hash_ratio) {
  assert(value_length <= key_length);
  assert(8 <= key_length);
  std::vector<std::string> keys;
  Options options;
  options.allow_mmap_reads = true;
  Env* env = options.env;
  EnvOptions env_options = EnvOptions(options);
  uint64_t file_size;
  if (FLAGS_file_dir.empty()) {
    FLAGS_file_dir = test::TmpDir();
  }
  std::string fname = FLAGS_file_dir + "/cuckoo_read_benchmark";

  uint64_t predicted_file_size =
    num * (key_length + value_length) / hash_ratio + 1024;

  unique_ptr<WritableFile> writable_file;
  ASSERT_OK(env->NewWritableFile(fname, &writable_file, env_options));
  CuckooTableBuilder builder(
      writable_file.get(), key_length + 8, value_length, hash_ratio,
      predicted_file_size, kMaxNumHashTable, 1000, true, GetSliceMurmurHash);
  ASSERT_OK(builder.status());
  for (uint64_t key_idx = 0; key_idx < num; ++key_idx) {
    // Value is just a part of key.
    std::string new_key(reinterpret_cast<char*>(&key_idx), sizeof(key_idx));
    new_key = std::string(key_length - new_key.size(), 'k') + new_key;
    ParsedInternalKey ikey(new_key, num, kTypeValue);
    std::string full_key;
    AppendInternalKey(&full_key, ikey);
    builder.Add(Slice(full_key), Slice(&full_key[0], value_length));
    ASSERT_EQ(builder.NumEntries(), key_idx + 1);
    ASSERT_OK(builder.status());
    keys.push_back(full_key);
  }
  ASSERT_OK(builder.Finish());
  ASSERT_EQ(num, builder.NumEntries());
  file_size = builder.FileSize();
  ASSERT_OK(writable_file->Close());
  unique_ptr<RandomAccessFile> read_file;
  ASSERT_OK(env->NewRandomAccessFile(fname, &read_file, env_options));

  CuckooTableReader reader(
      options,
      std::move(read_file),
      file_size,
      GetSliceMurmurHash);
  ASSERT_OK(reader.status());
  const UserCollectedProperties user_props =
    reader.GetTableProperties()->user_collected_properties;
  const uint32_t num_hash_fun = *reinterpret_cast<const uint32_t*>(
      user_props.at(CuckooTablePropertyNames::kNumHashTable).data());
  fprintf(stderr, "With %lu items and hash table ratio %f, number of hash"
      " functions used: %u.\n", num, hash_ratio, num_hash_fun);
  ReadOptions r_options;
  for (auto& key : keys) {
    int cnt = 0;
    ASSERT_OK(reader.Get(r_options, Slice(key), &cnt, CheckValue, nullptr));
    ASSERT_EQ(1, cnt);
  }
  // Shuffle Keys.
  std::random_shuffle(keys.begin(), keys.end());

  uint64_t time_now = env->NowMicros();
  for (uint64_t i = 0; i < num_reads; ++i) {
    reader.Get(r_options, Slice(keys[i % num]), nullptr, DoNothing, nullptr);
  }
  fprintf(stderr, "Time taken per op is %.3fus\n",
      (env->NowMicros() - time_now)*1.0/num_reads);
}
}  // namespace.

TEST(CuckooReaderTest, Performance) {
  // In all these tests, num_reads = 10*num_items.
  if (!FLAGS_enable_perf) {
    return;
  }
  BM_CuckooRead(100000, 8, 4, 1000000, 0.9);
  BM_CuckooRead(1000000, 8, 4, 10000000, 0.9);
  BM_CuckooRead(1000000, 8, 4, 10000000, 0.7);
  BM_CuckooRead(10000000, 8, 4, 100000000, 0.9);
  BM_CuckooRead(10000000, 8, 4, 100000000, 0.7);
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ParseCommandLineFlags(&argc, &argv, true);
  rocksdb::test::RunAllTests();
  return 0;
}

#endif  // GFLAGS.
