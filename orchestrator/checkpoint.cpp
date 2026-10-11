#include "core.hpp"
#include "fault.hpp"
#include <fcntl.h>
#include <iostream>
#include <sys/file.h>
#include <unistd.h>
namespace cnc {
namespace {
void durable(const fs::path &p, const std::string &bytes) {
    fault("storage-write");
    int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    require(fd >= 0, "checkpoint open failed");
    size_t offset = 0;
    while (offset < bytes.size()) {
        ssize_t n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (n <= 0) {
            close(fd);
            throw std::runtime_error("checkpoint write failed");
        }
        offset += n;
    }
    fault("before-file-flush");
    bool ok = fsync(fd) == 0;
    close(fd);
    require(ok, "checkpoint flush failed");
}
void flush_dir(const fs::path &p) {
    int fd = open(p.c_str(), O_RDONLY);
    require(fd >= 0, "directory open failed");
    bool ok = fsync(fd) == 0;
    close(fd);
    require(ok, "directory flush failed");
}
} // namespace
Checkpoint::Checkpoint(const fs::path &p, bool resume) : directory(p) {
    if (!resume)
        require(fs::create_directory(p), "checkpoint directory already exists; use --resume");
    require(fs::is_directory(p), "checkpoint directory missing");
    lockfd = open((p / "lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    require(lockfd >= 0, "checkpoint lock open failed");
    if (flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
        close(lockfd);
        lockfd = -1;
        throw std::runtime_error("checkpoint already in use");
    }
}
Checkpoint::~Checkpoint() {
    if (lockfd >= 0)
        close(lockfd);
}
void Checkpoint::save(const json &state, const std::string &root) {
    if (!fs::exists(directory / "root.cnf")) {
        durable(directory / "root.tmp", root);
        fs::rename(directory / "root.tmp", directory / "root.cnf");
        flush_dir(directory);
    }
    require(digest(read(directory / "root.cnf")) == digest(root), "saved root identity mismatch");
    unsigned long next = generation + 1;
    std::string name = "state-" + std::to_string(next) + ".json", bytes = state.dump();
    durable(directory / (name + ".tmp"), bytes);
    fault("before-state-publish");
    fs::rename(directory / (name + ".tmp"), directory / name);
    fault("after-state-publish");
    flush_dir(directory);
    json manifest = {{"generation", next},
                     {"file", name},
                     {"sha256", digest(bytes)},
                     {"root_sha256", digest(root)}};
    // Keep independently readable commit manifests; never rely on a corrupt latest pointer.
    auto m = "commit-" + std::to_string(next) + ".json";
    durable(directory / (m + ".tmp"), manifest.dump());
    fault("before-manifest-publish");
    fs::rename(directory / (m + ".tmp"), directory / m);
    fault("after-manifest-publish");
    flush_dir(directory);
    generation = next;
    // Retain two *valid* generations, not merely adjacent generation numbers.
    // Recovery may have skipped a corrupt generation between them.
    std::vector<std::pair<unsigned long, fs::path>> valid;
    for (auto &f : fs::directory_iterator(directory)) {
        auto name = f.path().filename().string();
        if (name.rfind("commit-", 0) != 0 || f.path().extension() != ".json")
            continue;
        try {
            auto manifest = json::parse(read(f.path()));
            unsigned long n = manifest.at("generation");
            auto data = "state-" + std::to_string(n) + ".json";
            if (manifest.at("file") == data &&
                manifest.at("sha256") == digest(read(directory / data)) &&
                manifest.at("root_sha256") == digest(root))
                valid.push_back({n, f.path()});
        } catch (...) { /* A corrupt generation must not evict a valid backup. */
        }
    }
    std::sort(valid.rbegin(), valid.rend());
    for (size_t i = 2; i < valid.size(); i++) {
        fs::remove(directory / ("state-" + std::to_string(valid[i].first) + ".json"));
        fs::remove(valid[i].second);
    }
    flush_dir(directory);
}
json Checkpoint::restore(std::string &root) {
    std::vector<unsigned long> candidates;
    for (auto &f : fs::directory_iterator(directory)) {
        auto s = f.path().filename().string();
        if (s.rfind("commit-", 0) != 0 || f.path().extension() != ".json")
            continue;
        try {
            size_t pos;
            auto n = std::stoul(s.substr(7), &pos);
            if (s.substr(7 + pos) == ".json")
                candidates.push_back(n);
        } catch (...) {
        }
    }
    std::sort(candidates.rbegin(), candidates.rend());
    require(!candidates.empty(), "no committed checkpoint");
    root = read(directory / "root.cnf");
    for (auto n : candidates) {
        try {
            json m = json::parse(read(directory / ("commit-" + std::to_string(n) + ".json")));
            std::string file = "state-" + std::to_string(n) + ".json";
            require(m.at("generation") == n && m.at("file") == file, "invalid checkpoint manifest");
            auto bytes = read(directory / file);
            require(m.at("sha256") == digest(bytes) && m.at("root_sha256") == digest(root),
                    "checkpoint checksum mismatch");
            auto state = json::parse(bytes);
            generation = candidates.front();
            return state;
        } catch (const std::exception &e) {
            std::cerr << "c checkpoint generation " << n << " rejected: " << e.what()
                      << "; trying previous generation\n";
        }
    }
    throw std::runtime_error("no intact checkpoint generation");
}
} // namespace cnc
