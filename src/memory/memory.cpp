#include <agentos/memory/memory.hpp>

#ifndef AGENTOS_NO_DUCKDB
#include <agentos/memory/duckdb_store.hpp>
#endif

#ifndef AGENTOS_NO_SQLITE
#include <agentos/memory/sqlite_store.hpp>
#endif

namespace agentos::memory {

MemorySystem::MemorySystem(fs::path ltm_dir, LTMBackend backend)
    : working_(std::make_unique<WorkingMemory>(32)),
      short_term_(std::make_unique<ShortTermMemory>(512)),
      graph_(std::make_unique<LocalGraphMemory>(ltm_dir.empty() ? fs::temp_directory_path() / "agentos_ltm" : ltm_dir)) {
  // LOW PRIORITY: Use temp directory if ltm_dir is empty
  if (ltm_dir.empty()) {
    ltm_dir = fs::temp_directory_path() / "agentos_ltm";
  }
  // 根据后端类型创建 LTM（都实现 IMemoryStore 接口，可热切换）
#ifndef AGENTOS_NO_DUCKDB
  if (backend == LTMBackend::DuckDB) {
    long_term_ = std::make_unique<DuckDBLongTermMemory>(std::move(ltm_dir));
  } else
#endif
#ifndef AGENTOS_NO_SQLITE
  if (backend == LTMBackend::SQLite) {
    long_term_ = std::make_unique<SQLiteLongTermMemory>(std::move(ltm_dir));
  } else
#endif
  {
    (void)backend;  // suppress unused warning when both disabled
    long_term_ = std::make_unique<LongTermMemory>(std::move(ltm_dir));
  }
}

} // namespace agentos::memory
