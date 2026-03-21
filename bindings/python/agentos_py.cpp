#ifdef AGENTOS_ENABLE_PYTHON

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <agentos/agentos.hpp>

namespace py = pybind11;

PYBIND11_MODULE(agentos_py, m) {
    m.doc() = "AgentOS Python bindings";

    // Version
    m.def("version", []() { return agentos::version().to_string(); });

    // AgentOSBuilder
    py::class_<agentos::AgentOSBuilder>(m, "AgentOSBuilder")
        .def(py::init<>())
        .def("mock", &agentos::AgentOSBuilder::mock, py::return_value_policy::reference)
        .def("threads", &agentos::AgentOSBuilder::threads, py::return_value_policy::reference)
        .def("tpm", &agentos::AgentOSBuilder::tpm, py::return_value_policy::reference)
        .def("build", &agentos::AgentOSBuilder::build);

    // AgentOS (opaque, accessed via unique_ptr)
    py::class_<agentos::AgentOS, std::unique_ptr<agentos::AgentOS>>(m, "AgentOS")
        .def("agent_count", &agentos::AgentOS::agent_count)
        .def("metrics_prometheus", &agentos::AgentOS::metrics_prometheus);

    // Quickstart functions
    m.def("quickstart_mock", &agentos::quickstart_mock);
}

#endif // AGENTOS_ENABLE_PYTHON
