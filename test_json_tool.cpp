#include <iostream>
#include <string>
#include "agentos/kernel/llm_kernel.hpp"
int main() {
    agentos::kernel::OpenAIBackend backend("fake", "https://api.deepseek.com", "deepseek-chat");
    agentos::kernel::LLMRequest req;
    req.messages.push_back(agentos::kernel::Message::user("计算 12 加 5 的结果，再乘以 7"));
    
    agentos::kernel::Message m2;
    m2.role = agentos::kernel::Role::Assistant;
    m2.content = "";
    req.messages.push_back(m2);

    agentos::kernel::Message m3;
    m3.role = agentos::kernel::Role::Tool;
    m3.content = "119";
    m3.tool_call_id = "call_123";
    req.messages.push_back(m3);

    req.temperature = 0.7;
    req.max_tokens = 100;

    std::string tools_json = "[{\"type\":\"function\",\"function\":{\"name\":\"demo\",\"description\":\"demo\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}}]";
    req.tools_json = tools_json;

    auto res = backend.complete(req);
    return 0;
}
