#include <iiFileProviderStorage.h>
#include "Monitor.h"
#include <DirectoryStorage.h>
#include <LogicalDisk.h>
#include <FileTree.h>
#include <iiFileProviderCoreIO.h>
#include <iiSDKCore/Core/Serialization/Serialization.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <tuple>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace iiSocietyHelper::Monitoring {
namespace {
namespace fs = std::filesystem;
constexpr std::array<std::pair<const char*, const char*>, 9> areas{{
    {"files", "Files"}, {"photos", "Photos"}, {"asset-library", "Asset Library"},
    {"generation-history", "Generation History"}, {"models", "Models"},
    {"thinking-space", "Thinking Space"}, {"forked", "Forked"},
    {"published", "Published"}, {"deleted", "Deleted"}
}};
std::string utf8(const fs::path& value) {
    const auto encoded = value.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}
fs::path path(std::string_view value) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string instance() {
    std::random_device random;
    std::ostringstream out;
    for (int i = 0; i < 4; ++i) out << std::hex << std::setw(8) << std::setfill('0') << random();
    return out.str();
}
bool uuid(std::string_view value) {
    if (value.size() != 36) return false;
    bool nonzero = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
        else {
            const auto c = value[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
            nonzero |= c != '0';
        }
    }
    return nonzero;
}
// Never resolve a redirected ancestor or follow a runtime/section symlink.
bool direct(const fs::path& value, bool directory) {
    if (!value.is_absolute() || value.lexically_normal() != value ||
        value.native().find(fs::path::value_type{}) != fs::path::string_type::npos) return false;
    fs::path prefix = value.root_path();
    std::error_code error;
    for (const auto& component : value.relative_path()) {
        prefix /= component;
        const auto status = fs::symlink_status(prefix, error);
        if (error || fs::is_symlink(status) || !fs::exists(status)) return false;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(prefix.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
        if (prefix != value && !fs::is_directory(status)) return false;
        if (prefix == value && (directory ? !fs::is_directory(status) : !fs::is_regular_file(status))) return false;
    }
    return true;
}
Result<std::string> bytes(const fs::path& file, std::uintmax_t limit) {
    if (!direct(file, false)) return std::unexpected("File is missing or redirected: " + utf8(file));
    std::error_code error;
    const auto length = fs::file_size(file, error);
    if (error || length > limit) return std::unexpected("File exceeds the read limit: " + utf8(file));
    std::ifstream input(file, std::ios::binary);
    std::string data(static_cast<std::size_t>(length), '\0');
    input.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) return std::unexpected("File changed while reading: " + utf8(file));
    return data;
}

// The existing selection and manifest formats are JSON. Parse their bounded
// documents without a GUI library; retain scalar types and validate the whole
// document, including ignored fields and escaped Unicode paths.
struct Scalar { enum Kind { String, Number, Other } kind = Other; std::string value; };
class Json {
    std::string_view input;
    std::size_t cursor = 0;
    void space() { while (cursor < input.size() && (input[cursor] == ' ' || input[cursor] == '\r' || input[cursor] == '\n' || input[cursor] == '\t')) ++cursor; }
    bool take(char c) { space(); if (cursor < input.size() && input[cursor] == c) { ++cursor; return true; } return false; }
    int hex4() {
        int value = 0;
        for (int i = 0; i < 4; ++i) {
            if (cursor == input.size()) return -1;
            const char c = input[cursor++];
            const int digit = c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : -1;
            if (digit < 0) return -1;
            value = value * 16 + digit;
        }
        return value;
    }
    static void append(std::string& out, unsigned c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xc0 | (c >> 6)); out += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { out += char(0xe0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
        else { out += char(0xf0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 63)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
    }
    bool string(std::string& out) {
        if (!take('"')) return false;
        while (cursor < input.size()) {
            const unsigned char c = input[cursor++];
            if (c == '"') return true;
            if (c < 32) return false;
            if (c != '\\') { out += char(c); continue; }
            if (cursor == input.size()) return false;
            switch (input[cursor++]) {
            case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
            case 'r': out += '\r'; break; case 't': out += '\t'; break;
            case 'u': {
                int code = hex4(); if (code < 0) return false;
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (cursor + 2 > input.size() || input.substr(cursor, 2) != "\\u") return false;
                    cursor += 2; const int low = hex4();
                    if (low < 0xdc00 || low > 0xdfff) return false;
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) return false;
                append(out, unsigned(code)); break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool number(std::string& out) {
        space(); const auto start = cursor;
        if (cursor < input.size() && input[cursor] == '-') ++cursor;
        if (cursor == input.size()) return false;
        if (input[cursor] == '0') ++cursor;
        else {
            if (input[cursor] < '1' || input[cursor] > '9') return false;
            while (cursor < input.size() && input[cursor] >= '0' && input[cursor] <= '9') ++cursor;
        }
        if (cursor < input.size() && input[cursor] == '.') {
            ++cursor; const auto begin = cursor;
            while (cursor < input.size() && input[cursor] >= '0' && input[cursor] <= '9') ++cursor;
            if (begin == cursor) return false;
        }
        if (cursor < input.size() && (input[cursor] == 'e' || input[cursor] == 'E')) {
            ++cursor; if (cursor < input.size() && (input[cursor] == '+' || input[cursor] == '-')) ++cursor;
            const auto begin = cursor;
            while (cursor < input.size() && input[cursor] >= '0' && input[cursor] <= '9') ++cursor;
            if (begin == cursor) return false;
        }
        out = input.substr(start, cursor-start); return true;
    }
    bool value(Scalar& out, unsigned depth) {
        if (depth > 24) return false;
        space(); if (cursor == input.size()) return false;
        if (input[cursor] == '"') { out.kind = Scalar::String; return string(out.value); }
        if (take('{')) {
            if (take('}')) return true;
            do { std::string key; Scalar ignored; if (!string(key) || !take(':') || !value(ignored, depth+1)) return false; } while (take(','));
            return take('}');
        }
        if (take('[')) {
            if (take(']')) return true;
            do { Scalar ignored; if (!value(ignored, depth+1)) return false; } while (take(','));
            return take(']');
        }
        for (const auto literal : {"true", "false", "null"}) {
            const std::string_view text(literal);
            if (input.substr(cursor, text.size()) == text) { cursor += text.size(); return true; }
        }
        out.kind = Scalar::Number; return number(out.value);
    }
public:
    explicit Json(std::string_view value): input(value) {}
    Result<std::map<std::string, Scalar>> object() {
        if (!ii::serialization::serialize(std::string(input))) return std::unexpected("JSON is not UTF-8");
        std::map<std::string, Scalar> result;
        if (!take('{')) return std::unexpected("Expected a JSON object");
        if (!take('}')) {
            do {
                std::string key; Scalar scalar;
                if (!string(key) || !take(':') || !value(scalar, 0) || !result.emplace(key, scalar).second)
                    return std::unexpected("Invalid JSON object");
            } while (take(','));
            if (!take('}')) return std::unexpected("Invalid JSON object terminator");
        }
        space(); if (cursor != input.size()) return std::unexpected("Trailing JSON data");
        return result;
    }
};
bool field(const std::map<std::string, Scalar>& object, const char* key, Scalar::Kind kind, std::string_view value = {}) {
    const auto it = object.find(key);
    return it != object.end() && it->second.kind == kind && (value.empty() || it->second.value == value);
}
Result<std::string> identity(const fs::path& root, const std::string& expected) {
    if (!direct(root, true) || (!iiSocietyContainer::DirectoryStorage::isPackageDirectory(root) && !iiSocietyContainer::LogicalDisk::owner(root)))
        return std::unexpected("Container is missing, redirected or incomplete");
    const auto manifest = bytes(root/".society-drive.json", 16384);
    if (!manifest) return std::unexpected(manifest.error());
    const auto json = Json(*manifest).object();
    if (!json || !field(*json, "type", Scalar::String, "SocietyDrive") || !field(*json, "schemaVersion", Scalar::Number, "1") || !field(*json, "identifier", Scalar::String))
        return std::unexpected("Invalid Society container manifest");
    const auto id = json->at("identifier").value;
    if (!uuid(id) || (!expected.empty() && expected != id)) return std::unexpected("Container identity mismatch");
    return id;
}
Result<std::pair<std::uint64_t, std::uint64_t>> node(const fs::path& root) {
#ifdef _WIN32
    HANDLE handle = iiFileProvider::native::createFile(root.c_str(), 0, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if (handle == INVALID_HANDLE_VALUE) return std::unexpected("Cannot open container identity");
    const bool okay = GetFileInformationByHandle(handle, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    CloseHandle(handle);
    if (!okay) return std::unexpected("Cannot read container identity");
    return std::pair<std::uint64_t, std::uint64_t>{info.dwVolumeSerialNumber, (std::uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow};
#else
    struct stat info{};
    if (lstat(root.c_str(), &info) || !S_ISDIR(info.st_mode)) return std::unexpected("Cannot read container identity");
    return std::pair<std::uint64_t, std::uint64_t>{info.st_dev, info.st_ino};
#endif
}
using SectionWire = std::tuple<std::string, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::int64_t, bool, bool>;
using Wire = std::tuple<std::uint32_t, std::string, std::string, std::string, std::uint64_t, std::int64_t, bool, bool, bool, std::string, std::vector<SectionWire>, std::vector<std::string>>;
Wire wire(const State& value) {
    std::vector<SectionWire> sections;
    for (const auto& s : value.sections) sections.emplace_back(s.key, s.files, s.directories, s.bytes, s.fingerprint, s.lastModified, s.available, s.complete);
    return {value.schemaVersion, value.instanceId, value.containerId, utf8(value.root), value.revision, value.observedAtMs, value.available, value.complete, value.daemonRunning, value.error, sections, value.changedSections};
}
State state(Wire value) {
    State out;
    std::string root;
    std::vector<SectionWire> sections;
    std::tie(out.schemaVersion, out.instanceId, out.containerId, root, out.revision, out.observedAtMs, out.available, out.complete, out.daemonRunning, out.error, sections, out.changedSections) = std::move(value);
    out.root = path(root);
    for (auto& s : sections) out.sections.push_back(std::apply([](auto&&...fields){return Section{std::forward<decltype(fields)>(fields)...};}, std::move(s)));
    return out;
}
Result<void> write(const State& value) {
    const auto encoded = ii::serialization::serialize(wire(value));
    if (!encoded) return std::unexpected(encoded.error().message);
    const auto target = statePath(value.root);
    if (!direct(target.parent_path(), true)) return std::unexpected("Runtime directory is redirected");
    std::error_code error;
    if (fs::exists(fs::symlink_status(target, error)) && !direct(target, false)) return std::unexpected("Runtime snapshot is redirected");
    auto output = iiFileProvider::core::AtomicFile::create(target);
    if (!output) return std::unexpected("Cannot create atomic state snapshot");
    auto written = output->write(*encoded);
    if (!written || *written != encoded->size()) return std::unexpected("Cannot write complete state snapshot");
    if (!output->commit()) return std::unexpected("Cannot commit state snapshot");
    return {};
}
void hash(Section& section, std::string_view value) {
    for (unsigned char c : value) { section.fingerprint ^= c; section.fingerprint *= 1099511628211ULL; }
    section.fingerprint ^= 0xff; section.fingerprint *= 1099511628211ULL;
}
std::size_t summarize(const iiSocietyContainer::FileTree::Entry& entry, Section& section) {
    hash(section, utf8(entry.relativePath));
    hash(section, std::to_string(entry.size));
#ifdef _MSC_VER
    const auto systemModified = std::chrono::clock_cast<std::chrono::system_clock>(entry.lastModified);
#else
    const auto systemModified = fs::file_time_type::clock::to_sys(entry.lastModified);
#endif
    hash(section, std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
        systemModified.time_since_epoch()).count()));
    hash(section, std::to_string(unsigned(entry.permissions)));
    const auto modified = std::chrono::duration_cast<std::chrono::milliseconds>(
        systemModified.time_since_epoch()).count();
    section.lastModified = std::max(section.lastModified, modified);
    if (entry.kind == iiSocietyContainer::FileTree::Kind::File) { ++section.files; section.bytes += entry.size; }
    else {
        if (entry.relativePath != ".") ++section.directories;
        section.complete &= entry.childrenLoaded;
    }
    std::size_t count = 1;
    for (const auto& child : entry.children) count += summarize(child, section);
    return count;
}
std::string quote(std::string_view value) {
    std::ostringstream out; out << '"';
    for (const unsigned char c : value) {
        switch (c) { case '"': out << "\\\""; break; case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break; case '\r': out << "\\r"; break; case '\t': out << "\\t"; break;
        default: if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec; else out << char(c); }
    }
    out << '"'; return out.str();
}
}
class Publisher::Private {
public:
    State current;
    Options options;
    std::pair<std::uint64_t, std::uint64_t> pinned{};
    bool running = false, initial = true;
#ifdef _WIN32
    HANDLE owner = INVALID_HANDLE_VALUE;
#else
    int owner = -1;
#endif
    void release() {
        running = false;
#ifdef _WIN32
        if (owner != INVALID_HANDLE_VALUE) CloseHandle(owner);
        owner = INVALID_HANDLE_VALUE;
#else
        if (owner >= 0) close(owner);
        owner = -1;
#endif
    }
    bool intact() const {
        return node(current.root) == Result<std::pair<std::uint64_t, std::uint64_t>>(pinned) && bool(identity(current.root, current.containerId));
    }
};
Publisher::Publisher(): d(std::make_unique<Private>()) {}
Publisher::~Publisher() { stop(); }
Result<void> Publisher::start(const fs::path& root, std::string expectedId, Options options) {
    if (d->running) return std::unexpected("Publisher is already running");
    if (!options.maxEntries || options.maxDepth > 256) return std::unexpected("Invalid scan limits");
    const auto id = identity(root, expectedId);
    if (!id) return std::unexpected(id.error());
    const auto pinned = node(root);
    if (!pinned) return std::unexpected(pinned.error());
    fs::path directory = root;
    for (const auto name : {".society-runtime", "monitor", "v1"}) {
        directory /= name;
        std::error_code error;
        if (iiFileProvider::native::createDirectory(directory, error)) fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace, error);
        if (error || !direct(directory, true)) return std::unexpected("Cannot prepare private monitor runtime");
    }
    const auto lock = directory/"owner.lock";
    std::error_code error;
    if (fs::exists(fs::symlink_status(lock, error)) && !direct(lock, false)) return std::unexpected("Monitor lock is redirected");
#ifdef _WIN32
    d->owner = iiFileProvider::native::createFile(lock.c_str(), GENERIC_READ|GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (d->owner == INVALID_HANDLE_VALUE) return std::unexpected("Another publisher owns this container");
#else
    d->owner = iiFileProvider::native::openFile(lock.c_str(), O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC, 0600);
    if (d->owner < 0 || flock(d->owner, LOCK_EX|LOCK_NB)) { d->release(); return std::unexpected("Another publisher owns this container"); }
#endif
    d->current = {};
    d->current.root = root; d->current.containerId = *id; d->current.instanceId = instance();
    d->pinned = *pinned;
    if (!d->intact()) { d->release(); return std::unexpected("Container changed while starting"); }
    const auto previous = read(root, *id);
    if (previous) d->current.revision = previous->revision;
    d->current.daemonRunning = true; d->options = options; d->initial = true; d->running = true;
    return {};
}
Result<bool> Publisher::poll() {
    if (!d->running) return std::unexpected("Publisher is not running");
    if (!d->intact()) { d->current.available = false; d->current.daemonRunning = false; d->release(); return std::unexpected("Container was removed, redirected or replaced"); }
    State next = d->current;
    next.sections.clear(); next.changedSections.clear(); next.error.clear();
    next.available = true; next.complete = true; next.observedAtMs = now();
    // Replicas can change their namespace before a payload becomes resident.
    // Observe catalog metadata only; never decode or copy its object inventory.
    Section catalog; catalog.fingerprint = 14695981039346656037ULL;
    const auto metadata = iiSocietyContainer::FileTree(next.root).entry(".society-sync/catalog.json");
    if (metadata) (void)summarize(*metadata.value, catalog);
    else hash(catalog, std::to_string(metadata.error.value()));
    std::size_t remaining = d->options.maxEntries;
    for (const auto& [key, name] : areas) {
        Section section; section.key = key; section.fingerprint = 14695981039346656037ULL;
        hash(section, std::to_string(catalog.fingerprint));
        const auto directory = next.root/name;
        section.available = direct(directory, true);
        if (section.available && remaining) {
            const auto tree = iiSocietyContainer::FileTree(directory).snapshot({}, {true, d->options.maxDepth, remaining});
            if (tree) {
                const auto used = summarize(*tree.value, section);
                remaining -= std::min(remaining, used);
            } else { section.complete = false; next.error = "An area could not be scanned: " + section.key; }
        } else section.complete = false;
        next.complete &= section.complete;
        const auto previous = std::ranges::find(d->current.sections, section.key, &Section::key);
        if (previous == d->current.sections.end() || *previous != section) next.changedSections.push_back(section.key);
        next.sections.push_back(std::move(section));
    }
    if (!d->intact()) { d->release(); return std::unexpected("Container changed during scan"); }
    const bool changed = d->initial || !next.changedSections.empty() || next.available != d->current.available || next.error != d->current.error;
    if (changed) ++next.revision;
    const auto saved = write(next);
    if (!saved) return std::unexpected(saved.error());
    d->current = std::move(next); d->initial = false;
    return changed;
}
void Publisher::stop() {
    if (!d->running) return;
    d->current.daemonRunning = false; d->current.observedAtMs = now();
    if (d->intact()) (void)write(d->current);
    d->release();
}
const State& Publisher::state() const { return d->current; }
fs::path statePath(const fs::path& root) { return root/".society-runtime/monitor/v1/state.cbor"; }
Result<State> read(const fs::path& root, std::string expectedId, std::chrono::milliseconds staleAfter) {
    const auto id = identity(root, expectedId);
    if (!id) return std::unexpected(id.error());
    const auto pinned = node(root);
    const auto content = bytes(statePath(root), 1024*1024);
    if (!content) return std::unexpected(content.error());
    ii::serialization::Limits limits; limits.max_bytes = 1024*1024; limits.max_blob_bytes = 16384;
    limits.max_container_items = 64; limits.max_items = 256; limits.max_allocation_bytes = 1024*1024;
    const auto encoded = std::as_bytes(std::span(content->data(), content->size()));
    auto decoded = ii::serialization::deserialize<Wire>(encoded, limits);
    if (!decoded) return std::unexpected(decoded.error().message);
    auto result = state(std::move(*decoded));
    if (result.schemaVersion != 1 || result.root != root || result.containerId != *id || result.instanceId.empty() || result.sections.size() != areas.size() || result.observedAtMs <= 0)
        return std::unexpected("Snapshot identity or schema mismatch");
    for (std::size_t i = 0; i < areas.size(); ++i) if (result.sections[i].key != areas[i].first) return std::unexpected("Snapshot section mismatch");
    if (pinned != node(root) || !identity(root, *id)) return std::unexpected("Container changed while reading");
    const auto age = now() - result.observedAtMs;
    if (age > staleAfter.count() || age < -staleAfter.count()) result.daemonRunning = false;
    return result;
}
fs::path defaultSelectionFile() {
    if (const auto selected = std::getenv("SOCIETY_STORAGE_SETTINGS_PATH")) return path(selected);
#ifdef _WIN32
    if (const auto base = std::getenv("LOCALAPPDATA")) return path(base)/"iisacc/Society/storage.json";
#else
    if (const auto home = std::getenv("HOME")) {
#ifdef __APPLE__
        return path(home)/"Library/Preferences/iisacc/Society/storage.json";
#else
        if (const auto config = std::getenv("XDG_CONFIG_HOME")) return path(config)/"iisacc/Society/storage.json";
        return path(home)/".config/iisacc/Society/storage.json";
#endif
    }
#endif
    return {};
}
Result<Selection> readSelection(const fs::path& file) {
    const auto content = bytes(file, 16384);
    if (!content) return std::unexpected(content.error());
    const auto json = Json(*content).object();
    if (!json || !field(*json, "schemaVersion", Scalar::Number, "1") || !field(*json, "containerId", Scalar::String) || !field(*json, "path", Scalar::String))
        return std::unexpected("Invalid Society selection");
    Selection result{path(json->at("path").value), json->at("containerId").value};
    const auto id = identity(result.root, result.containerId);
    if (!id) return std::unexpected(id.error());
    return result;
}
std::string toJson(const State& value) {
    std::ostringstream out;
    out << "{\"schemaVersion\":" << value.schemaVersion << ",\"instanceId\":" << quote(value.instanceId)
        << ",\"containerId\":" << quote(value.containerId) << ",\"rootPath\":" << quote(utf8(value.root))
        << ",\"revision\":" << quote(std::to_string(value.revision)) << ",\"observedAtMs\":" << value.observedAtMs
        << ",\"available\":" << (value.available?"true":"false") << ",\"complete\":" << (value.complete?"true":"false")
        << ",\"daemonRunning\":" << (value.daemonRunning?"true":"false") << ",\"error\":" << quote(value.error) << ",\"sections\":[";
    bool comma = false;
    for (const auto& s : value.sections) {
        if (comma) out << ','; comma = true;
        out << "{\"key\":" << quote(s.key) << ",\"files\":" << s.files << ",\"directories\":" << s.directories
            << ",\"bytes\":" << s.bytes << ",\"fingerprint\":" << quote(std::to_string(s.fingerprint))
            << ",\"lastModified\":" << s.lastModified << ",\"available\":" << (s.available?"true":"false")
            << ",\"complete\":" << (s.complete?"true":"false") << '}';
    }
    out << "],\"changedSections\":["; comma = false;
    for (const auto& key : value.changedSections) { if (comma) out << ','; comma = true; out << quote(key); }
    out << "]}"; return out.str();
}
class Subscription::Private {
public:
    struct Worker { std::mutex mutex; std::condition_variable changed; bool stopped = false; };
    std::shared_ptr<Worker> worker;
    std::thread thread;
};
Subscription::Subscription(): d(std::make_unique<Private>()) {}
Subscription::~Subscription() { stop(); }
Result<void> Subscription::start(const fs::path& root, std::string expectedId, Handler handler, std::chrono::milliseconds interval) {
    stop();
    if (!handler || interval < std::chrono::milliseconds(10) || interval > std::chrono::seconds(60)) return std::unexpected("Invalid subscription options");
    const auto id = identity(root, expectedId);
    if (!id) return std::unexpected(id.error());
    auto worker = std::make_shared<Private::Worker>();
    d->worker = worker;
    d->thread = std::thread([worker, root, id=*id, handler=std::move(handler), interval] {
        std::optional<State> previous;
        for (;;) {
            { std::lock_guard guard(worker->mutex); if (worker->stopped) break; }
            auto current = read(root, id);
            State value;
            if (current) value = std::move(*current);
            else { value.root = root; value.containerId = id; value.error = current.error(); value.complete = false; }
            if (!previous || value.instanceId != previous->instanceId || value.revision != previous->revision || value.error != previous->error || value.daemonRunning != previous->daemonRunning || value.available != previous->available) {
                try { handler(value); } catch (...) { /* A consumer cannot terminate the monitor worker. */ }
                previous = std::move(value);
            }
            std::unique_lock guard(worker->mutex);
            if (worker->changed.wait_for(guard, interval, [&]{return worker->stopped;})) break;
        }
    });
    return {};
}
void Subscription::stop() {
    if (d->worker) {
        { std::lock_guard guard(d->worker->mutex); d->worker->stopped = true; }
        d->worker->changed.notify_all();
    }
    if (d->thread.joinable()) {
        if (d->thread.get_id() == std::this_thread::get_id()) d->thread.detach();
        else d->thread.join();
    }
    d->worker.reset();
}
}
