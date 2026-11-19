#pragma once
#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#  if defined(IISOCIETYHELPER_RESIDENT_BUILD)
#    define IISOCIETYHELPER_RESIDENT_EXPORT __declspec(dllexport)
#  else
#    define IISOCIETYHELPER_RESIDENT_EXPORT __declspec(dllimport)
#  endif
#else
#  define IISOCIETYHELPER_RESIDENT_EXPORT __attribute__((visibility("default")))
#endif

namespace iiSocietyHelper {
struct ResidentMessage { std::string role; std::string content; };

/// A single absolute budget may be reused across a multi-frame exchange.
/// The optional cancellation predicate runs on the calling thread and must return promptly.
struct ResidentWait {
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    std::function<bool()> canceled;
};

// Caller-owned, blocking local transport. Run receiveJson() on an explicit
// asynchronous task. The daemon owns the model and scopes replies by socket.
// On Unix, close() interrupts a blocked receive; wait for outstanding calls
// to return before reconnecting or destroying the client.
class IISOCIETYHELPER_RESIDENT_EXPORT ResidentNative final {
public:
    ResidentNative();
    ~ResidentNative();
    ResidentNative(const ResidentNative &) = delete;
    ResidentNative &operator=(const ResidentNative &) = delete;
    std::expected<void,std::string> connect(std::filesystem::path brokerDirectory = {});
    /// Connect to the same-user broker with a caller-controlled wait budget.
    std::expected<void,std::string> connect(std::filesystem::path brokerDirectory, const ResidentWait &);
    std::expected<void,std::string> subscribe();
    std::expected<std::string,std::string> generate(const std::vector<ResidentMessage> &messages);
    std::expected<void,std::string> cancel(std::string_view requestId);
    // For explicit advanced options and status queries, callers may send any
    // valid protocol JSON object and parse the complete response themselves.
    std::expected<void,std::string> sendJson(std::string_view object);
    std::expected<std::string,std::string> receiveJson();
    /// A failed transfer closes the connection so no partial frame can be reused.
    std::expected<void,std::string> sendJson(std::string_view object, const ResidentWait &);
    std::expected<std::string,std::string> receiveJson(const ResidentWait &);
    void close() noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> d;
};
}
