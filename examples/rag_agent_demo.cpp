#include "agentos/kernel/llm_kernel.hpp"
#include "agentos/knowledge/knowledge_base.hpp"
#include <iostream>
#include <memory>

using namespace agentos::kernel;
using namespace agentos::knowledge;

int main() {
  std::cout << "===========================================\n";
  std::cout << "  AgentOS 20-Line Enterprise RAG Setup\n";
  std::cout << "===========================================\n\n";

  const char *api_key = std::getenv("OPENAI_API_KEY");
  if (!api_key) {
    std::cerr << "[Error] Please set OPENAI_API_KEY environment variable.\n";
    return 1;
  }

  const char *base_url = std::getenv("OPENAI_BASE_URL");
  std::string url = base_url ? base_url : "https://api.openai.com/v1";

  const char *model_env = std::getenv("OPENAI_MODEL");
  std::string model = model_env ? model_env : "gpt-4o-mini";

  // 1. Boot up the LLM Backend
  auto llm = std::make_shared<OpenAIBackend>(api_key, url, model);

  // 2. Instantiate KnowledgeBase & Ingest Local Documents
  KnowledgeBase kb(llm);
  kb.ingest_directory("./examples/sample_knowledge");

  // 3. User query invokes Multi-Way Hybrid Search (Semantic + BM25 + RRF)
  std::string query = "What are the secret plans for 2027?";
  std::cout << "\n[User]: " << query << "\n";

  // Fetch top 3 most relevant RRF chunks
  auto facts = kb.search(query, 3);

  // 4. Inject into Agent Generation Phase
  LLMRequest req;
  req.model = model;

  std::string context = "Context Facts:\n";
  for (const auto &f : facts)
    context += "- " + f.content + "\n";

  req.messages.push_back(Message::system(
      "You are a helpful agent. Answer based ONLY on the context."));
  req.messages.push_back(Message::user(context + "\nQuestion: " + query));

  // 5. Stream Output back to User
  std::cout << "[Agent]: ";
  auto _ = llm->stream(
      req, [](std::string_view token) { std::cout << token << std::flush; });
  std::cout << "\n\n";

  return 0;
}
