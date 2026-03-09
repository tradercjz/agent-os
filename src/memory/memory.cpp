#include <agentos/memory/memory.hpp>
#include <agentos/memory/duckdb_store.hpp>
#include <agentos/memory/sqlite_store.hpp>

namespace agentos::memory {

MemorySystem::MemorySystem(fs::path ltm_dir, LTMBackend backend)
    : working_(std::make_unique<WorkingMemory>(32)),
      short_term_(std::make_unique<ShortTermMemory>(512)),
      graph_(std::make_unique<LocalGraphMemory>(ltm_dir)) {
  // 根据后端类型创建 LTM（都实现 IMemoryStore 接口，可热切换）
  if (backend == LTMBackend::DuckDB) {
    long_term_ = std::make_unique<DuckDBLongTermMemory>(std::move(ltm_dir));
  } else if (backend == LTMBackend::SQLite) {
    long_term_ = std::make_unique<SQLiteLongTermMemory>(std::move(ltm_dir));
  } else {
    long_term_ = std::make_unique<LongTermMemory>(std::move(ltm_dir));
  }
}

} // namespace agentos::memory
