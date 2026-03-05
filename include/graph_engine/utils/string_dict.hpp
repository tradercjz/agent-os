#pragma once

#include "graph_engine/core/types.hpp"
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace graph_engine {
namespace utils {

// StringDict is a bidirectional dictionary used during offline graph building
// It maps strings (e.g. Entity identifiers "Apple", "Person") into compact
// integer IDs It also provides binary serialization capable of being dumped and
// reloaded.
class StringDict {
public:
  StringDict() = default;

  // Insert a string if it doesn't exist, return its ID.
  // If it exists, return the existing ID.
  EntityID get_or_insert(const std::string &key) {
    auto it = str_to_id_.find(key);
    if (it != str_to_id_.end()) {
      return it->second;
    }

    EntityID new_id = static_cast<EntityID>(id_to_str_.size());
    str_to_id_[key] = new_id;
    id_to_str_.push_back(key);
    return new_id;
  }

  // Get string by ID. Throws if out of bounds.
  const std::string &get_string(EntityID id) const {
    if (id >= id_to_str_.size()) {
      throw std::out_of_range("StringDict ID out of range");
    }
    return id_to_str_[id];
  }

  // Find ID by string. Returns true if found, false otherwise.
  bool find_id(const std::string &key, EntityID &out_id) const {
    auto it = str_to_id_.find(key);
    if (it != str_to_id_.end()) {
      out_id = it->second;
      return true;
    }
    return false;
  }

  size_t size() const { return id_to_str_.size(); }

  // Serialize dictionary to binary format for fast loading
  bool save(const std::string &path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out)
      return false;

    // Write number of entries
    uint64_t count = id_to_str_.size();
    out.write(reinterpret_cast<const char *>(&count), sizeof(count));

    // Write all strings consecutively
    for (const auto &str : id_to_str_) {
      uint32_t len = static_cast<uint32_t>(str.size());
      out.write(reinterpret_cast<const char *>(&len), sizeof(len));
      out.write(str.data(), len);
    }

    return out.good();
  }

  // Load from binary format rebuilds both internal structures
  bool load(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
      return false;

    str_to_id_.clear();
    id_to_str_.clear();

    uint64_t count = 0;
    if (!in.read(reinterpret_cast<char *>(&count), sizeof(count))) {
      return false; // possibly empty dict
    }

    id_to_str_.reserve(count);
    str_to_id_.reserve(count);

    for (uint64_t i = 0; i < count; ++i) {
      uint32_t len = 0;
      if (!in.read(reinterpret_cast<char *>(&len), sizeof(len)))
        return false;

      std::string str(len, '\0');
      if (!in.read(&str[0], len))
        return false;

      id_to_str_.push_back(str);
      str_to_id_[str] = static_cast<EntityID>(i);
    }

    return in.good() || in.eof();
  }

private:
  std::vector<std::string> id_to_str_;
  std::unordered_map<std::string, EntityID> str_to_id_;
};

} // namespace utils
} // namespace graph_engine
