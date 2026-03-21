#!/usr/bin/env python3
"""Basic smoke test for AgentOS Python bindings."""
try:
    import agentos_py as aos
    print(f"AgentOS version: {aos.version()}")

    os = aos.quickstart_mock()
    print(f"Agent count: {os.agent_count()}")
    print(f"Metrics:\n{os.metrics_prometheus()[:200]}...")
    print("OK: All bindings work!")
except ImportError:
    print("SKIP: agentos_py not built (enable with -DAGENTOS_ENABLE_PYTHON=ON)")
