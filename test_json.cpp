#include <iostream>
#include <string>
#include "agentos/kernel/llm_kernel.hpp"
int main() {
    agentos::kernel::OpenAIBackend backend("fake", "https://api.deepseek.com", "deepseek-chat");
    agentos::kernel::LLMRequest req;
    req.messages.push_back(agentos::kernel::Message::user("你是谁"));
    req.temperature = 0.7;
    req.max_tokens = 100;
    
    // Simulate what Agent::think does
    std::string tools_json = "[{\"type\":\"function\",\"function\":{\"name\":\"demo\",\"description\":\"demo\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}}]";
    req.tools_json = tools_json;

    // We can't call build_request_json because it's private, but we can call complete and catch the debug output before it curl fails
    auto res = backend.complete(req);
    return 0;
}
