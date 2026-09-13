#include <endian.h>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "gflags/gflags.h"
#include "proto/certain.pb.h"
#include "rocksdb/db.h"

DEFINE_string(db_path, "certain/build/experiment_artifacts/node0/test_plog.o",
              "Path to RocksDB plog directory");

class __attribute__((packed)) EntryKey {
 public:
  uint64_t entity_id;
  uint64_t entry;
  uint64_t value_id;
};

void PrintHex(const std::string& str) {
  for (unsigned char c : str) {
    std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)c << " ";
  }
  std::cout << std::dec << "\n";
}

int main(int argc, char* argv[]) {
  google::ParseCommandLineFlags(&argc, &argv, true);

  rocksdb::Options options;
  options.create_if_missing = false;
  rocksdb::DB* db = nullptr;
  rocksdb::Status status = rocksdb::DB::OpenForReadOnly(options, FLAGS_db_path, &db);
  if (!status.ok()) {
    std::cerr << "Failed to open RocksDB at " << FLAGS_db_path << ": "
              << status.ToString() << std::endl;
    return 1;
  }
  std::unique_ptr<rocksdb::DB> db_ptr(db);

  std::cout << "=================================================================\n";
  std::cout << "  Deep Dive: Inspecting RocksDB Plog Binary Records at:\n";
  std::cout << "  " << FLAGS_db_path << "\n";
  std::cout << "=================================================================\n\n";

  rocksdb::ReadOptions read_options;
  std::unique_ptr<rocksdb::Iterator> iter(db->NewIterator(read_options));
  int count = 0;

  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    count++;
    std::string key_str = iter->key().ToString();
    std::string val_str = iter->value().ToString();

    std::cout << "-----------------------------------------------------------------\n";
    std::cout << "[Record #" << count << "]\n";
    std::cout << "Raw Key (" << key_str.size() << " bytes, Hex): ";
    PrintHex(key_str);

    if (key_str.size() == sizeof(EntryKey)) {
      const EntryKey* raw_key = reinterpret_cast<const EntryKey*>(key_str.data());
      uint64_t entity_id = be64toh(raw_key->entity_id);
      uint64_t entry = be64toh(raw_key->entry);
      uint64_t value_id = be64toh(raw_key->value_id);

      std::cout << "Decoded Key: entity_id=" << entity_id
                << " | entry=" << entry
                << " | value_id=" << value_id << "\n";

      if (value_id == 0) {
        std::cout << "Type: [EntryRecord Metadata (Proto)]\n";
        certain::EntryRecord record;
        if (record.ParseFromString(val_str)) {
          std::cout << "  - prepared_num: " << record.prepared_num() << "\n";
          std::cout << "  - promised_num: " << record.promised_num() << "\n";
          std::cout << "  - accepted_num: " << record.accepted_num() << "\n";
          std::cout << "  - value_id:     " << record.value_id() << "\n";
          std::cout << "  - chosen:       " << (record.chosen() ? "TRUE" : "FALSE") << "\n";
          std::cout << "  - has_vid_only: " << (record.has_value_id_only() ? "TRUE" : "FALSE") << "\n";
          std::cout << "  - value bytes:  \"" << record.value() << "\" ("
                    << record.value().size() << " bytes)\n";
          std::cout << "  - uuids (" << record.uuids_size() << "): [";
          for (int i = 0; i < record.uuids_size(); ++i) {
            std::cout << record.uuids(i) << (i + 1 < record.uuids_size() ? ", " : "");
          }
          std::cout << "]\n";
        } else {
          std::cout << "  (Failed to parse Protobuf EntryRecord)\n";
        }
      } else {
        std::cout << "Type: [Standalone Value Payload (value_id=" << value_id << ")]\n";
        std::cout << "  - Payload Content: \"" << val_str << "\" ("
                  << val_str.size() << " bytes)\n";
      }
    } else {
      std::cout << "(Key size does not match 24-byte EntryKey)\n";
    }
  }

  std::cout << "-----------------------------------------------------------------\n";
  std::cout << "Total Records Inspected: " << count << "\n";
  std::cout << "=================================================================\n";

  return 0;
}
