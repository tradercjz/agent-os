# DolphinDB 插件编译部署指南

## 概述

本文档记录了 AgentOS DolphinDB 插件的完整编译部署过程，以及遇到的问题和解决方案。

## 🚀 快速开始

### 环境要求

- **操作系统**: Rocky Linux 8/9 或 macOS
- **编译器**: GCC 14.2.1 (推荐) 或 Clang
- **CMake**: 3.13+
- **DolphinDB**: 3.00.5
- **AgentOS**: 当前版本

### 编译部署步骤

```bash
# 1. 克隆项目
git clone https://github.com/tradercjz/agent-os.git
cd agent-os

# 2. 配置构建（强制使用旧 ABI 兼容 DolphinDB）
cmake -B build -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_CXX_FLAGS="-D_GLIBCXX_USE_CXX11_ABI=0"

# 3. 编译插件
cmake --build build --target PluginAgentOS -- -j$(nproc)

# 4. 部署插件
cp build/plugins/libPluginAgentOS.so ~/3.00.5/server/plugins/AgentOS/
cp plugins/dolphindb/PluginAgentOS.txt ~/3.00.5/server/plugins/AgentOS/

# 5. 验证依赖
ldd ~/3.00.5/server/plugins/AgentOS/libPluginAgentOS.so | grep duckdb
```

## 🔧 常见问题及解决方案

### 1. C++ ABI 不匹配问题

#### 问题现象

```dolphindb
loadPlugin(getHomeDir() + "/plugins/AgentOS/PluginAgentOS.txt")
// 错误: undefined symbol: _ZN6duckdb6DuckDBC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_8DBConfigE
```

#### 问题原因

- **新 ABI**: `std::__cxx11::string` (GCC 5.0+ 默认)
- **旧 ABI**: `std::string` (GCC 4.x 及之前)
- **冲突**: AgentOS 主库使用新 ABI，DuckDB 库使用旧 ABI

#### 解决方案

**方法一：全局强制旧 ABI（推荐）**
```bash
cmake -B build -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_CXX_FLAGS="-D_GLIBCXX_USE_CXX11_ABI=0"
```

**方法二：仅插件使用旧 ABI**
```cmake
# plugins/dolphindb/CMakeLists.txt
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_definitions(PluginAgentOS PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
    target_compile_options(PluginAgentOS PRIVATE -D_GLIBCXX_USE_CXX11_ABI=0)
endif()
```

### 2. DuckDB 库版本兼容性

#### 问题现象

```bash
# 插件需要新 ABI 符号
nm -D libPluginAgentOS.so | grep DuckDB
U _ZN6duckdb6DuckDBC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_8DBConfigE

# 但 DuckDB 库只提供旧 ABI 符号
objdump -T /usr/local/lib/libduckdb.so | grep DuckDB
_ZN6duckdb6DuckDBC1ERKSsPNS_8DBConfigE
```

#### 解决方案

**下载兼容的 DuckDB 版本**:
```bash
# 下载 v1.1.3 预编译版本（支持旧 ABI）
cd /tmp
wget https://github.com/duckdb/duckdb/releases/download/v1.1.3/libduckdb-linux-amd64.zip
unzip libduckdb-linux-amd64.zip

# 替换系统库
sudo cp libduckdb.so /usr/local/lib/
sudo cp duckdb.hpp /usr/local/include/
```

### 3. DolphinDB 异常类命名空间

#### 问题现象

```cpp
error: invalid use of incomplete type 'const class ddb::IllegalArgumentException'
error: invalid use of incomplete type 'const class ddb::RuntimeException'
```

#### 问题原因

DolphinDB 异常类不在 `ddb` 命名空间中，它们是全局的。

#### 解决方案

**修复宏定义** (`plugins/dolphindb/dolphindb_macros.hpp`):
```cpp
// 错误的写法
namespace ddb {
    class IllegalArgumentException;
    class RuntimeException;
}
#define DDB_SAFE_END(func_name) \
    } catch (const ddb::IllegalArgumentException&) { throw; } \
      catch (const ddb::RuntimeException&) { throw; } \

// 正确的写法
#include <Exceptions.h>  // DolphinDB exception classes
#define DDB_SAFE_END(func_name) \
    } catch (const IllegalArgumentException&) { throw; } \
      catch (const RuntimeException&) { throw; } \
```

### 4. Vector::appendString 调用错误

#### 问题现象

```cpp
error: no matching function for call to 'ddb::Vector::appendString(char*, int)'
```

#### 问题原因

DolphinDB 的 `appendString` 方法需要特定的参数类型。

#### 解决方案

```cpp
// 错误的写法
col_sid->appendString(const_cast<char*>(session_ids[i].c_str()), 1);

// 正确的写法
const char* sid = session_ids[i].c_str();
col_sid->appendString(&sid, 1);
```

### 5. Agent::run_async 方法不存在

#### 问题现象

```cpp
error: 'class Agent' has no member named 'run_async'
```

#### 问题原因

`run_async` 方法在 `AgentBase` 模板中，但 `find_agent` 返回的是 `Agent` 基类指针。

#### 解决方案

```cpp
// 使用基类的 run 方法（同步）
auto result = agent->run(task);

// 处理结果
if (result.has_value()) {
    dict->set(new String("success"), new Bool(true));
    dict->set(new String("output"), new String(result.value()));
} else {
    dict->set(new String("success"), new Bool(false));
    dict->set(new String("error"), new String(result.error().message));
}
```

## 🏗️ 编译系统配置

