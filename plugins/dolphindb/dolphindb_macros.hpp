#pragma once

#include <string>
#include <stdexcept>

// Forward declare DolphinDB exception classes so the macro compiles
// without needing the full DolphinDB SDK headers.
namespace ddb {
    class IllegalArgumentException;
    class RuntimeException;
}

// 统一异常转换宏：捕获所有非 DolphinDB 标准异常，转为 ddb::RuntimeException
#define DDB_SAFE_BEGIN  try {
#define DDB_SAFE_END(func_name) \
    } catch (const ddb::IllegalArgumentException&) { throw; } \
      catch (const ddb::RuntimeException&) { throw; } \
      catch (const std::exception& e) { \
          throw ddb::RuntimeException(std::string(func_name " error: ") + e.what()); \
      } catch (...) { \
          throw ddb::RuntimeException(func_name ": unknown internal error"); \
      }
