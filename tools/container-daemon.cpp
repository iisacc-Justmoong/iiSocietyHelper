#include <iiSocietyHelper/Monitoring/Monitor.h>
#include <atomic>
#include <charconv>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
struct Arguments {
    std::filesystem::path root, selection = iiSocietyHelper::Monitoring::defaultSelectionFile();
    std::string identifier;
    int intervalMs = 1000, exitAfterMs = 0;
    iiSocietyHelper::Monitoring::Options limits;
    bool once = false, read = false, watch = false;
};
bool number(std::string_view input, int& out) {
    const auto [end, error] = std::from_chars(input.data(), input.data()+input.size(), out);
    return error == std::errc{} && end == input.data()+input.size();
}
std::filesystem::path path(const std::string& value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
}
int main(int argc, char** argv) {
    Arguments args;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--help") {
            std::cout << "SocietyContainerDaemon [--root PATH --identifier UUID | --selection-file PATH]\n"
                         "  --interval-ms N     scan/heartbeat interval (50..60000, default 1000)\n"
                         "  --max-entries N     metadata budget (default 100000)\n"
                         "  --max-depth N       directory depth (default 64)\n"
                         "  --once              publish once and shut down\n"
                         "  --read-state        read the latest state and exit\n"
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
        else {
            int parsed = 0;
            if (!number(value, parsed)) { std::cerr << "Invalid integer: " << flag << '\n'; return 2; }
            if (flag == "--interval-ms" && parsed >= 50 && parsed <= 60000) args.intervalMs = parsed;
            else if (flag == "--exit-after-ms" && parsed > 0) args.exitAfterMs = parsed;
            else if (flag == "--max-entries" && parsed > 0) args.limits.maxEntries = std::size_t(parsed);
            else if (flag == "--max-depth" && parsed >= 0 && parsed <= 256) args.limits.maxDepth = std::size_t(parsed);
            else { std::cerr << "Unknown option or out-of-range value: " << flag << '\n'; return 2; }
        }
    }
    if ((args.read || args.watch) && args.root.empty()) {
        const auto selected = iiSocietyHelper::Monitoring::readSelection(args.selection);
        if (!selected) { std::cerr << selected.error() << '\n'; return 1; }
        args.root = selected->root; args.identifier = selected->containerId;
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
    while (!done()) {
        auto selection = args.root.empty() ? iiSocietyHelper::Monitoring::readSelection(args.selection)
            : iiSocietyHelper::Monitoring::Result<iiSocietyHelper::Monitoring::Selection>({args.root, args.identifier});
        if (!selection || selection->root != currentRoot || selection->containerId != currentId) {
            publisher.stop(); active = false; currentRoot.clear(); currentId.clear();
        }
        std::string error;
        if (!selection) error = selection.error();
        else {
            if (!active) {
                const auto started = publisher.start(selection->root, selection->containerId, args.limits);
                if (started) { active = true; currentRoot = selection->root; currentId = selection->containerId; }
                else error = started.error();
            }
            if (active) {
                const auto changed = publisher.poll();
                if (!changed) { error = changed.error(); publisher.stop(); active = false; }
                else if (*changed) std::cout << iiSocietyHelper::Monitoring::toJson(publisher.state()) << std::endl;
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
