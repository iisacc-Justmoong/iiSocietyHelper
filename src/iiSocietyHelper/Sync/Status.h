#pragma once
#include "iiSocietyHelper/Export.h"
// Qt's optional keyword macro collides with Core's private ring member name.
// Keep that compatibility concern at this existing Qt facade boundary.
#pragma push_macro("slots")
#undef slots
#include <iiSDKCore/Runtime/Query.h>
#include <iiSDKCore/Runtime/QueryStream.h>
#pragma pop_macro("slots")
#include <atomic>

namespace iiSocietyHelper::Sync {
enum class StatusCode : int { Before = 0, InProgress = 1, Complete = 2, Failed = 3, Cancelled = 4 };
struct ReadStatus final {};
class Controller;
class Synchronizer;

// Readable from any thread. Changes are dispatched on each receiver's chosen
// Core executor; the owning Controller/Synchronizer publishes on its own thread.
class IISOCIETYHELPER_EXPORT Status final {
public:
    using Query = ii::runtime::QueryPort<ReadStatus, int>;
    using Changes = ii::runtime::QueryStream<ReadStatus, int, int>;
    Status();
    ~Status();
    Status(const Status &) = delete;
    Status &operator=(const Status &) = delete;
    Query &query();
    Changes &changes();
private:
    friend class Controller;
    friend class Synchronizer;
    void publish(StatusCode code);
    std::atomic<int> value_{int(StatusCode::Before)};
    Query query_;
    Changes changes_;
};
}
