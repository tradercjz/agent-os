#pragma once

#include <string>
#include <stdexcept>
#include <Exceptions.h>  // DolphinDB exception classes

// 统一异常转换宏：捕获所有非 DolphinDB 标准异常，转为 RuntimeException
#define DDB_SAFE_BEGIN  try {
#define DDB_SAFE_END(func_name) \
    } catch (const IllegalArgumentException&) { throw; } \
      catch (const RuntimeException&) { throw; } \
      catch (const std::exception& e) { \
          throw RuntimeException(std::string(func_name " error: ") + e.what()); \
      } catch (...) { \
          throw RuntimeException(func_name ": unknown internal error"); \
      }
