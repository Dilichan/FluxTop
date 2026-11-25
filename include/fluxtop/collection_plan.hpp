#pragma once

namespace fluxtop {

// Startup configuration for the sampler, independent of displayed columns.
struct CollectionPlan {
    bool gpu = true;
    bool network = true;
    bool fps = false;
};

} // namespace fluxtop
