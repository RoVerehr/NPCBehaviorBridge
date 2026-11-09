#pragma once
#include "nbb/core.hpp"
#include "nbb/runtime_policy.hpp"
#include <optional>
namespace nbb::load_capture {
bool install(const runtime::Version& host);
void finish();
std::optional<Json> takePlan(const Json& manifest,const Json& config,const Sequence& exclusions);
Json diagnosticSummary();
void releasePlan();
bool canPruneNativeWork();
double measuredEarlyMilliseconds();
}