### CMake 选项

```cmake
# plugins/dolphindb/CMakeLists.txt 关键配置
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# GCC ABI 兼容（DolphinDB server 使用旧 ABI）
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_definitions(PluginAgentOS PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
    target_compile_options(PluginAgentOS PRIVATE -fcoroutines)
endif()

# DuckDB 库查找
find_library(DUCKDB_LIBRARY duckdb HINTS 
    /usr/local/lib /opt/homebrew/lib /usr/lib64 /usr/lib)
```

### 跨平台支持

```cmake
# 支持多个平台的库路径
find_library(DUCKDB_LIBRARY duckdb HINTS 
    /usr/local/lib      # Linux 自编译
    /opt/homebrew/lib   # macOS Homebrew
    /usr/lib64          # Rocky Linux
    /usr/lib            # Ubuntu/Debian
)
```

## 🧪 验证和调试

### 符号检查

```bash
# 检查插件依赖的符号
nm -D libPluginAgentOS.so | grep "U.*DuckDB"

# 检查 DuckDB 库提供的符号
objdump -T /usr/local/lib/libduckdb.so | grep DuckDB

# 检查动态链接依赖
ldd libPluginAgentOS.so | grep duckdb
```

### 编译器版本检查

```bash
# 检查 DuckDB 库的编译器版本
readelf -p .comment /usr/local/lib/libduckdb.so

# 检查当前编译器版本
gcc --version
```

### ABI 一致性检查

```bash
# 检查符号 mangling 是否匹配
c++filt _ZN6duckdb6DuckDBC1ERKSsPNS_8DBConfigE
# 输出: duckdb::DuckDB::DuckDB(std::string const&, duckdb::DBConfig*)

c++filt _ZN6duckdb6DuckDBC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPNS_8DBConfigE
# 输出: duckdb::DuckDB::DuckDB(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, duckdb::DBConfig*)
```

## 📋 功能清单

### 插件导出函数

| 函数名 | DolphinDB 调用 | 描述 |
|--------|----------------|------|
| `agentOS::init` | `agentOS::init()` | 初始化 AgentOS |
| `agentOS::health` | `agentOS::health()` | 健康检查 |
| `agentOS::status` | `agentOS::status()` | 状态信息 |
| `agentOS::createAgent` | `agentOS::createAgent()` | 兼容别名：创建 Agent |
| `agentOS::createAgent2` | `agentOS::createAgent2()` | 推荐写法：显式 V2 创建 Agent |
| `agentOS::destroy` | `agentOS::destroy()` | 销毁 Agent |
| `agentOS::ask` | `agentOS::ask()` | 单轮同步对话；兼容 handle 重载，持久 Agent 推荐用 `ask2()` |
| `agentOS::ask2` | `agentOS::ask2()` | 推荐写法：持久 Agent 同步对话 |
| `agentOS::askStream` | `agentOS::askStream()` | 单轮流式对话；兼容 handle 重载，持久 Agent 推荐用 `askStream2()` |
| `agentOS::askStream2` | `agentOS::askStream2()` | 推荐写法：持久 Agent 流式对话 |
| `agentOS::askAsync` | `agentOS::askAsync()` | 异步对话 |
| `agentOS::poll` | `agentOS::poll()` | 轮询结果 |
| `agentOS::cancelAsync` | `agentOS::cancelAsync()` | 取消异步 |
| `agentOS::createKB` | `agentOS::createKB()` | 创建知识库 |
| `agentOS::ingest` | `agentOS::ingest()` | 文档摄取 |
| `agentOS::search` | `agentOS::search()` | 知识检索 |
| `agentOS::askWithKB` | `agentOS::askWithKB()` | RAG 对话 |
| `agentOS::askWithKBAsync` | `agentOS::askWithKBAsync()` | 异步 RAG |

## 🚨 故障排除

### 插件加载失败

1. **检查依赖库**:
   ```bash
   ldd ~/3.00.5/server/plugins/AgentOS/libPluginAgentOS.so
   ```

2. **检查符号匹配**:
   ```bash
   objdump -T ~/3.00.5/server/plugins/AgentOS/libPluginAgentOS.so | grep UNDEF
   ```

3. **检查 ABI 兼容性**:
   ```bash
   readelf -p .comment /usr/local/lib/libduckdb.so
   ```

### 编译错误

1. **清理构建目录**:
   ```bash
   rm -rf build
   cmake -B build -DCMAKE_POSITION_INDEPENDENT_CODE=ON
   ```

2. **检查编译器版本**:
   ```bash
   gcc --version
   scl enable gcc-toolset-14 bash
   ```

3. **检查依赖库**:
   ```bash
   dnf install -y sqlite-devel curl-devel gcc-toolset-14
   ```

## 📚 参考资料

- [DolphinDB Plugin Development Guide](https://www.dolphindb.cn/help200/PluginDevelopment/Introduction.html)
- [GCC C++ ABI Compatibility](https://gcc.gnu.org/onlinedocs/libstdc++/manual/abi.html)
- [AgentOS Documentation](https://github.com/tradercjz/agent-os)

## 🏷️ 版本历史

- **v1.0.0**: 初始版本，支持基础 AgentOS 功能
- **v1.1.0**: 添加知识库和 RAG 功能
- **v1.2.0**: 添加异步流式 API
- **v1.3.0**: 修复 ABI 兼容性问题

---

**注意**: 本文档基于 Rocky Linux 8/9 和 GCC 14.2.1 环境，其他发行版可能需要调整路径和依赖。
