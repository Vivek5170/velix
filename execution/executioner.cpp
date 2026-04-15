#include "../communication/network_config.hpp"
#include "../communication/socket_wrapper.hpp"
#include "prepare_runner.hpp"
#include "run_launcher.hpp"
#include "runtime_adapters/runtime_adapters.hpp"
#include "../utils/config_utils.hpp"
#include "../utils/logger.hpp"
#include "../utils/process_spawner.hpp"
#include "../utils/thread_pool.hpp"
#include "../vendor/nlohmann/json.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace velix::core {

namespace {

class ExecutionerException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct TransparentStringHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }

    std::size_t operator()(const std::string& value) const noexcept {
        return operator()(std::string_view(value));
    }

    std::size_t operator()(const char* value) const noexcept {
        return operator()(std::string_view(value));
    }
};

struct TransparentStringEqual {
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
        return lhs == rhs;
    }
};

// ---------------------------------------------------------------------------
// SHA-256 — portable, no external deps, standard C++/C only
// Based on the public domain implementation by Brad Conte.
// ---------------------------------------------------------------------------
namespace sha256_impl {

inline constexpr std::array<uint32_t, 64> K = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t ch(uint32_t x,uint32_t y,uint32_t z)  { return (x&y)^(~x&z); }
inline uint32_t maj(uint32_t x,uint32_t y,uint32_t z) { return (x&y)^(x&z)^(y&z); }
inline uint32_t ep0(uint32_t x) { return rotr(x,2)^rotr(x,13)^rotr(x,22); }
inline uint32_t ep1(uint32_t x) { return rotr(x,6)^rotr(x,11)^rotr(x,25); }
inline uint32_t sig0(uint32_t x){ return rotr(x,7)^rotr(x,18)^(x>>3);  }
inline uint32_t sig1(uint32_t x){ return rotr(x,17)^rotr(x,19)^(x>>10); }

struct Ctx {
    std::array<uint8_t, 64> data{};
    uint32_t datalen{0};
    uint64_t bitlen{0};
    std::array<uint32_t, 8> state{
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
};

void transform(Ctx &ctx, const uint8_t *data) {
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    uint32_t t1;
    uint32_t t2;
    std::array<uint32_t, 64> m{};
    for (int i=0,j=0;i<16;++i,j+=4)
        m[i]=(uint32_t(data[j])<<24)|(uint32_t(data[j+1])<<16)|
             (uint32_t(data[j+2])<<8)|uint32_t(data[j+3]);
    for (int i=16;i<64;++i)
        m[i]=sig1(m[i-2])+m[i-7]+sig0(m[i-15])+m[i-16];
    a=ctx.state[0];b=ctx.state[1];c=ctx.state[2];d=ctx.state[3];
    e=ctx.state[4];f=ctx.state[5];g=ctx.state[6];h=ctx.state[7];
    for (int i=0;i<64;++i){
        t1=h+ep1(e)+ch(e,f,g)+K[i]+m[i];
        t2=ep0(a)+maj(a,b,c);
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    ctx.state[0]+=a;ctx.state[1]+=b;ctx.state[2]+=c;ctx.state[3]+=d;
    ctx.state[4]+=e;ctx.state[5]+=f;ctx.state[6]+=g;ctx.state[7]+=h;
}

void update(Ctx &ctx, const uint8_t *data, size_t len) {
    for (size_t i=0;i<len;++i){
        ctx.data[ctx.datalen++]=data[i];
        if (ctx.datalen==64){ transform(ctx,ctx.data.data()); ctx.bitlen+=512; ctx.datalen=0; }
    }
}

std::string finalise(Ctx &ctx) {
    uint32_t i=ctx.datalen;
    ctx.data[i++]=0x80;
    if (ctx.datalen<56){
        while(i<56) {
            ctx.data[i++]=0x00;
        }
    }
    else {
        while(i<64) {
            ctx.data[i++]=0x00;
        }
        transform(ctx,ctx.data.data());
        std::fill(ctx.data.begin(), ctx.data.begin() + 56, 0);
    }
    ctx.bitlen+=uint64_t(ctx.datalen)*8;
    ctx.data[63]=uint8_t(ctx.bitlen);
    ctx.data[62]=uint8_t(ctx.bitlen>>8);
    ctx.data[61]=uint8_t(ctx.bitlen>>16);
    ctx.data[60]=uint8_t(ctx.bitlen>>24);
    ctx.data[59]=uint8_t(ctx.bitlen>>32);
    ctx.data[58]=uint8_t(ctx.bitlen>>40);
    ctx.data[57]=uint8_t(ctx.bitlen>>48);
    ctx.data[56]=uint8_t(ctx.bitlen>>56);
    transform(ctx,ctx.data.data());
    std::array<uint8_t, 32> hash{};
    for (int j=0;j<4;++j){
        hash[j]    =uint8_t(ctx.state[0]>>(24-j*8));
        hash[j+4]  =uint8_t(ctx.state[1]>>(24-j*8));
        hash[j+8]  =uint8_t(ctx.state[2]>>(24-j*8));
        hash[j+12] =uint8_t(ctx.state[3]>>(24-j*8));
        hash[j+16] =uint8_t(ctx.state[4]>>(24-j*8));
        hash[j+20] =uint8_t(ctx.state[5]>>(24-j*8));
        hash[j+24] =uint8_t(ctx.state[6]>>(24-j*8));
        hash[j+28] =uint8_t(ctx.state[7]>>(24-j*8));
    }
    std::ostringstream oss;
    for (const auto byte : hash)
        oss<<std::hex<<std::setw(2)<<std::setfill('0')<<int(byte);
    return oss.str();
}

} // namespace sha256_impl

std::string sha256(std::string_view text) {
    sha256_impl::Ctx ctx;
    sha256_impl::update(ctx,
        reinterpret_cast<const uint8_t*>(text.data()), text.size());
    return sha256_impl::finalise(ctx);
}

// ---------------------------------------------------------------------------
// Cross-platform file lock
// ---------------------------------------------------------------------------
class FileLockGuard {
public:
    explicit FileLockGuard(const fs::path &lock_file,
                           int wait_ms   = 30000,
                           int retry_ms  = 50) {
#ifndef _WIN32
        fd_ = ::open(lock_file.string().c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
        if (fd_ < 0)
            throw ExecutionerException("lock open failed: " + lock_file.string());
        // Enforce owner-only permissions even when opening an existing lock file.
        if (::fchmod(fd_, S_IRUSR | S_IWUSR) != 0) {
            ::close(fd_); fd_ = -1;
            throw ExecutionerException("lock chmod failed: " + lock_file.string());
        }
        const int attempts = std::max(1, wait_ms / std::max(1, retry_ms));
        for (int i = 0; i < attempts; ++i) {
            if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) return;
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                ::close(fd_); fd_ = -1;
                throw ExecutionerException("lock acquire failed: " + lock_file.string());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(retry_ms));
        }
        ::close(fd_); fd_ = -1;
        throw ExecutionerException("lock timeout: " + lock_file.string());
#else
        handle_ = CreateFileA(lock_file.string().c_str(),
                              GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const int attempts = std::max(1, wait_ms / std::max(1, retry_ms));
            for (int i = 0; i < attempts; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(retry_ms));
                handle_ = CreateFileA(lock_file.string().c_str(),
                                      GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (handle_ != INVALID_HANDLE_VALUE) return;
            }
            throw ExecutionerException("lock timeout: " + lock_file.string());
        }
#endif
    }

