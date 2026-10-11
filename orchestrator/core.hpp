#pragma once
#include "vendor/json.hpp"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace cnc {
using json = nlohmann::json;
using Clauses = std::vector<std::vector<int>>;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
extern volatile sig_atomic_t stopping;
inline double seconds(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}
inline void require(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::string read(const fs::path &);
void write(const fs::path &, const std::string &);
std::string digest(const std::string &);
std::string unique_id();
inline bool same_canonical(const json &a, const json &b) {
    return a.at("hash") == b.at("hash") && a.at("key") == b.at("key");
}
struct Problem {
    int variables = 0;
    Clauses clauses;
    static Problem parse(const std::string &);
    std::string dimacs() const;
    bool valid(const std::vector<int> &model, const std::vector<int> &assumptions = {}) const;
};
struct Residual {
    Problem formula;
    std::vector<int> values;
    bool conflict = false;
};
Residual residual(const Problem &, const std::vector<int> &);
void normalize(Clauses &);
struct Process {
    int pid = -1, status = -1;
    Clock::time_point started;
    fs::path output;
    Process() = default;
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;
    Process(Process &&) noexcept;
    ~Process();
    bool poll();
    void cancel(double grace = .1);
};
Process launch(const std::vector<std::string> &args, const fs::path &output,
               bool stop_before_exec = false);
json analyze(const Problem &, bool symmetry, int seed = 0);
json bounded_analysis(const Problem &, bool symmetry, double budget, const fs::path &,
                      int seed = 0);
struct LookaheadResult {
    int variable = 0;
    std::string outcome = "UNRESOLVED";
    std::vector<int> model;
};
LookaheadResult lookahead(const Problem &, const std::string &binary, double budget,
                          const fs::path &);
bool dependency_reaches(const json &, int from, int target, std::set<int> &seen);
struct Checkpoint {
    fs::path directory;
    int lockfd = -1;
    unsigned long generation = 0;
    explicit Checkpoint(const fs::path &, bool resume);
    ~Checkpoint();
    json restore(std::string &root);
    void save(const json &state, const std::string &root);
};
json defaults(const fs::path &repo);
int solve(json config, const std::string &input, const std::string &checkpoint, bool resume,
          const std::set<std::string> &overrides);
} // namespace cnc
