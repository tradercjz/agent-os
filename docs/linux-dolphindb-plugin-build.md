# Linux 上编译 DolphinDB 插件指南

## 概述

本文档总结了在 Rocky Linux 上编译 AgentOS DolphinDB 插件时遇到的常见问题及其解决方案。

## 🔧 常见问题及解决方案

### 1. C++ ABI 不匹配问题

#### 问题现象
```bash
loadPlugin(getHomeDir() + "/plugins/AgentOS/PluginAgentOS.txt")
# 错误: undefined symbol: _ZN6duckdb10Connection7PrepareERKSs
```

#### 问题原因
- DuckDB 库编译时使用的 GCC 版本与当前环境不匹配
- 旧版本 DuckDB (GCC 8.5.0) 与新版本插件 (GCC 14.2.1) 的 C++ string 符号不同

#### 解决方案
```bash
# 下载兼容的 DuckDB 预编译版本
cd /tmp
wget https://github.com/duckdb/duckdb/releases/download/v1.1.3/libduckdb-linux-amd64.zip
unzip libduckdb-linux-amd64.zip

# 替换系统库
sudo cp libduckdb.so /usr/local/lib/
sudo cp duckdb.hpp /usr/local/include/

# 重新编译插件
cmake --build build_plugin --target PluginAgentOS --clean-first -- -j$(nproc)
```

### 2. 位置无关代码 (-fPIC) 问题

#### 问题现象
```bash
relocation R_X86_64_32 against '.rodata.str1.8' can not be used when making a shared object
```

#### 问题原因
- AgentOS 静态库没有用 `-fPIC` 编译
- 插件需要链接静态库但符号不是位置无关的

#### 解决方案
```bash
# 重新配置构建目录启用 -fPIC
rm -rf build_plugin
cmake -B build_plugin -DCMAKE_POSITION_INDEPENDENT_CODE=ON

# 重新编译
cmake --build build_plugin --target PluginAgentOS -- -j$(nproc)
```

### 3. 跨平台库路径问题

#### 问题现象
```bash
DuckDB library not found
```

#### 问题原因
- 不同 Linux 发行版的库路径不同
- macOS 和 Linux 的 Homebrew 路径不同

#### 解决方案
更新 CMakeLists.txt 中的库查找路径：

```cmake
# 支持多个平台路径
find_library(DUCKDB_LIBRARY duckdb HINTS 
    /usr/local/lib /opt/homebrew/lib /usr/lib64 /usr/lib)
find_path(DUCKDB_INCLUDE_DIR duckdb.hpp HINTS
    /usr/local/include /opt/homebrew/include /usr/include)
```

### 4. 测试链接问题

#### 问题现象
```bash
undefined reference to `testing::internal::EqFailure'
```

#### 问题原因
- GoogleTest 在 Rocky Linux 上的链接问题
- 只影响单元测试，不影响插件功能

#### 解决方案
```bash
# 只编译插件，跳过测试
cmake --build build_plugin --target PluginAgentOS -- -j$(nproc)

# 或者忽略测试错误
cmake --build build_plugin -- -j$(nproc) || true
```

## 🚀 完整编译流程

### 1. 环境准备
```bash
# Rocky Linux 依赖
sudo dnf install -y sqlite-devel curl-devel gcc-toolset-14
scl enable gcc-toolset-14 bash

# 安装兼容的 DuckDB
cd /tmp
wget https://github.com/duckdb/duckdb/releases/download/v1.1.3/libduckdb-linux-amd64.zip
unzip libduckdb-linux-amd64.zip
sudo cp libduckdb.so /usr/local/lib/
sudo cp duckdb.hpp /usr/local/include/
```

### 2. 配置构建
```bash
# 清理旧的构建目录
rm -rf build_plugin

# 配置启用 -fPIC
cmake -B build_plugin -DCMAKE_POSITION_INDEPENDENT_CODE=ON
```

### 3. 编译插件
```bash
# 只编译插件（推荐）
cmake --build build_plugin --target PluginAgentOS -- -j$(nproc)

# 或者强制重新编译
cmake --build build_plugin --target PluginAgentOS --clean-first -- -j$(nproc)
```

### 4. 部署插件
```bash
# 复制到 DolphinDB
cp build_plugin/plugins/libPluginAgentOS.so ~/3.00.5/server/plugins/AgentOS/

# 验证依赖
ldd ~/3.00.5/server/plugins/AgentOS/libPluginAgentOS.so | grep duckdb
```

## 🔍 验证检查

### 检查插件符号
```bash
# 检查 DuckDB 符号
objdump -T build_plugin/plugins/libPluginAgentOS.so | grep duckdb | head -3

# 检查 AgentOS 符号
nm -D build_plugin/plugins/libPluginAgentOS.so | grep agentos | head -3
```

### 检查库依赖
```bash
# 检查动态链接
ldd build_plugin/plugins/libPluginAgentOS.so

# 检查 DuckDB 链接
ldd build_plugin/plugins/libPluginAgentOS.so | grep duckdb
```

## 📋 最佳实践

1. **使用兼容的库版本**: 确保所有依赖库使用相同的编译器版本
2. **启用 -fPIC**: 为静态库编译启用位置无关代码
3. **跨平台路径**: 在 CMakeLists.txt 中支持多个平台的库路径
4. **分离编译**: 单独编译插件避免测试链接问题
5. **符号检查**: 编译后验证插件包含正确的符号

## 🆘 故障排除

### 插件加载失败
1. 检查 `ldd` 输出确认所有依赖都能找到
2. 使用 `objdump -T` 检查符号匹配
3. 确认 ABI 兼容性 (`_GLIBCXX_USE_CXX11_ABI=0`)

### 编译错误
1. 检查编译器版本兼容性
2. 确认所有依赖库已安装
3. 使用 `--clean-first` 强制重新编译

### 符号未找到
1. 重新安装兼容版本的依赖库
2. 检查库的编译环境
3. 使用预编译版本而非源码编译

---

**注意**: 本文档基于 Rocky Linux 8/9 和 GCC 14.2.1 环境，其他发行版可能需要调整。
