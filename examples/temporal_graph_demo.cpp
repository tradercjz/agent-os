// ============================================================
// AgentOS Temporal Graph Demo
// 演示时序图数据库的核心功能：时间窗口查询和K-hop遍历
// ============================================================
#include <graph_engine/builder/graph_builder.hpp>
#include <graph_engine/core/immutable_graph.hpp>
#include <chrono>
#include <iostream>

using namespace graph_engine;

void ok(std::string_view msg) {
  std::cout << "\033[32m  ✓ " << msg << "\033[0m\n";
}
void info(std::string_view msg) {
  std::cout << "\033[34m  · " << msg << "\033[0m\n";
}
void section(std::string_view title) {
  std::cout << "\n\033[1;36m══════════════════════════════════════\033[0m\n  "
            << "\033[1;33m" << title << "\033[0m\n"
            << "\033[1;36m══════════════════════════════════════\033[0m\n";
}

// 将时间戳转换为可读格式
std::string ts_to_string(uint64_t ts) {
  if (ts == 0) return "永久";
  std::time_t t = static_cast<std::time_t>(ts);
  std::tm* tm = std::localtime(&t);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", tm);
  return std::string(buf);
}

int main() {
  section("1. 构建时序图：员工职业变迁");
  
  // 创建图构建器
  builder::GraphBuilder builder("/tmp/temporal_graph_demo");
  
  // 添加时序边（时间戳用简化表示）
  // 时间线：2020-2025年
  uint64_t t2020 = 1577836800;  // 2020-01-01
  uint64_t t2021 = 1609459200;  // 2021-01-01
  uint64_t t2022 = 1640995200;  // 2022-01-01  
  uint64_t t2023 = 1672531200;  // 2023-01-01
  uint64_t t2024 = 1704067200;  // 2024-01-01
  
  // Alice 的职业变迁
  builder.add_edge("Alice", "Google", "works_at", t2020, t2022);  // 2020-2022在Google
  builder.add_edge("Alice", "Meta", "works_at", t2022, 0);       // 2022至今在Meta
  
  // Bob 的职业变迁
  builder.add_edge("Bob", "Microsoft", "works_at", t2020, t2023); // 2020-2023在Microsoft
  builder.add_edge("Bob", "Apple", "works_at", t2023, 0);        // 2023至今在Apple
  
  // Carol 的职业变迁（跳槽频繁）
  builder.add_edge("Carol", "Amazon", "works_at", t2020, t2021);  // 2020-2021在Amazon
  builder.add_edge("Carol", "Google", "works_at", t2021, t2022);  // 2021-2022在Google
  builder.add_edge("Carol", "Meta", "works_at", t2022, t2024);    // 2022-2024在Meta
  builder.add_edge("Carol", "Apple", "works_at", t2024, 0);       // 2024至今在Apple
  
  // 添加管理关系
  builder.add_edge("Alice", "Bob", "manages", t2022, 0);          // Alice从2022年开始管理Bob
  builder.add_edge("Bob", "Carol", "manages", t2023, 0);         // Bob从2023年开始管理Carol
  
  // 添加技能关系
  builder.add_edge("Alice", "Machine Learning", "skilled_in", t2020, 0);
  builder.add_edge("Bob", "Distributed Systems", "skilled_in", t2020, 0);
  builder.add_edge("Carol", "Frontend Development", "skilled_in", t2020, 0);
  builder.add_edge("Carol", "Machine Learning", "skilled_in", t2023, 0); // Carol 2023年开始学习ML
  
  ok("图数据构建完成");
  
  // 构建不可变图
  if (!builder.build()) {
    std::cerr << "Failed to build graph\n";
    return 1;
  }
  ok("CSR/CSC索引构建完成");
  
  // 加载图
  core::ImmutableGraph graph("/tmp/temporal_graph_demo");
  if (!graph.load()) {
    std::cerr << "Failed to load graph\n";
    return 1;
  }
  ok("时序图加载完成");
  
  section("2. 时间切片查询：查看不同时间点的状态");
  
  // 查询2021年的状态
  info("查询2021年Alice的工作状态：");
  auto subgraph_2021 = graph.k_hop("Alice", 1, t2021);
  if (!subgraph_2021.triples.empty()) {
    for (const auto& triple : subgraph_2021.triples) {
      if (triple.relation == "works_at") {
        std::cout << "    " << triple.src << " --[" << triple.relation 
                  << "]--> " << triple.dst 
                  << " (有效期: " << ts_to_string(triple.start_ts) 
                  << " ~ " << ts_to_string(triple.end_ts) << ")\n";
      }
    }
  }
  
  // 查询2023年的状态
  info("查询2023年Alice的工作状态：");
  auto subgraph_2023 = graph.k_hop("Alice", 1, t2023);
  if (!subgraph_2023.triples.empty()) {
    for (const auto& triple : subgraph_2023.triples) {
      if (triple.relation == "works_at") {
        std::cout << "    " << triple.src << " --[" << triple.relation 
                  << "]--> " << triple.dst 
                  << " (有效期: " << ts_to_string(triple.start_ts) 
                  << " ~ " << ts_to_string(triple.end_ts) << ")\n";
      }
    }
  }
  
  section("3. K-hop遍历：查询组织关系网络");
  
  // 查询Alice的2度关系网络（当前时间）
  info("查询Alice的2度关系网络（当前）：");
  auto subgraph_current = graph.k_hop("Alice", 2, 0); // 0表示当前时间
  if (!subgraph_current.triples.empty()) {
    std::cout << "  发现 " << subgraph_current.node_ids.size() << " 个节点\n";
    std::cout << "  发现 " << subgraph_current.triples.size() << " 条关系\n\n";
    
    // 打印所有关系
    for (const auto& triple : subgraph_current.triples) {
      std::cout << "    " << triple.src << " --[" << triple.relation 
                << "]--> " << triple.dst << "\n";
    }
  }
  
  section("4. 时序推理：追踪技能传播路径");
  
  // 查询Carol学习Machine Learning后的影响
  info("查询Carol学习ML后的管理链变化：");
  auto subgraph_ml = graph.k_hop("Carol", 3, t2024); // 2024年的状态
  if (!subgraph_ml.triples.empty()) {
    // 找到所有与ML相关的路径
    for (const auto& triple : subgraph_ml.triples) {
      if (triple.relation == "skilled_in" && triple.dst == "Machine Learning") {
        std::cout << "    " << triple.src << " 掌握了 " << triple.dst 
                  << " (学习时间: " << ts_to_string(triple.start_ts) << ")\n";
      }
    }
    
    // 展示管理链
    std::cout << "\n  管理链：\n";
    for (const auto& triple : subgraph_ml.triples) {
      if (triple.relation == "manages") {
        std::cout << "    " << triple.src << " 管理 " << triple.dst << "\n";
      }
    }
  }
  
  section("5. 对比：普通图 vs 时序图");
  
  info("普通图查询（忽略时间）：");
  auto subgraph_no_time = graph.k_hop("Carol", 2, 0);
  if (!subgraph_no_time.triples.empty()) {
    std::cout << "  Carol的所有关系（历史+当前）：\n";
    for (const auto& triple : subgraph_no_time.triples) {
      if (triple.src == "Carol") {
        std::cout << "    " << triple.src << " --[" << triple.relation 
                  << "]--> " << triple.dst << "\n";
      }
    }
  }
  
  info("时序图查询（仅当前有效关系）：");
  // 这里我们手动过滤当前时间有效的边
  std::cout << "  Carol的当前关系：\n";
  for (const auto& triple : subgraph_no_time.triples) {
    if (triple.src == "Carol") {
      // 检查关系在当前时间是否有效
      bool is_current = (triple.end_ts == 0) || (triple.end_ts >= 1704067200);
      if (is_current) {
        std::cout << "    " << triple.src << " --[" << triple.relation 
                  << "]--> " << triple.dst << " ✅当前有效\n";
      } else {
        std::cout << "    " << triple.src << " --[" << triple.relation 
                  << "]--> " << triple.dst << " ❌已过期\n";
      }
    }
  }
  
  section("6. 实际应用场景");
  
  info("场景1：风险控制 - 查询某员工的历史权限");
  auto carol_history = graph.k_hop("Carol", 2, 0);
  std::cout << "  Carol的完整职业轨迹：\n";
  for (const auto& triple : carol_history.triples) {
    if (triple.src == "Carol" && triple.relation == "works_at") {
      std::cout << "    " << ts_to_string(triple.start_ts) << " ~ " 
                << ts_to_string(triple.end_ts) << ": " << triple.dst << "\n";
    }
  }
  
  info("场景2：知识传播 - 追踪技能在组织中的传播");
  std::cout << "  Machine Learning技能传播路径：\n";
  // 找到所有掌握ML的人
  for (const auto& triple : carol_history.triples) {
    if (triple.relation == "skilled_in" && triple.dst == "Machine Learning") {
      std::cout << "    " << triple.src << " (从" 
                << ts_to_string(triple.start_ts) << "开始)\n";
    }
  }
  
  ok("时序图演示完成");
  
  std::cout << "\n\033[1;32m核心优势总结：\033[0m\n";
  std::cout << "  1. \033[33m时间维度\033[0m：每个关系都有生效/失效时间\n";
  std::cout << "  2. \033[33m历史追溯\033[0m：可以查询任意时间点的状态\n";
  std::cout << "  3. \033[33m时序推理\033[0m：支持基于时间的因果分析\n";
  std::cout << "  4. \033[33m生命周期管理\033[0m：自然处理关系的开始和结束\n";
  std::cout << "  5. \033[33mK-hop + 时间过滤\033[0m：高效的时序子图查询\n";
  
  return 0;
}
