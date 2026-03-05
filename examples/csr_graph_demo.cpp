#include "agentos/kernel/llm_kernel.hpp"
#include "graph_engine/builder/graph_builder.hpp"
#include "graph_engine/core/immutable_graph.hpp"
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace graph_engine;
using namespace agentos;

// A utility function to print a node's neighborhood
void print_neighbors(const core::ImmutableGraph &graph,
                     const std::string &node_name) {
  std::cout << "\033[1;36m[" << node_name << "]\033[0m's neighbors:\n";
  auto neighbors = graph.get_neighbors(node_name);
  if (neighbors.empty()) {
    std::cout << "  (No outgoing edges found)\n";
    return;
  }

  for (const auto &[target, relation] : neighbors) {
    std::cout << "  --(\033[33m" << relation << "\033[0m)--> [\033[32m"
              << target << "\033[0m]\n";
  }
  std::cout << "\n";
}

// A utility function to print a node's incoming neighborhood using CSC
void print_incoming_neighbors(const core::ImmutableGraph &graph,
                              const std::string &node_name) {
  std::cout << "\033[1;36m[" << node_name
            << "]\033[0m's INCOMING neighbors (CSC):\n";
  auto neighbors = graph.get_incoming_neighbors(node_name);
  if (neighbors.empty()) {
    std::cout << "  (No incoming edges found)\n";
    return;
  }

  for (const auto &[source, relation] : neighbors) {
    std::cout << "  [\033[32m" << source << "\033[0m] --(\033[33m" << relation
              << "\033[0m)--> \n";
  }
  std::cout << "\n";
}

int main() {
  fs::path db_path = "/tmp/agentos_financial_graph";

  std::cout << "===========================================\n";
  std::cout << "  AgentOS LLM -> CSR Graph Extraction Demo\n";
  std::cout << "===========================================\n\n";

  // 1. Initialize LLM Backend
  std::string api_key =
      std::getenv("OPENAI_API_KEY") ? std::getenv("OPENAI_API_KEY") : "";
  std::string base_url = std::getenv("OPENAI_BASE_URL")
                             ? std::getenv("OPENAI_BASE_URL")
                             : "https://api.openai.com/v1";
  std::string model =
      std::getenv("OPENAI_MODEL") ? std::getenv("OPENAI_MODEL") : "gpt-4o-mini";

  if (api_key.empty()) {
    std::cerr << "\033[31m[Error] Please set OPENAI_API_KEY environment "
                 "variable to run the LLM extraction demo.\033[0m\n";
    return 1;
  }

  std::cout << "[LLM] Initializing backend: " << base_url << " (" << model
            << ")...\n";
  kernel::OpenAIBackend llm(api_key, base_url, model);

  // 2. The Unstructured Text (Simulated Financial News)
  std::string financial_news = R"(
        Apple Inc. (AAPL) today announced a strategic partnership with OpenAI to integrate ChatGPT into iOS 18.
        Tim Cook, CEO of Apple, stated this will revolutionize Siri.
        Meanwhile, Microsoft, which has heavily invested in OpenAI, is reportedly monitoring the Apple deal closely.
        Satya Nadella (CEO of Microsoft) mentioned they maintain a strong relationship with Sam Altman (OpenAI CEO).
    )";

  std::cout << "\n[Phase 1] Analyzing Unstructured Financial Text:\n";
  std::cout << "-------------------------------------------\n";
  std::cout << financial_news << "\n";
  std::cout << "-------------------------------------------\n\n";

  // 3. Extract Triplets via LLM Force JSON output
  std::cout << "[Phase 2] Extracting Knowledge Triplets via LLM...\n";

  kernel::LLMRequest req;
  std::string system_prompt =
      "You are a specialized financial Knowledge Graph extractor. "
      "Read the provided text and extract entities and relationships. "
      "Return ONLY a JSON array of objects, with each object having exactly "
      "three keys: "
      "'src', 'dst', and 'relation'. "
      "Keep entity names short (e.g., 'Apple', 'Tim Cook'). "
      "Keep relationships as snake_case verbs (e.g., 'partnered_with', "
      "'invested_in', 'ceo_of').";
  req.messages.push_back(kernel::Message::system(system_prompt));
  req.messages.push_back(kernel::Message::user(financial_news));
  req.temperature = 0.1f;

  auto resp = llm.complete(req);
  if (!resp) {
    std::cerr << "LLM request failed: " << resp.error().message << "\n";
    return 1;
  }

  std::string llm_output = resp->content;

  // Clean markdown code blocks if the LLM adds them
  if (llm_output.find("```json") == 0) {
    llm_output = llm_output.substr(7);
    if (llm_output.rfind("```") != std::string::npos) {
      llm_output = llm_output.substr(0, llm_output.rfind("```"));
    }
  }

  std::cout << "  -> LLM Raw Output Parsing...\n";
  std::vector<std::tuple<std::string, std::string, std::string>> triplets;

  try {
    nlohmann::json parsed = nlohmann::json::parse(llm_output);

    // Handle if LLM wraps the array inside an object e.g., {"triplets": [...]}
    nlohmann::json arr = parsed;
    if (parsed.is_object()) {
      for (auto &el : parsed.items()) {
        if (el.value().is_array()) {
          arr = el.value();
          break;
        }
      }
    }

    if (arr.is_array()) {
      for (const auto &item : arr) {
        if (item.contains("src") && item.contains("dst") &&
            item.contains("relation")) {
          triplets.push_back({item["src"].get<std::string>(),
                              item["dst"].get<std::string>(),
                              item["relation"].get<std::string>()});
        }
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "JSON Parsing error: " << e.what() << "\n";
    std::cerr << "Raw Output was:\n" << llm_output << "\n";
    return 1;
  }

  std::cout << "  -> Extracted " << triplets.size()
            << " valid relationships.\n\n";

  // 4. Offline Graph Building
  std::cout << "[Phase 3] Building Immutable CSR Graph...\n";
  if (fs::exists(db_path)) {
    fs::remove_all(db_path);
  }

  {
    builder::GraphBuilder builder(db_path);
    for (const auto &[src, dst, rel] : triplets) {
      builder.add_edge(src, dst, rel);
    }

    if (builder.build()) {
      std::cout << "  -> Graph successfully compressed and dumped to disk.\n\n";
    } else {
      std::cerr << "Failed to build graph.\n";
      return 1;
    }
  }

  // 5. Online Retrieval Mmap Loading
  std::cout << "[Phase 4] Launching CSR Engine (Zero-Copy Mmap)...\n";
  {
    core::ImmutableGraph engine(db_path);
    if (!engine.load()) {
      std::cerr << "Failed to mmap graph.\n";
      return 1;
    }

    std::cout << "  -> Load Successful! Ready for queries.\n";
    std::cout << "-------------------------------------------\n\n";

    // Query example
    std::cout
        << "[Graph Query] Show me what we know about Apple and Microsoft:\n\n";
    print_neighbors(engine, "Apple");
    print_neighbors(engine, "Microsoft");
    print_neighbors(engine, "OpenAI");
    print_incoming_neighbors(engine, "OpenAI");
  }

  std::cout << "===========================================\n";
  return 0;
}