    ~FileLockGuard() {
#ifndef _WIN32
        if (fd_ >= 0) { ::flock(fd_, LOCK_UN); ::close(fd_); }
#else
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#endif
    }

    FileLockGuard(const FileLockGuard&) = delete;
    FileLockGuard& operator=(const FileLockGuard&) = delete;

private:
#ifndef _WIN32
    int fd_{-1};
#else
    HANDLE handle_{INVALID_HANDLE_VALUE};
#endif
};

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
struct ExecutionerConfig {
    int         socket_timeout_ms   = 5000;
    int         max_client_threads  = 128;
    int         max_queue_size      = 1024;
    int         prepare_timeout_ms  = 300000;
    int         cache_lock_wait_ms  = 310000;  // must exceed prepare_timeout_ms
    int         run_timeout_ms      = 0;
    std::string cache_root          = ".velix/build_cache";
};

ExecutionerConfig load_config() {
    ExecutionerConfig cfg;
    for (const char *p : {"config/executioner.json",
                          "../config/executioner.json",
                          "build/config/executioner.json"}) {
        std::ifstream f(p);
        if (!f.is_open()) continue;
        try {
            json c; f >> c;
            cfg.socket_timeout_ms  = c.value("socket_timeout_ms",  cfg.socket_timeout_ms);
            cfg.max_client_threads = c.value("max_client_threads",  cfg.max_client_threads);
            cfg.max_queue_size     = c.value("max_queue_size",       cfg.max_queue_size);
            cfg.prepare_timeout_ms = c.value("prepare_timeout_ms",  cfg.prepare_timeout_ms);
            cfg.cache_lock_wait_ms = c.value("cache_lock_wait_ms",  cfg.cache_lock_wait_ms);
            cfg.run_timeout_ms     = c.value("run_timeout_ms",      cfg.run_timeout_ms);
            cfg.cache_root         = c.value("cache_root",           cfg.cache_root);
            // Enforce invariant: lock wait must cover worst-case prepare
            if (cfg.cache_lock_wait_ms <= cfg.prepare_timeout_ms)
                cfg.cache_lock_wait_ms = cfg.prepare_timeout_ms + 10000;
        } catch (const std::exception &e) {
            LOG_WARN("Failed to parse " + std::string(p) + ": " + e.what());
        }
        break;
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// PrepareCoalescer — collapses N concurrent prepares of the same key into 1
// ---------------------------------------------------------------------------
struct PrepareSlot {
    std::mutex              mu;
    std::condition_variable cv;
    enum class State { preparing, ready, failed } state{State::preparing};
    std::string             error;
};

class PrepareCoalescer {
public:
    // Returns {slot, is_leader}.
    // Leader must call finish() or fail() when done.
    // Followers must call wait().
    std::pair<std::shared_ptr<PrepareSlot>, bool>
    enter(const std::string &key) {
        std::scoped_lock g(mu_);
        auto it = map_.find(key);
        if (it != map_.end())
            return {it->second, false};
        auto slot = std::make_shared<PrepareSlot>();
        map_[key] = slot;
        return {slot, true};
    }

    void finish(const std::string &key, std::shared_ptr<PrepareSlot> slot) {
        {
            std::scoped_lock g(slot->mu);
            slot->state = PrepareSlot::State::ready;
        }
        slot->cv.notify_all();
        erase(key);
    }

    void fail(const std::string &key, std::shared_ptr<PrepareSlot> slot,
              const std::string &error) {
        {
            std::scoped_lock g(slot->mu);
            slot->state = PrepareSlot::State::failed;
            slot->error = error;
        }
        slot->cv.notify_all();
        erase(key);
    }

    // Returns true if ready, false if failed (sets error).
    bool wait(std::shared_ptr<PrepareSlot> slot, const ExecutionerConfig &config, std::string &error) {
        std::unique_lock<std::mutex> lk(slot->mu);
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(config.prepare_timeout_ms + 10000);
        const bool signaled = slot->cv.wait_until(lk, deadline, [&]{
            return slot->state != PrepareSlot::State::preparing;
        });
        if (!signaled) {
            error = "prepare_peer_timeout";
            return false;
        }
        if (slot->state == PrepareSlot::State::failed) {
            error = slot->error;
            return false;
        }
        return true;
    }

private:
    void erase(const std::string &key) {
        std::scoped_lock g(mu_);
        map_.erase(key);
    }

    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<PrepareSlot>,
                       TransparentStringHash, TransparentStringEqual> map_;
};

// ---------------------------------------------------------------------------
// Package cache — readers/writer lock for high-concurrency reads
// ---------------------------------------------------------------------------
struct CachedPackage {
    std::string path;
    json        manifest;
    std::string cache_key;          // pre-computed, invalidated on force_refresh
};

class PackageCache {
public:
    // Returns empty string if not found.
    bool get(const std::string &name, CachedPackage &out) const {
        std::shared_lock<std::shared_mutex> lk(mu_);
        auto it = map_.find(name);
        if (it == map_.end()) return false;
        out = it->second;
        return true;
    }

    void put(const std::string &name, CachedPackage pkg) {
        std::unique_lock<std::shared_mutex> lk(mu_);
        map_[name] = std::move(pkg);
    }

    void erase(const std::string &name) {
        std::unique_lock<std::shared_mutex> lk(mu_);
        map_.erase(name);
    }

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, CachedPackage,
                       TransparentStringHash, TransparentStringEqual> map_;
};

// ---------------------------------------------------------------------------
// ExecutionerService
// ---------------------------------------------------------------------------
class ExecutionerService {
public:
    ExecutionerService()
        : running_(false),
          config_(load_config()),
          thread_pool_(config_.max_client_threads, config_.max_queue_size) {}

    ~ExecutionerService() { stop(); }

    void start(int port) {
        if (running_.exchange(true)) return;

        try {
            const std::string host =
                velix::communication::resolve_bind_host("EXECUTIONER", "127.0.0.1");
            {
                std::scoped_lock lk(server_mu_);
                server_socket_ = std::make_unique<velix::communication::SocketWrapper>();
                server_socket_->create_tcp_socket();
                server_socket_->bind(host, static_cast<uint16_t>(port));
                server_socket_->listen(256);
            }
            LOG_INFO("Executioner listening on " + host + ":" + std::to_string(port));

            while (running_) {
#ifndef _WIN32
                reap_children();
#endif
                std::shared_ptr<velix::communication::SocketWrapper> srv;
                {
                    std::scoped_lock lk(server_mu_);
                    if (!server_socket_ || !server_socket_->is_open()) break;
                    // non-owning view — safe because stop() waits for accept loop to exit
                    srv = std::shared_ptr<velix::communication::SocketWrapper>(
                        server_socket_.get(),
                        [](velix::communication::SocketWrapper*){});
                }

                if (!srv->has_data(250)) continue;

                velix::communication::SocketWrapper client = srv->accept();
                if (config_.socket_timeout_ms > 0)
                    client.set_timeout_ms(config_.socket_timeout_ms);

                auto cp = std::make_shared<velix::communication::SocketWrapper>(
                    std::move(client));

                const bool ok = thread_pool_.try_submit([this, cp]() mutable {
                    handle_client(std::move(*cp));
                });

                if (!ok) {
                    LOG_WARN("Executioner: thread pool saturated, rejecting request");
                    try {
                        // Send busy response from a detached thread so accept loop
                        // is never blocked by a slow client.
                        std::thread([cp]() mutable {
                            try {
                                const json r = {{"status","error"},
                                    {"error","executioner_busy"}};
                                velix::communication::send_json(*cp, r.dump());
                            } catch (const std::exception &) {
                                // Best-effort busy response only.
                            }
                        }).detach();
                    } catch (const std::system_error &) {
                        // Thread creation failure while rejecting under load.
                    }
                }
            }
        } catch (const std::exception &e) {
            const bool was_running = running_.load();
            running_ = false;
            if (was_running) {
                LOG_ERROR("Executioner startup failed: " + std::string(e.what()));
            }
            throw;
        }
    }

    void stop() {
        running_ = false;
        std::scoped_lock lk(server_mu_);
        if (server_socket_ && server_socket_->is_open())
            server_socket_->close();
#ifndef _WIN32
        reap_children();
#endif
    }

private:

#ifndef _WIN32
    void reap_children() {
        int st = 0;
        while (::waitpid(-1, &st, WNOHANG) > 0) {}
    }
#endif

    // ------------------------------------------------------------------
    // Manifest validation
    // ------------------------------------------------------------------
    bool validate_manifest(const std::string &name, const json &manifest,
                           std::string &error) {
        if (!manifest.contains("name") || !manifest["name"].is_string()) {
            error = "manifest_missing_name"; return false; }
        if (manifest["name"].get<std::string>() != name) {
            error = "manifest_name_mismatch"; return false; }
        if (!manifest.contains("runtime") || !manifest["runtime"].is_string()) {
            error = "manifest_missing_runtime"; return false; }
        if (manifest.contains("prepare") && !manifest["prepare"].is_array()) {
            error = "manifest_prepare_must_be_array"; return false; }
        if (manifest.contains("workdir") && !manifest["workdir"].is_string()) {
            error = "manifest_workdir_must_be_string"; return false; }
        if (manifest.contains("entry") && !manifest["entry"].is_string()) {
            error = "manifest_entry_must_be_string"; return false; }
        if (manifest.contains("run")) {
            if (!manifest["run"].is_object()) {
                error = "manifest_run_must_be_object"; return false; }
            if (manifest["run"].contains("command") &&
                !manifest["run"]["command"].is_string()) {
                error = "manifest_run_command_must_be_string"; return false; }
            if (manifest["run"].contains("args") &&
                !manifest["run"]["args"].is_array()) {
                error = "manifest_run_args_must_be_array"; return false; }
        }
        if (!manifest.contains("run") && !manifest.contains("entry")) {
            error = "manifest_requires_run_or_entry"; return false; }
        return true;
    }

    // ------------------------------------------------------------------
    // Param validation
    // ------------------------------------------------------------------
    bool validate_params(const json &params, const json &schema,
                         std::string &error) {
        if (!schema.is_object()) return true;
        const json props    = schema.value("properties", schema);
        const json required = schema.value("required", json::array());
        for (const auto &rk : required) {
            if (!rk.is_string()) continue;
            if (!params.contains(rk.get<std::string>())) {
                error = "missing_required_param: " + rk.get<std::string>();
                return false;
            }
        }
        for (auto it = props.begin(); it != props.end(); ++it) {
            if (!params.contains(it.key()) || !it.value().is_object()) continue;
            const std::string t = it.value().value("type", "string");
            const json &v = params[it.key()];
            bool ok = (t=="string"  && v.is_string())  ||
                      (t=="number"  && v.is_number())   ||
                      (t=="boolean" && v.is_boolean())  ||
                      (t=="array"   && v.is_array())    ||
                      (t=="object"  && v.is_object())   ||
                      (t!="string"  && t!="number" && t!="boolean"
                                    && t!="array"  && t!="object");
            if (!ok) {
                error = "param_type_mismatch: " + it.key() + " expected " + t;
                return false;
            }
        }
        return true;
    }

    // ------------------------------------------------------------------
    // Cache key — pure in-process SHA-256, no temp files, no subprocesses
    // ------------------------------------------------------------------
    std::string compute_cache_key(const json &manifest,
                                  const runtime_adapters::RuntimeResolution &rt,
                                  const std::string &pkg_path) {
        std::vector<fs::path> files;
        std::error_code ec;
        const fs::path velix_dir = fs::path(".velix");
        for (auto const &e : fs::recursive_directory_iterator(pkg_path, ec)) {
            if (ec) { ec.clear(); continue; }
            const auto &p = e.path();
            if (!fs::is_regular_file(p, ec) || ec) { ec.clear(); continue; }
            bool in_velix = false;
            for (auto it = p.begin(); it != p.end(); ++it) {
                if (*it == velix_dir) { in_velix = true; break; }
            }
            if (in_velix) continue;
            files.push_back(p);
        }
        std::sort(files.begin(), files.end());

        // Stream everything into one SHA-256 context — no giant string in memory
        sha256_impl::Ctx ctx;
        const auto feed = [&](const std::string &s) {
            sha256_impl::update(ctx,
                reinterpret_cast<const uint8_t*>(s.data()), s.size());
        };
        feed(manifest.dump());
        feed("\n");
        feed(rt.runtime);
        feed("\n");
        feed(rt.version);
        feed("\n");
        for (const auto &p : files) {
            feed(p.lexically_relative(pkg_path).string());
            feed("\n");
            std::ifstream in(p, std::ios::binary);
            std::array<char, 65536> buf{};
            while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
                sha256_impl::update(ctx,
                    reinterpret_cast<const uint8_t*>(buf.data()),
                    static_cast<size_t>(in.gcount()));
            }
            feed("\n");
        }
        return sha256_impl::finalise(ctx);
    }

    // ------------------------------------------------------------------
    // Package resolution — shared_mutex for hot read path
    // ------------------------------------------------------------------
    bool resolve_package(const std::string &name, bool force_refresh,
                         CachedPackage &out) {
        if (name.empty()) return false;

        if (!force_refresh) {
            if (pkg_cache_.get(name, out)) return true;
        }

        // Disk scan (no lock held)
        std::string pkg_path;
        for (const char *prefix : {"skills/", "agents/"}) {
            const std::string candidate = std::string(prefix) + name;
            if (std::ifstream(candidate + "/manifest.json").good()) {
                pkg_path = candidate;
                break;
            }
        }
        if (pkg_path.empty()) return false;

        json manifest;
        try {
            std::ifstream f(pkg_path + "/manifest.json");
            f >> manifest;
        } catch (const std::exception &e) {
            LOG_ERROR("Failed to read manifest for " + name + ": " + e.what());
            return false;
        }

        std::string err;
        if (!validate_manifest(name, manifest, err)) {
            LOG_ERROR("Invalid manifest for " + name + ": " + err);
            return false;
        }

        CachedPackage pkg{pkg_path, manifest, ""};
        runtime_adapters::RuntimeResolution runtime;
        std::string rerr;
        if (!runtime_adapters::select_runtime_adapter(
                manifest, pkg_path, pkg_path, runtime, rerr)) {
            LOG_ERROR("Failed to select runtime adapter: " + rerr);
            return false;
        }

        if (force_refresh || pkg.cache_key.empty()) {
            pkg.cache_key = compute_cache_key(pkg.manifest, runtime, pkg.path);
        }
        pkg_cache_.put(name, pkg);
        out = std::move(pkg);
        return true;
    }

    // ------------------------------------------------------------------
    // Cache management
    // ------------------------------------------------------------------
    json handle_cache_clean(const std::string &trace_id) {
        try {
            std::error_code ec;
            if (fs::exists(config_.cache_root, ec))
                fs::remove_all(config_.cache_root, ec);
            fs::create_directories(config_.cache_root, ec);
            return {{"status","ok"},{"trace_id",trace_id},
                    {"phase","cache"},{"action","clean"}};
        } catch (const std::exception &e) {
            return {{"status","error"},{"trace_id",trace_id},
                    {"error", std::string("cache_clean_failed: ") + e.what()}};
        }
    }

    json handle_cache_prune(const json &req, const std::string &trace_id) {
        const int older_sec = req.value("older_than_sec", 7*24*3600);
        int removed = 0;
        try {
            std::error_code ec;
            if (!fs::exists(config_.cache_root, ec))
                return {{"status","ok"},{"trace_id",trace_id},
                        {"phase","cache"},{"action","prune"},{"removed",0}};
            const auto now = fs::file_time_type::clock::now();
            for (const auto &e : fs::directory_iterator(config_.cache_root, ec)) {
                if (!e.is_directory()) continue;
                const auto age = now - fs::last_write_time(e.path(), ec);
                if (ec) { ec.clear(); continue; }
                if (age > std::chrono::seconds(older_sec)) {
                    fs::remove_all(e.path(), ec);
                    if (!ec) ++removed;
                }
            }
            return {{"status","ok"},{"trace_id",trace_id},
                    {"phase","cache"},{"action","prune"},{"removed",removed}};
        } catch (const std::exception &e) {
            return {{"status","error"},{"trace_id",trace_id},
                    {"removed",removed},
                    {"error", std::string("cache_prune_failed: ") + e.what()}};
        }
    }

    // ------------------------------------------------------------------
    // Core exec handler
    // ------------------------------------------------------------------
    json handle_exec_request(const json &req) {
        const std::string msg_type = req.value("message_type", "");
        const std::string trace_id = req.value("trace_id", "");

        if (msg_type == "EXEC_CACHE_CLEAN") return handle_cache_clean(trace_id);
        if (msg_type == "EXEC_CACHE_PRUNE") return handle_cache_prune(req, trace_id);

        if (msg_type != "EXEC_VELIX_PROCESS")
            return {{"status","error"},{"trace_id",trace_id},
                    {"error","unsupported_message_type"}};

        // --- intent validation ---
        const std::string intent    = req.value("intent", "JOIN_PARENT_TREE");
        const bool        is_handler= req.value("is_handler", false);
        const bool        force_ref = req.value("force_refresh", false);
        const int         src_pid   = req.value("source_pid", -1);

        if (intent != "JOIN_PARENT_TREE" && intent != "NEW_TREE")
            return {{"status","error"},{"trace_id",trace_id},
                    {"error","invalid_intent"}};
        if (intent == "JOIN_PARENT_TREE" && src_pid <= 0)
            return {{"status","error"},{"trace_id",trace_id},
                    {"error","invalid_source_pid_for_join_parent_tree"}};
        if (intent == "NEW_TREE" && !is_handler)
            return {{"status","error"},{"trace_id",trace_id},
                    {"error","intent_override_new_tree_allowed_only_for_handler"}};
        if (src_pid <= 0)
            return {{"status","error"},{"trace_id",trace_id},
                    {"error","invalid_source_pid"}};

        const std::string pkg_name = req.value("name", "");
        if (pkg_name.empty())
            return {{"status","error"},{"trace_id",trace_id},
                    {"error","empty_package_name"}};

        const json params = req.value("params", json::object());

        // 1. Resolve package (readers-writer cached, no mutex held after)
        CachedPackage pkg;
        if (!resolve_package(pkg_name, force_ref, pkg))
            return {{"status","error"},{"trace_id",trace_id},
                    {"error","package_not_found_or_invalid: " + pkg_name}};

        // 2. Param validation
        std::string perr;
        if (!validate_params(params, pkg.manifest.value("parameters", json::object()), perr))
            return {{"status","error"},{"trace_id",trace_id},
                    {"error","params_validation_failed: " + perr}};

        // 3. Workdir
        const std::string rel_wd = pkg.manifest.value("workdir", std::string("."));
        const std::string workdir =
            (fs::path(pkg.path) / rel_wd).lexically_normal().string();
        std::error_code ec;
        if (!fs::is_directory(workdir, ec))
            return {{"status","error"},{"trace_id",trace_id},
                    {"phase","prepare"},{"error","workdir_not_found: " + workdir}};

        // 4. Runtime adapter
        runtime_adapters::RuntimeResolution runtime;
        std::string rerr;
        if (!runtime_adapters::select_runtime_adapter(
                pkg.manifest, pkg.path, workdir, runtime, rerr))
            return {{"status","error"},{"trace_id",trace_id},
                    {"phase","adapter"},{"error",rerr}};

        // 5. Parse prepare steps
        std::vector<prepare_runner::PrepareStep> steps;
        std::string perr2;
        if (!prepare_runner::parse_prepare_steps(
                pkg.manifest, config_.prepare_timeout_ms, steps, perr2))
            return {{"status","error"},{"trace_id",trace_id},
                    {"phase","prepare"},{"error",perr2}};

        if (!runtime.injected_prepare_steps.empty()) {
            std::vector<prepare_runner::PrepareStep> merged;
            merged.reserve(runtime.injected_prepare_steps.size() + steps.size());
            for (const auto &s : runtime.injected_prepare_steps)
                merged.push_back({s.command, s.args, s.timeout_ms});
            for (const auto &s : steps)
                merged.push_back(s);
            steps.swap(merged);
        }

        // 6. Environment
#ifdef _WIN32
        const char py_sep = ';';
#else
        const char py_sep = ':';
#endif
        fs::path workspace_root = fs::current_path();
        {
            const fs::path pkg_p(pkg.path);
            const fs::path cand = pkg_p.parent_path().parent_path();
            if (!cand.empty()) workspace_root = fs::absolute(cand);
        }
        std::string pythonpath = workspace_root.string();
        if (const char *ep = std::getenv("PYTHONPATH"); ep && *ep) {
            pythonpath += py_sep;
            pythonpath += ep;
        }

        const std::string tree_id   = req.value("tree_id", "");
        const bool join_tree        = (intent == "JOIN_PARENT_TREE");
        std::map<std::string,std::string> env = {
            {"VELIX_PARENT_PID",    std::to_string(src_pid)},
            {"VELIX_BUS_PORT",      std::to_string(velix::utils::get_port("BUS", 5174))},
            {"VELIX_INTENT",        intent},
            {"VELIX_TRACE_ID",      trace_id},
            {"VELIX_PROCESS_NAME",  pkg_name},
            {"VELIX_TREE_ID",       join_tree ? tree_id : ""},
            {"VELIX_PARAMS",        params.dump()},
            {"VELIX_USER_ID",       req.value("user_id", "")},
            {"PYTHONPATH",          pythonpath},
        };
        for (const auto &[k,v] : runtime.env_overrides) env[k] = v;

        // 7. Compute cache key — pure in-process, thread-safe, no temp files
        if (pkg.cache_key.empty()) {
            pkg.cache_key = compute_cache_key(pkg.manifest, runtime, pkg.path);
            // write back the computed key so future calls skip recomputation
            CachedPackage updated = pkg;
            pkg_cache_.put(pkg_name, updated);
        }
        const std::string &cache_key = pkg.cache_key;

        const fs::path cache_dir   = fs::path(config_.cache_root) / cache_key;
        const fs::path lock_file   = cache_dir / ".lock";
        const fs::path status_file = cache_dir / "status.json";
        const fs::path logs_dir    = cache_dir / "logs";
        const fs::path prepare_log = logs_dir  / "prepare.log";

        // 8. Prepare — coalesced: only one thread prepares per cache key,
        //    all others wait on a condition_variable (no thread slot wasted spinning)
        auto [slot, is_leader] = coalescer_.enter(cache_key);

        if (is_leader) {
            std::string fail_reason;
            try {
                fs::create_directories(cache_dir, ec);
                fs::create_directories(logs_dir,  ec);

                bool cache_ready = false;

                if (!force_ref) {
                    // Cross-process lock only for the status file read/write
                    // (fast — microseconds, not minutes)
                    try {
                        FileLockGuard fl(lock_file, config_.cache_lock_wait_ms);
                        if (fs::exists(status_file, ec)) {
                            try {
                                std::ifstream in(status_file);
                                json st; in >> st;
                                const std::string sv = st.value("status","");
                                if (sv == "ready") {
                                    cache_ready = true;
                                } else {
                                    // "preparing" = previous crash, "failed" = last attempt failed
                                    // Either way: delete and re-prepare
                                    fs::remove(status_file, ec);
                                    LOG_WARN("Cache stale state '" + sv +
                                             "', re-preparing: " + cache_dir.string());
                                }
                            } catch (const json::exception &) {
                                fs::remove(status_file, ec);
                                LOG_WARN("Corrupt status file, re-preparing: " +
                                         status_file.string());
                            }
                        }
                    } catch (const std::exception &e) {
                        fail_reason = std::string("status_lock_failed: ") + e.what();
                        coalescer_.fail(cache_key, slot, fail_reason);
                        return {{"status","error"},{"trace_id",trace_id},
                                {"phase","prepare"},{"error",fail_reason}};
                    }
                } else {
                    // force_refresh: nuke old cache entirely
                    fs::remove_all(cache_dir, ec);
                    fs::create_directories(cache_dir, ec);
                    fs::create_directories(logs_dir,  ec);
                }

                if (!cache_ready) {
                    // Write "preparing" under lock
                    try {
                        FileLockGuard fl(lock_file, config_.cache_lock_wait_ms);
                        std::ofstream out(status_file);
                        out << json({{"status","preparing"}}).dump(2);
                    } catch (const std::exception &e) {
                        fail_reason = std::string("status_write_failed: ") + e.what();
                        coalescer_.fail(cache_key, slot, fail_reason);
                        return {{"status","error"},{"trace_id",trace_id},
                                {"phase","prepare"},{"error",fail_reason}};
                    }

                    // Run prepare — this is the slow part; no locks held
                    const json prep = prepare_runner::execute_prepare(
                        steps, env, workdir, trace_id, prepare_log.string());

                    if (prep.value("status","error") != "ok") {
                        fail_reason = prep.value("error","prepare_failed");
                        if (prep.contains("command")) {
                            fail_reason += " command=" + prep.value("command", std::string(""));
                        }
                        if (prep.contains("exit_code")) {
                            fail_reason += " exit_code=" +
                                           std::to_string(prep.value("exit_code", -1));
                        }
                        if (prep.value("timed_out", false)) {
                            fail_reason += " timed_out=true";
                        }
                        try {
                            FileLockGuard fl(lock_file, config_.cache_lock_wait_ms);
                            std::ofstream out(status_file);
                            out << json({{"status","failed"},
                                         {"error",fail_reason},
                                         {"log_file",prepare_log.string()}}).dump(2);
                        } catch (const std::exception &) {
                            // Keep original prepare failure as primary signal.
                        }
                        coalescer_.fail(cache_key, slot, fail_reason);
                        return prep;
                    }

                    // Write "ready" under lock
                    try {
                        FileLockGuard fl(lock_file, config_.cache_lock_wait_ms);
                        std::ofstream out(status_file);
                        out << json({{"status","ready"}}).dump(2);
                    } catch (const std::exception &) {
                        // Non-fatal: cache will re-prepare next time
                        LOG_WARN("Failed to write ready status: " + status_file.string());
                    }
                }

                coalescer_.finish(cache_key, slot);

            } catch (const std::exception &e) {
                fail_reason = e.what();
                coalescer_.fail(cache_key, slot, fail_reason);
                return {{"status","error"},{"trace_id",trace_id},
                        {"phase","prepare"},
                        {"error", std::string("prepare_cache_failed: ") + fail_reason}};
            }
        } else {
            // Follower — yields CPU entirely until leader signals
            std::string wait_err;
            if (!coalescer_.wait(slot, config_, wait_err))
                return {{"status","error"},{"trace_id",trace_id},
                        {"phase","prepare"},
                        {"error","prepare_failed_by_peer: " + wait_err}};
        }

        // 9. Launch
        LOG_INFO("Launching: " + pkg_name + " [trace=" + trace_id +
                 " wd=" + workdir + "]");
        return run_launcher::launch(runtime, env, workdir, trace_id);
    }

    // ------------------------------------------------------------------
    // Client handler
    // ------------------------------------------------------------------
    void handle_client(velix::communication::SocketWrapper sock) {
        json response;
        try {
            const std::string raw = velix::communication::recv_json(sock);
            if (raw.empty()) return;
            const json req = json::parse(raw);
            response = handle_exec_request(req);
        } catch (const std::exception &e) {
            LOG_WARN("handle_client exception: " + std::string(e.what()));
            response = {{"status", "error"},
                        {"error", std::string("executioner_client_exception: ") +
                                      e.what()}};
        }
        try {
            if (sock.is_open()) { // Ensure socket is valid before sending
                velix::communication::send_json(sock, response.dump());
            } else {
                LOG_WARN("handle_client: Socket is not open, skipping send.");
            }
        } catch (const std::exception &e) {
            if (errno == EPIPE) {
                LOG_WARN("handle_client send failed: Broken pipe");
            } else {
                LOG_WARN("handle_client send failed: " + std::string(e.what()));
            }
        }
    }

    // ------------------------------------------------------------------
    // Members
    // ------------------------------------------------------------------
    std::atomic<bool>                                        running_;
    ExecutionerConfig                                        config_;
    std::mutex                                               server_mu_;
    std::unique_ptr<velix::communication::SocketWrapper>    server_socket_;
    velix::utils::ThreadPool                                 thread_pool_;
    PackageCache                                             pkg_cache_;
    PrepareCoalescer                                         coalescer_;
};

ExecutionerService &service() {
    static ExecutionerService s;
    return s;
}

} // namespace

void start_executioner(int port = 5172) { service().start(port); }
void stop_executioner()          { service().stop();       }

} // namespace velix::core
