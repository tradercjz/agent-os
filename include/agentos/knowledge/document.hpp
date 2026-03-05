#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos::knowledge {

/**
 * @brief Represents a smaller semantic block partitioned from a Document.
 * Used for granular retrieval in the BM25 and HNSW index.
 */
struct Chunk {
  std::string id;          // Unique chunk ID, usually "docId_chunkIndex"
  std::string document_id; // Reference to parent Document
  std::string content;     // The actual text payload of the chunk

  // Optional metadata specific to this chunk (e.g., "start_line", "end_line")
  std::unordered_map<std::string, std::string> metadata;

  // We do not store vectors here. Vectors go directly to HNSW memory tier.
};

/**
 * @brief Represents a full File or generic knowledge entry ready for ingestion.
 */
struct Document {
  std::string id; // Unique identifier (typically the absolute file path or URI)
  std::string title;     // Display title
  std::string content;   // Full raw text content of the document
  uint64_t content_hash; // Hash of the content. Used for
                         // differential/incremental syncing.

  // The partitions/chunks belonging to this document
  std::vector<Chunk> chunks;

  // Document global metadata (e.g., "author", "created_at", "source_type")
  std::unordered_map<std::string, std::string> metadata;

  /**
   * @brief Utility to compute the hash of the current content.
   */
  void compute_hash() { content_hash = std::hash<std::string>{}(content); }
};

} // namespace agentos::knowledge
