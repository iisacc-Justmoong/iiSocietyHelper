#include <iiSocietyHelper/Monitoring/Monitor.h>
#include <InventoryIndex.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <csignal>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
struct Arguments {
    std::filesystem::path root, selection = iiSocietyHelper::Monitoring::defaultSelectionFile();
    std::string identifier;
    std::string indexFolder, indexKey;
    int intervalMs = 1000, scanIntervalMs = 5000, exitAfterMs = 0;
    iiSocietyHelper::Monitoring::Options limits;
    bool once = false, read = false, watch = false, queryList = false, queryGet = false;
    bool intervalExplicit = false, scanIntervalExplicit = false;
};
bool number(std::string_view input, int& out) {
    const auto [end, error] = std::from_chars(input.data(), input.data()+input.size(), out);
    return error == std::errc{} && end == input.data()+input.size();
}
std::filesystem::path path(const std::string& value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
std::string quote(std::string_view value) {
    std::ostringstream out; out << '"';
    for (const unsigned char c : value) {
        switch (c) {
        case '"': out << "\\\""; break; case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break; case '\r': out << "\\r"; break; case '\t': out << "\\t"; break;
        default: if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec; else out << char(c);
        }
    }
    out << '"'; return out.str();
}
std::string json(const iiSocietyContainer::InventoryEntry &value) {
    std::ostringstream out;
    out << "{\"section\":" << quote(value.section) << ",\"path\":" << quote(value.path)
        << ",\"name\":" << quote(value.name) << ",\"key\":" << quote(value.key)
        << ",\"metadataRevision\":" << quote(value.revision) << ",\"size\":" << value.size
        << ",\"modifiedMs\":" << value.modifiedMs << ",\"directory\":" << (value.directory ? "true" : "false") << '}';
    return out.str();
}
}
int main(int argc, char** argv) {
    Arguments args;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--help") {
            std::cout << "SocietyContainerDaemon [--root PATH --identifier UUID | --selection-file PATH]\n"
                         "  --interval-ms N     heartbeat interval (50..60000, default 1000)\n"
                         "  --scan-interval-ms N  full scan interval (50..60000, default 5000)\n"
                         "  --max-entries N     optional metadata entry budget\n"
                         "  --max-depth N       directory depth (default 256)\n"
                         "  --once              publish once and shut down\n"
                         "  --read-state        read the latest state and exit\n"
                         "  --index-list PATH   list cached immediate children by root-relative path\n"
                         "  --index-get KEY     look up an indexed key\n"
                         "  --watch             subscribe without owning a publisher\n"
                         "  --exit-after-ms N   bounded diagnostic run\n";
            return 0;
        }
        if (flag == "--version") { std::cout << "iiSocietyHelper Monitoring " IISOCIETYHELPER_VERSION "\n"; return 0; }
        if (flag == "--once") { args.once = true; continue; }
        if (flag == "--read-state") { args.read = true; continue; }
        if (flag == "--watch") { args.watch = true; continue; }
        if (i+1 == argc) { std::cerr << "Missing value: " << flag << '\n'; return 2; }
        const std::string value = argv[++i];
        if (flag == "--root") args.root = path(value);
        else if (flag == "--selection-file") args.selection = path(value);
        else if (flag == "--identifier") args.identifier = value;
        else if (flag == "--index-list") { args.queryList = true; args.indexFolder = value; }
        else if (flag == "--index-get") { args.queryGet = true; args.indexKey = value; }
        else {
            int parsed = 0;
            if (!number(value, parsed)) { std::cerr << "Invalid integer: " << flag << '\n'; return 2; }
            if (flag == "--interval-ms" && parsed >= 50 && parsed <= 60000) { args.intervalMs = parsed; args.intervalExplicit = true; }
            else if (flag == "--scan-interval-ms" && parsed >= 50 && parsed <= 60000) { args.scanIntervalMs = parsed; args.scanIntervalExplicit = true; }
            else if (flag == "--exit-after-ms" && parsed > 0) args.exitAfterMs = parsed;
            else if (flag == "--max-entries" && parsed > 0) args.limits.maxEntries = std::size_t(parsed);
            else if (flag == "--max-depth" && parsed >= 0 && parsed <= 256) args.limits.maxDepth = std::size_t(parsed);
            else { std::cerr << "Unknown option or out-of-range value: " << flag << '\n'; return 2; }
        }
    }
    if (args.intervalExplicit && !args.scanIntervalExplicit) args.scanIntervalMs = args.intervalMs;
    if (int(args.once) + int(args.read) + int(args.watch) + int(args.queryList) + int(args.queryGet) > 1 ||
        (args.queryList && args.indexFolder.empty()) || (args.queryGet && args.indexKey.empty())) {
        std::cerr << "Choose one mode and provide a nonempty index query\n"; return 2;
    }
    if ((args.read || args.watch || args.queryList || args.queryGet) && args.root.empty()) {
        const auto selected = iiSocietyHelper::Monitoring::readSelection(args.selection);
        if (!selected) { std::cerr << selected.error() << '\n'; return 1; }
        args.root = selected->root; args.identifier = selected->containerId;
    }
    if (args.queryList || args.queryGet) {
        const auto state = iiSocietyHelper::Monitoring::read(args.root, args.identifier);
        if (!state) { std::cerr << state.error() << '\n'; return 1; }
        if (args.queryList) {
            const auto entries = iiSocietyContainer::InventoryIndex::children(args.root, state->containerId, path(args.indexFolder));
            if (!entries) { std::cerr << entries.error() << '\n'; return 1; }
            std::cout << '[';
            bool comma = false;
            for (const auto &entry : *entries) { if (comma) std::cout << ','; comma = true; std::cout << json(entry); }
            std::cout << "]\n";
        } else {
            const auto entry = iiSocietyContainer::InventoryIndex::find(args.root, state->containerId, args.indexKey);
            if (!entry) { std::cerr << entry.error() << '\n'; return 1; }
            std::cout << (*entry ? json(**entry) : "null") << '\n';
        }
        return 0;
    }
    if (args.read) {
        const auto value = iiSocietyHelper::Monitoring::read(args.root, args.identifier);
        if (!value) { std::cerr << value.error() << '\n'; return 1; }
        std::cout << iiSocietyHelper::Monitoring::toJson(*value) << '\n'; return 0;
    }
    std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
    const auto startedAt = std::chrono::steady_clock::now();
    auto done = [&] { return interrupted || (args.exitAfterMs && std::chrono::steady_clock::now()-startedAt >= std::chrono::milliseconds(args.exitAfterMs)); };
    if (args.watch) {
        iiSocietyHelper::Monitoring::Subscription subscription;
        const auto started = subscription.start(args.root, args.identifier, [](const auto& state) {
            std::cout << iiSocietyHelper::Monitoring::toJson(state) << std::endl;
        }, std::chrono::milliseconds(args.intervalMs));
        if (!started) { std::cerr << started.error() << '\n'; return 1; }
        while (!done()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 0;
    }
    iiSocietyHelper::Monitoring::Publisher publisher;
    std::string lastError;
    bool active = false;
    std::filesystem::path currentRoot;
    std::string currentId;
    auto nextScan = std::chrono::steady_clock::time_point::min();
    while (!done()) {
        auto selection = args.root.empty() ? iiSocietyHelper::Monitoring::readSelection(args.selection)
            : iiSocietyHelper::Monitoring::Result<iiSocietyHelper::Monitoring::Selection>({args.root, args.identifier});
        if (!selection || selection->root != currentRoot || selection->containerId != currentId) {
            publisher.stop(); active = false; currentRoot.clear(); currentId.clear();
            nextScan = std::chrono::steady_clock::time_point::min();
        }
        std::string error;
        if (!selection) error = selection.error();
        else {
            if (!active) {
                const auto started = publisher.start(selection->root, selection->containerId, args.limits);
                if (started) {
                    active = true; currentRoot = selection->root; currentId = selection->containerId;
                    nextScan = std::chrono::steady_clock::time_point::min();
                }
                else error = started.error();
            }
            if (active) {
                bool scanned = false;
                if (args.once || std::chrono::steady_clock::now() >= nextScan) {
                    const auto changed = publisher.poll();
                    if (!changed) { error = changed.error(); publisher.stop(); active = false; }
                    else {
                        scanned = true;
                        nextScan = std::chrono::steady_clock::now() + std::chrono::milliseconds(args.scanIntervalMs);
                        if (*changed) std::cout << iiSocietyHelper::Monitoring::toJson(publisher.state()) << std::endl;
                    }
                } else {
                    const auto heartbeat = publisher.heartbeat();
                    if (!heartbeat) { error = heartbeat.error(); publisher.stop(); active = false; }
                }
                if (scanned && args.once) {
                    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::minutes(5));
                    if (args.exitAfterMs) {
                        const auto deadline = startedAt + std::chrono::milliseconds(args.exitAfterMs);
                        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                        timeout = std::min(timeout, std::max(remaining, std::chrono::milliseconds::zero()));
                    }
                    const auto indexed = publisher.waitForIndex(timeout);
                    if (!indexed) { std::cerr << indexed.error() << '\n'; publisher.stop(); return 1; }
                }
            }
        }
        if (error != lastError) {
            if (!error.empty()) std::cerr << error << '\n';
            lastError = error;
        }
        if (args.once) return error.empty() ? 0 : 1;
        // Bounded waits make service shutdown responsive even with slow scans.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(args.intervalMs);
        while (!done() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    publisher.stop();
    return 0;
}
