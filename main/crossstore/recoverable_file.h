#pragma once

#include <string>

namespace crossstore {

// The .previous sibling is the recovery record. Before rename(new, current),
// a failed write/reset can only leave the old file at current or .previous.
// All callers supply bounded, validated paths and close/flush newFile first.
template <typename StorageType>
bool recoverFile(StorageType& storage, const std::string& current) {
  const std::string previous = current + ".previous";
  if (!storage.exists(previous.c_str())) return true;
  if (storage.exists(current.c_str())) return storage.remove(previous.c_str());
  return storage.rename(previous.c_str(), current.c_str());
}

template <typename StorageType>
bool commitFile(StorageType& storage, const char* newFile, const std::string& current) {
  if (!storage.exists(newFile) || !recoverFile(storage, current)) return false;
  const std::string previous = current + ".previous";
  const bool existed = storage.exists(current.c_str());
  if (existed && !storage.rename(current.c_str(), previous.c_str())) return false;
  if (!storage.rename(newFile, current.c_str())) {
    if (existed) storage.rename(previous.c_str(), current.c_str());
    return false;
  }
  // Cleanup may fail without losing either version. recoverFile retries later.
  if (existed) storage.remove(previous.c_str());
  return true;
}
}  // namespace crossstore
