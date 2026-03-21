// ============================================================
// Sample test plugin — compiled as a shared library (.dylib/.so)
// ============================================================
#include <agentos/core/plugin.hpp>

class SamplePlugin : public agentos::IPlugin {
public:
    std::string name() const override { return "sample"; }
    std::string version() const override { return "1.0.0"; }
    agentos::Result<void> init(agentos::AgentOS& /*os*/) override { return {}; }
    void shutdown() override {}
};

extern "C" agentos::IPlugin* agentos_plugin_create() {
    return new SamplePlugin();
}

extern "C" void agentos_plugin_destroy(agentos::IPlugin* p) {
    delete p;
}
