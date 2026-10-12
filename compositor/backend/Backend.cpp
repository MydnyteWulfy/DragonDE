#include "backend/Backend.h"

#include "core/Log.h"

#include <algorithm>

namespace dragon {

Output* Backend::addOutput(std::unique_ptr<Output> output)
{
    auto* raw = output.get();
    m_outputs.push_back(std::move(output));
    log::info("backend", "{}: output {} added", name(), raw->name());
    outputAdded.emit(raw);
    return raw;
}

void Backend::removeOutput(Output* output)
{
    auto it = std::ranges::find_if(m_outputs, [output](const auto& o) { return o.get() == output; });
    if (it == m_outputs.end())
        return;
    log::info("backend", "{}: output {} removed", name(), output->name());
    outputRemoved.emit(output);
    m_outputs.erase(it);
}

void Backend::clearOutputs()
{
    while (!m_outputs.empty())
        removeOutput(m_outputs.back().get());
}

} // namespace dragon
