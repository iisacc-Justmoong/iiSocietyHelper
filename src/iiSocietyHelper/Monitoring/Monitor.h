#pragma once
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <iiSocietyHelper/Monitoring/Export.h>

namespace iiSocietyHelper::Monitoring {
struct Section {
    std::string key;
    std::uint64_t files = 0, directories = 0, bytes = 0;
    std::uint64_t fingerprint = 0;
    std::int64_t lastModified = 0;
    bool available = false, complete = true;
    bool operator==(const Section&) const = default;
};
struct State {
    std::uint32_t schemaVersion = 1;
    std::string instanceId, containerId;
    std::filesystem::path root;
    std::uint64_t revision = 0;
    std::int64_t observedAtMs = 0;
    bool available = false, complete = true, daemonRunning = false;
    std::string error;
    std::vector<Section> sections;
    std::vector<std::string> changedSections;
};
struct Options {
    std::size_t maxDepth = 64, maxEntries = 100000;
};
struct Selection { std::filesystem::path root; std::string containerId; };
template<class T> using Result = std::expected<T, std::string>;

// One native producer per container. Poll reads public metadata only. Runtime
// snapshots are atomic and excluded from the inventory. No Qt dependency.
class IISOCIETYHELPER_MONITORING_EXPORT Publisher final {
public:
    Publisher();
    ~Publisher();
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;
    Result<void> start(const std::filesystem::path& root, std::string expectedId = {}, Options options = {});
    Result<bool> poll(); // true only when the logical state/revision changed
    void stop();
    const State& state() const;
private:
    class Private;
    std::unique_ptr<Private> d;
};
IISOCIETYHELPER_MONITORING_EXPORT std::filesystem::path statePath(const std::filesystem::path& root);
IISOCIETYHELPER_MONITORING_EXPORT Result<State> read(const std::filesystem::path& root,
    std::string expectedId = {}, std::chrono::milliseconds staleAfter = std::chrono::seconds(5));
IISOCIETYHELPER_MONITORING_EXPORT std::filesystem::path defaultSelectionFile();
IISOCIETYHELPER_MONITORING_EXPORT Result<Selection> readSelection(const std::filesystem::path& file);
IISOCIETYHELPER_MONITORING_EXPORT std::string toJson(const State& state);

// Callback runs on this object's worker; GUI adapters must marshal to their UI
// thread. Late subscribers receive the current snapshot. Heartbeats do not
// duplicate state callbacks. Stop joins the worker before returning.
class IISOCIETYHELPER_MONITORING_EXPORT Subscription final {
public:
    using Handler = std::function<void(const State&)>;
    Subscription();
    ~Subscription();
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Result<void> start(const std::filesystem::path& root, std::string expectedId, Handler handler,
        std::chrono::milliseconds interval = std::chrono::milliseconds(250));
    void stop();
private:
    class Private;
    std::unique_ptr<Private> d;
};
}
