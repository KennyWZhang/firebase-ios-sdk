/*
 * Copyright 2018 Google
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Firestore/core/src/firebase/firestore/local/leveldb_transaction.h"

#include <leveldb/write_batch.h>

#include "Firestore/core/src/firebase/firestore/local/leveldb_key.h"
#include "Firestore/core/src/firebase/firestore/util/firebase_assert.h"
#include "Firestore/core/src/firebase/firestore/util/log.h"
#include "absl/memory/memory.h"

using leveldb::DB;
using leveldb::ReadOptions;
using leveldb::Slice;
using leveldb::Status;
using leveldb::WriteBatch;
using leveldb::WriteOptions;

namespace firebase {
namespace firestore {
namespace local {

LevelDbTransaction::Iterator::Iterator(LevelDbTransaction* txn)
    : db_iter_(txn->db_->NewIterator(txn->read_options_)),
      last_version_(txn->version_),
      txn_(txn),
      mutations_iter_(txn->mutations_.begin()),
      current_(),
      is_mutation_(false),
      // Iterator doesn't really point to anything yet, so is
      // invalid
      is_valid_(false) {
}

void LevelDbTransaction::Iterator::UpdateCurrent() {
  bool mutation_is_valid = mutations_iter_ != txn_->mutations_.end();
  is_valid_ = mutation_is_valid || db_iter_->Valid();

  if (is_valid_) {
    if (!mutation_is_valid) {
      is_mutation_ = false;
    } else if (!db_iter_->Valid()) {
      is_mutation_ = true;
    } else {
      // Both iterators are valid. If the leveldb key is equal to or greater
      // than the current mutation key, we are looking at a mutation next. It's
      // either sooner in the iteration or directly shadowing the underlying
      // committed value in leveldb.
      is_mutation_ = db_iter_->key().compare(mutations_iter_->first) >= 0;
    }
    if (is_mutation_) {
      current_ = *mutations_iter_;
    } else {
      current_ = {db_iter_->key().ToString(), db_iter_->value().ToString()};
    }
  }
}

void LevelDbTransaction::Iterator::Seek(const std::string& key) {
  db_iter_->Seek(key);
  FIREBASE_ASSERT_MESSAGE(db_iter_->status().ok(),
                          "leveldb iterator reported an error: %s",
                          db_iter_->status().ToString().c_str());
  for (; db_iter_->Valid() && IsDeleted(db_iter_->key()); db_iter_->Next()) {
  }
  FIREBASE_ASSERT_MESSAGE(db_iter_->status().ok(),
                          "leveldb iterator reported an error: %s",
                          db_iter_->status().ToString().c_str());
  mutations_iter_ = txn_->mutations_.lower_bound(key);
  UpdateCurrent();
  last_version_ = txn_->version_;
}

absl::string_view LevelDbTransaction::Iterator::key() {
  FIREBASE_ASSERT_MESSAGE(Valid(), "key() called on invalid iterator");
  return current_.first;
}

absl::string_view LevelDbTransaction::Iterator::value() {
  FIREBASE_ASSERT_MESSAGE(Valid(), "value() called on invalid iterator");
  return current_.second;
}

bool LevelDbTransaction::Iterator::IsDeleted(leveldb::Slice slice) {
  return txn_->deletions_.find(slice.ToString()) != txn_->deletions_.end();
}

bool LevelDbTransaction::Iterator::SyncToTransaction() {
  if (last_version_ < txn_->version_) {
    // Intentionally copying here since Seek() may update current_. We need the
    // copy to do the comparison below.
    const std::string current_key = current_.first;
    Seek(current_key);
    // If we advanced, we don't need to advance again.
    return is_valid_ && current_.first > current_key;
  } else {
    return false;
  }
}

void LevelDbTransaction::Iterator::AdvanceLDB() {
  do {
    db_iter_->Next();
  } while (db_iter_->Valid() && IsDeleted(db_iter_->key()));
  FIREBASE_ASSERT_MESSAGE(db_iter_->status().ok(),
                          "leveldb iterator reported an error: %s",
                          db_iter_->status().ToString().c_str());
}

void LevelDbTransaction::Iterator::Next() {
  FIREBASE_ASSERT_MESSAGE(Valid(), "Next() called on invalid iterator");
  bool advanced = SyncToTransaction();
  if (!advanced) {
    if (is_mutation_) {
      // A mutation might be shadowing leveldb. If so, advance both.
      if (db_iter_->Valid() && db_iter_->key() == mutations_iter_->first) {
        AdvanceLDB();
      }
      ++mutations_iter_;
    } else {
      AdvanceLDB();
    }
    UpdateCurrent();
  }
}

bool LevelDbTransaction::Iterator::Valid() {
  return is_valid_;
}

LevelDbTransaction::LevelDbTransaction(DB* db,
                                       absl::string_view label,
                                       const ReadOptions& read_options,
                                       const WriteOptions& write_options)
    : db_(db),
      mutations_(),
      deletions_(),
      read_options_(read_options),
      write_options_(write_options),
      version_(0),
      label_(std::string{label}) {
}

const ReadOptions& LevelDbTransaction::DefaultReadOptions() {
  static ReadOptions options = ([]() {
    ReadOptions read_options;
    read_options.verify_checksums = true;
    return read_options;
  })();
  return options;
}

const WriteOptions& LevelDbTransaction::DefaultWriteOptions() {
  static WriteOptions options;
  return options;
}

void LevelDbTransaction::Put(const absl::string_view& key,
                             const absl::string_view& value) {
  std::string key_string(key);
  std::string value_string(value);
  mutations_[key_string] = value_string;
  deletions_.erase(key_string);
  version_++;
}

std::unique_ptr<LevelDbTransaction::Iterator>
LevelDbTransaction::NewIterator() {
  return absl::make_unique<LevelDbTransaction::Iterator>(this);
}

Status LevelDbTransaction::Get(const absl::string_view& key,
                               std::string* value) {
  std::string key_string(key);
  if (deletions_.find(key_string) != deletions_.end()) {
    return Status::NotFound(key_string + " is not present in the transaction");
  } else {
    Mutations::iterator iter(mutations_.find(key_string));
    if (iter != mutations_.end()) {
      *value = iter->second;
      return Status::OK();
    } else {
      return db_->Get(read_options_, key_string, value);
    }
  }
}

void LevelDbTransaction::Delete(const absl::string_view& key) {
  std::string to_delete(key);
  deletions_.insert(to_delete);
  mutations_.erase(to_delete);
  version_++;
}

void LevelDbTransaction::Commit() {
  WriteBatch batch;
  for (auto it = deletions_.begin(); it != deletions_.end(); it++) {
    batch.Delete(*it);
  }

  for (auto it = mutations_.begin(); it != mutations_.end(); it++) {
    batch.Put(it->first, it->second);
  }

  if (util::LogGetLevel() <= util::kLogLevelDebug) {
    util::LogDebug("Committing transaction: %s", ToString().c_str());
  }

  Status status = db_->Write(write_options_, &batch);
  FIREBASE_ASSERT_MESSAGE(status.ok(),
                          "Failed to commit transaction:\n%s\n Failed: %s",
                          ToString().c_str(), status.ToString().c_str());
}

std::string LevelDbTransaction::ToString() {
  std::string dest("<LevelDbTransaction " + label_ + ": ");
  int64_t changes = deletions_.size() + mutations_.size();
  int64_t bytes = 0;  // accumulator for size of individual mutations.
  dest += std::to_string(changes) + " changes ";
  std::string items;  // accumulator for individual changes.
  for (auto it = deletions_.begin(); it != deletions_.end(); it++) {
    items += "\n  - Delete " + Describe(*it);
  }
  for (auto it = mutations_.begin(); it != mutations_.end(); it++) {
    int64_t change_bytes = it->second.length();
    bytes += change_bytes;
    items += "\n  - Put " + Describe(it->first) + " (" +
             std::to_string(change_bytes) + " bytes)";
  }
  dest += "(" + std::to_string(bytes) + " bytes):" + items + ">";
  return dest;
}

}  // namespace local
}  // namespace firestore
}  // namespace firebase
