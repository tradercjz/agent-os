// Agent Snapshots, Cloning, and Conversation Persistence
#include <agentos/agentos.hpp>
#include <agentos/context/conversation_store.hpp>
#include <iostream>

using namespace agentos;

int main() {
    auto os = AgentOSBuilder().mock().security(false).build();

    // Create and use an agent
    auto agent = make_agent(*os, "original")
        .prompt("You are a coding assistant.")
        .context(4096)
        .create();

    (void)agent->run("What is a lambda in C++?");

    // Snapshot the agent
    auto snap_result = os->snapshot_agent(agent->id());
    if (snap_result.has_value()) {
        auto& snap = *snap_result;
        auto save_res = snap.save("/tmp/agent_snapshot.json");
        if (save_res.has_value()) {
            std::cout << "Agent snapshot saved\n";
        }
    }

    // Clone the agent (inherits config + context)
    auto clone = os->clone_agent(agent->id(), "assistant-v2");
    if (clone.has_value()) {
        std::cout << "Cloned agent: " << (*clone)->config().name
                  << " (ID: " << (*clone)->id() << ")\n";
    }

    // Conversation persistence
    context::ConversationStore store("/tmp/conversations");
    context::ConversationRecord record;
    record.conversation_id = "conv-001";
    record.agent_name = "original";
    record.messages.push_back(kernel::Message::user("Hello"));
    record.messages.push_back(kernel::Message::assistant("Hi!"));
    (void)store.save(record);

    auto ids = store.list();
    std::cout << "Saved conversations: " << ids.size() << "\n";

    return 0;
}
