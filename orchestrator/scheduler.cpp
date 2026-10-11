#include "core.hpp"
#include "fault.hpp"
#include <deque>
#include <iostream>
#include <memory>
#include <nauty.h>
#include <openssl/crypto.h>
#include <sys/resource.h>
#include <thread>
namespace cnc {
json defaults(const fs::path &repo) {
    return {{"backend", "iglucose"},
            {"solver", (repo / "iglucose/core/iglucose").string()},
            {"march", (repo / "march_cu/march_cu").string()},
            {"workers", 1},
            {"attempt-seconds", 10.0},
            {"symmetry", false},
            {"deduplicate", false},
            {"symmetry-seconds", 1.0},
            {"canonical-seconds", 1.0},
            {"lookahead-seconds", 1.0},
            {"max-depth", 100000},
            {"max-cubes", 1000000},
            {"index-bytes", 67108864},
            {"checkpoint-interval", 60.0},
            {"retries", 1},
            {"seed", 0}};
}

namespace {
json cube(int id, int parent, int depth, const std::vector<int> &a, const std::string &reason) {
    return {{"id", id},
            {"parent", parent},
            {"depth", depth},
            {"fixed", a},
            {"reason", reason},
            {"state", "QUEUED"},
            {"children", json::array()},
            {"representative", -1},
            {"attempt", 0},
            {"retries", 0},
            {"active_seconds", 0.0},
            {"model", json::array()},
            {"canonical", nullptr},
            {"canonical_done", false},
            {"propagated", json::array()},
            {"block", ""},
            {"orbit", json::array()},
            {"generators", json::array()},
            {"orbit_variables", json::array()}};
}
bool terminal(const json &c) { return c["state"] == "SAT" || c["state"] == "UNSAT"; }
std::vector<int> complete(std::vector<int> v) {
    for (size_t i = 1; i < v.size(); i++)
        if (!v[i])
            v[i] = 1;
    return v;
}
size_t memory_charge(const json &j) {
    size_t n = sizeof(json) + 64;
    if (j.is_string())
        return n + j.get_ref<const std::string &>().capacity() + 1;
    if (j.is_array()) {
        n += j.get_ref<const json::array_t &>().capacity() * sizeof(json);
        for (auto &v : j)
            n += memory_charge(v);
    }
    if (j.is_object())
        for (auto it = j.begin(); it != j.end(); ++it)
            n += 128 + it.key().capacity() + memory_charge(it.value());
    return n;
}
void count(json &s, const std::string &k) { s["counters"][k] = s["counters"].value(k, 0LL) + 1; }
json versions(const json &config) {
    return {{"schema", 1},
            {"adapter", "cnc-2"},
            {"json", "3.11.3"},
            {"crypto", OpenSSL_version(OPENSSL_VERSION)},
            {"encoding", "polarity-pair-1"},
            {"dejavu", "4c275e9ebac4fe51a26aac406682b9bcbfd03a9d"},
            {"canonicalizer", NAUTYVERSION},
            {"solver_sha256", digest(read(config.at("solver").get<std::string>()))},
            {"march_sha256", digest(read(config.at("march").get<std::string>()))}};
}
std::vector<int> transfer(const Problem &f, const json &target, const json &source) {
    auto r = residual(f, target.at("fixed").get<std::vector<int>>());
    auto values = r.values;
    auto a = target.at("canonical").at("mapping"), b = source.at("canonical").at("mapping");
    std::map<int, int> inverse;
    for (auto it = b.begin(); it != b.end(); ++it)
        inverse[it.value().get<int>()] = std::stoi(it.key());
    auto model = source.at("model").get<std::vector<int>>();
    for (auto it = a.begin(); it != a.end(); ++it) {
        int from = inverse.at(it.value().get<int>());
        values.at(std::stoi(it.key())) = model.at(from);
    }
    return complete(values);
}
Clauses mapped(const Problem &p, const json &mapping) {
    std::set<int> labels;
    for (auto it = mapping.begin(); it != mapping.end(); ++it) {
        int v = std::stoi(it.key());
        require(v > 0 && v <= p.variables, "invalid canonical variable");
        int label = it.value();
        require(label > 0 && labels.insert(label).second, "invalid canonical labels");
    }
    Clauses cs = p.clauses;
    std::set<int> used;
    for (auto &c : cs)
        for (int &l : c) {
            used.insert(std::abs(l));
            int v = mapping.at(std::to_string(std::abs(l)));
            l = (l > 0 ? 1 : -1) * v;
        }
    require(used.size() == mapping.size(), "canonical mapping domain mismatch");
    normalize(cs);
    return cs;
}

void validate(json &s, const Problem &f) {
    auto &cs = s.at("cubes");
    require(cs.is_array() && !cs.empty(), "checkpoint has no root cube");
    std::set<std::string> states = {
        "QUEUED", "RUNNING", "PENDING_SPLIT", "SPLIT", "WAITING_EQUIVALENT",
        "SAT",    "UNSAT",   "UNRESOLVED"};
    require(cs[0].at("parent") == -1 && cs[0].at("depth") == 0 && cs[0].at("fixed").empty(),
            "invalid root cube");
    for (size_t i = 0; i < cs.size(); i++) {
        auto &c = cs[i];
        require(c.at("id") == i && states.count(c.at("state").get<std::string>()),
                "invalid cube identity/state");
        auto r = residual(f, c.at("fixed").get<std::vector<int>>());
        if (c.at("state") == "SAT")
            require(f.valid(c.at("model").get<std::vector<int>>(),
                            c.at("fixed").get<std::vector<int>>()),
                    "invalid saved SAT model");
        if (i) {
            int parent = c.at("parent");
            require(parent >= 0 && parent < int(i), "invalid cube parent");
            require(c.at("depth").get<int>() == cs[parent].at("depth").get<int>() + 1,
                    "invalid cube depth");
            auto children = cs[parent].at("children");
            require(std::find(children.begin(), children.end(), json(i)) != children.end(),
                    "orphan cube");
        }
        auto children = c.at("children");
        if (!children.empty())
            require(c.at("state") == "SPLIT" || terminal(c), "split cube has incompatible state");
        if (c.at("state") == "UNSAT" && !children.empty())
            for (auto &ch : children)
                require(cs.at(ch.get<int>()).at("state") == "UNSAT",
                        "UNSAT parent has unresolved child");
        require(children.empty() || children.size() == 2, "incomplete split");
        if (c.at("state") == "SPLIT")
            require(children.size() == 2, "split without children");
        if (!children.empty()) {
            std::vector<int> base = c.at("fixed"), a = base, b = base;
            auto orbit = c.at("orbit").get<std::vector<int>>();
            require(!orbit.empty(), "split evidence missing");
            for (int v : orbit)
                require(v > 0 && v <= f.variables && !r.values[v], "invalid split variable");
            if (orbit.size() > 1) {
                auto vars = c.at("orbit_variables").get<std::vector<int>>();
                std::map<int, int> parent;
                for (int v : vars)
                    parent[v] = v;
                auto root = [&](int v) {
                    while (parent.at(v) != v)
                        v = parent.at(v);
                    return v;
                };
                for (auto &gen : c.at("generators")) {
                    require(gen.size() == vars.size(), "bad symmetry evidence");
                    std::map<int, int> perm;
                    std::set<int> images;
                    for (size_t k = 0; k < vars.size(); k++) {
                        int v = gen[k];
                        require(parent.count(v) && images.insert(v).second,
                                "invalid generator permutation");
                        perm[vars[k]] = v;
                    }
                    Clauses image = r.formula.clauses;
                    for (auto &cl : image)
                        for (int &l : cl)
                            l = (l > 0 ? 1 : -1) * perm.at(std::abs(l));
                    normalize(image);
                    require(image == r.formula.clauses, "invalid generator evidence");
                    for (int v : vars)
                        parent[root(v)] = root(perm.at(v));
                }
                int group = root(orbit[0]);
                std::vector<int> expected;
                for (int v : vars)
                    if (root(v) == group)
                        expected.push_back(v);
                require(expected == orbit, "invalid recorded orbit");
            }
            a.push_back(orbit[0]);
            for (int v : orbit)
                b.push_back(-v);
            for (int k = 0; k < 2; k++) {
                int child = children[k];
                require(child > int(i) && child < int(cs.size()) && cs[child].at("parent") == i,
                        "invalid child edge");
                require(cs[child].at("fixed") == json(k ? b : a), "split assumptions mismatch");
            }
        }
        if (!c.at("canonical").is_null()) {
            auto canon = c.at("canonical");
            require(canon.at("hash") == digest(canon.at("key").get<std::string>()),
                    "invalid canonical hash");
            require(json(mapped(r.formula, canon.at("mapping"))).dump() == canon.at("key"),
                    "invalid canonical mapping/key");
        }
        int rep = c.at("representative");
        require(rep >= -1 && rep < int(cs.size()) && rep != int(i), "invalid representative");
        if (c.at("state") == "WAITING_EQUIVALENT")
            require(rep >= 0, "missing representative");
        if (rep >= 0) {
            require(children.empty(), "alias also has child obligations");
            if (c.at("state") == "UNSAT")
                require(cs[rep].at("state") == "UNSAT",
                        "UNSAT alias has unresolved representative");
            require(!c.at("canonical").is_null() && !cs[rep].at("canonical").is_null(),
                    "alias canonical data missing");
            require(c.at("canonical").at("key") == cs[rep].at("canonical").at("key"),
                    "non-equivalent alias");
        }
        require(c.at("attempt").get<long long>() < s.at("next_attempt").get<long long>() &&
                    c.at("retries").get<int>() >= 0,
                "invalid attempt/retry metadata");
    }
    std::vector<int> color(cs.size());
    for (size_t root = 0; root < cs.size(); root++) {
        std::vector<std::pair<int, bool>> pending{{int(root), false}};
        while (!pending.empty()) {
            auto [id, exit] = pending.back();
            pending.pop_back();
            if (exit) {
                color[id] = 2;
                continue;
            }
            require(color[id] != 1, "cyclic result dependencies");
            if (color[id] == 2)
                continue;
            color[id] = 1;
            pending.push_back({id, true});
            for (auto &child : cs[id]["children"])
                pending.push_back({child.get<int>(), false});
            int rep = cs[id]["representative"];
            if (rep >= 0)
                pending.push_back({rep, false});
        }
    }
}
struct Temp {
    fs::path path;
    Temp() {
        auto base = fs::temp_directory_path() / "cnc-XXXXXX";
        auto s = base.string();
        std::vector<char> v(s.begin(), s.end());
        v.push_back(0);
        char *p = mkdtemp(v.data());
        require(p, "cannot create run temp directory");
        path = p;
    }
    ~Temp() {
        std::error_code e;
        fs::remove_all(path, e);
    }
};
} // namespace
int solve(json config, const std::string &input, const std::string &location, bool resume,
          const std::set<std::string> &overrides) {
    std::unique_ptr<Checkpoint> checkpoint;
    if (!location.empty())
        checkpoint = std::make_unique<Checkpoint>(location, resume);
    std::string source;
    json s;
    if (resume) {
        s = checkpoint->restore(source);
        require(s.at("versions").at("schema") == 1, "unsupported checkpoint schema");
        if (!input.empty())
            require(digest(read(input)) == digest(source), "resume input differs from saved root");
        json saved = s.at("config");
        const std::set<std::string> allowed = {"workers", "checkpoint-interval", "max-depth",
                                               "max-cubes", "index-bytes"};
        for (auto &key : overrides) {
            require(allowed.count(key), "forbidden resume override: " + key);
            if (key == "max-depth" || key == "max-cubes" || key == "index-bytes")
                require(config[key] >= saved[key], "resume limits may only increase");
            saved[key] = config[key];
        }
        config = saved;
        require(s.at("versions") == versions(config),
                "checkpoint solver/analysis version mismatch");
    } else {
        source = read(input);
        s = {{"versions", versions(config)},
             {"run", unique_id()},
             {"next_attempt", 1},
             {"cubes", json::array({cube(0, -1, 0, {}, "root")})},
             {"counters", json::object()},
             {"outcome", "UNKNOWN"},
             {"sessions", json::array()}};
    }
    Problem f = Problem::parse(source);
    if (resume)
        require(s.at("input_sha256") == digest(source), "snapshot input identity mismatch");
    validate(s, f);
    if (!s.contains("phase_seconds"))
        s["phase_seconds"] = json::object();
    s["config"] = config;
    s["input_sha256"] = digest(source);
    auto session = unique_id();
    s["sessions"].push_back({{"id", session}, {"overrides", overrides}});
    s["session"] = session;
    for (auto &c : s["cubes"]) {
        if (c["state"] == "RUNNING")
            c["state"] = "QUEUED";
        if (c["state"] == "UNRESOLVED" && c["block"] == "split-limit")
            c["state"] = "PENDING_SPLIT";
    }
    Temp temp;
    std::map<std::string, std::vector<int>> index;
    size_t index_used = 0;
    auto add_index = [&](int id) {
        auto &c = s["cubes"][id];
        if (c["canonical"].is_null())
            return;
        std::string key = digest(c["canonical"]["key"].get<std::string>());
        index[key].push_back(id);
        index_used += memory_charge(c["canonical"]) + 256;
    };
    for (size_t id = 0; id < s["cubes"].size(); id++)
        add_index(id);
    require(index_used <= config["index-bytes"].get<size_t>(),
            "saved canonical data exceeds index budget");
    std::map<int, std::unique_ptr<Process>> active;
    // Reverse dependency edges let a completed cube notify only its parents
    // and aliases, instead of repeatedly scanning the entire cube table.
    std::vector<std::vector<int>> dependents(s["cubes"].size());
    std::deque<int> pending_results;
    for (size_t id = 0; id < s["cubes"].size(); ++id) {
        const auto &c = s["cubes"][id];
        for (const auto &child : c["children"])
            dependents[child.get<int>()].push_back(id);
        int rep = c["representative"];
        if (rep >= 0)
            dependents[rep].push_back(id);
        if (terminal(c) || c["state"] == "UNRESOLVED")
            pending_results.push_back(id);
    }
    auto last_save = Clock::now();
    auto save = [&](bool required) {
        if (!checkpoint)
            return;
        try {
            json snapshot = s;
            for (auto &[id, p] : active)
                snapshot["cubes"][id]["active_seconds"] =
                    snapshot["cubes"][id]["active_seconds"].get<double>() + seconds(p->started);
            auto save_started = Clock::now();
            checkpoint->save(snapshot, source);
            s["phase_seconds"]["checkpoint"] =
                s["phase_seconds"].value("checkpoint", 0.0) + seconds(save_started);
            last_save = Clock::now();
            std::cerr << "c checkpoint generation " << checkpoint->generation << " committed\n";
        } catch (const std::exception &e) {
            std::cerr << "c checkpoint failed (age " << seconds(last_save) << "s): " << e.what()
                      << '\n';
            if (required)
                throw;
        }
    };
    // Tests can stop at a real lifecycle boundary and save the unmodified state.
    // Cancel workers only after saving, so RUNNING restoration is exercised safely.
    auto probe = [&]() {
        for (auto &record : s["cubes"])
            if (test_snapshot_requested(record["state"].get<std::string>())) {
                require(bool(checkpoint), "test snapshot requires checkpointing");
                save(true);
                for (auto &entry : active)
                    entry.second->cancel();
                std::_Exit(99);
            }
    };
    auto log = [&](int id, const std::string &event) {
        std::cerr << "c "
                  << json{{"run", s["run"]},
                          {"session", session},
                          {"cube", id},
                          {"attempt", s["cubes"][id]["attempt"]},
                          {"event", event}}
                         .dump()
                  << '\n';
    };
    auto finish = [&](int id, const std::string &state, const std::vector<int> &model) {
        auto &c = s["cubes"][id];
        if (state == "SAT")
            require(f.valid(model, c["fixed"].get<std::vector<int>>()),
                    "SAT model validation failed");
        c["state"] = state;
        c["model"] = model;
        pending_results.push_back(id);
        fault("after-result");
        log(id, state);
    };
    auto propagate = [&]() {
        while (!pending_results.empty()) {
            int source = pending_results.front();
            pending_results.pop_front();
            for (int i : dependents[source]) {
                auto &c = s["cubes"][i];
                if (c["state"] == "WAITING_EQUIVALENT") {
                    auto &d = s["cubes"][c["representative"].get<int>()];
                    if (d["state"] == "SAT") {
                        finish(i, "SAT", transfer(f, c, d));
                    } else if (d["state"] == "UNSAT") {
                        finish(i, "UNSAT", {});
                    } else if (d["state"] == "UNRESOLVED") {
                        c["state"] = "QUEUED";
                        c["representative"] = -1;
                        count(s, "failed_representatives_released");
                    }
                } else if (c["state"] == "SPLIT") {
                    int a = c["children"][0], b = c["children"][1];
                    auto &ca = s["cubes"][a];
                    auto &cb = s["cubes"][b];
                    if (ca["state"] == "SAT" || cb["state"] == "SAT") {
                        finish(i, "SAT",
                               (ca["state"] == "SAT" ? ca : cb)["model"].get<std::vector<int>>());
                    } else if (ca["state"] == "UNSAT" && cb["state"] == "UNSAT") {
                        finish(i, "UNSAT", {});
                    }
                }
            }
        }
    };
    auto error = [&](int id, const std::string &why) {
        auto &c = s["cubes"][id];
        int failures = c["retries"];
        c["retries"] = failures + 1;
        c["block"] = why;
        c["state"] = failures < config["retries"].get<int>() ? "QUEUED" : "UNRESOLVED";
        if (c["state"] == "UNRESOLVED")
            pending_results.push_back(id);
        count(s, "worker_errors");
        log(id, why);
    };
    auto collect = [&]() {
        for (auto it = active.begin(); it != active.end();) {
            int id = it->first;
            auto &p = *it->second;
            bool done = p.poll(), timeout = false;
            if (!done && seconds(p.started) >= config["attempt-seconds"].get<double>()) {
                p.cancel();
                timeout = true;
                done = true;
            }
            if (!done) {
                ++it;
                continue;
            }
            auto &c = s["cubes"][id];
            c["active_seconds"] = c["active_seconds"].get<double>() + seconds(p.started);
            if (timeout) {
                c["state"] = "PENDING_SPLIT";
                count(s, "timeouts");
                log(id, "timeout");
                probe();
            } else if (p.status != 0 && p.status != 10 && p.status != 20)
                error(id, "worker-exit-" + std::to_string(p.status));
            else {
                try {
                    std::istringstream out(read(p.output));
                    std::string line, result;
                    auto r = residual(f, c["fixed"].get<std::vector<int>>());
                    std::vector<int> model(f.variables + 1);
                    bool ended = false;
                    while (std::getline(out, line)) {
                        std::istringstream row(line);
                        std::string tag;
                        if (!(row >> tag))
                            continue;
                        if (tag == "s") {
                            std::string value, extra;
                            require(bool(row >> value) && !(row >> extra),
                                    "malformed worker status");
                            require(result.empty(), "duplicate worker status");
                            result = value;
                        }
                        if (tag == "v") {
                            long long l;
                            while (row >> l) {
                                if (l == 0) {
                                    ended = true;
                                    continue;
                                }
                                require(!ended && l >= -f.variables && l <= f.variables,
                                        "invalid worker model literal");
                                int v = std::abs(l), sign = l > 0 ? 1 : -1;
                                require(!model[v] || model[v] == sign, "conflicting model values");
                                model[v] = sign;
                            }
                            require(row.eof(), "malformed model");
                        }
                    }
                    if (result == "UNSATISFIABLE") {
                        require(p.status != 10 &&
                                    (config["backend"] != "ilingeling" || p.status == 20),
                                "status/exit disagreement");
                        finish(id, "UNSAT", {});
                    } else if (result == "SATISFIABLE") {
                        require(p.status != 20 && ended &&
                                    (config["backend"] != "ilingeling" || p.status == 10),
                                "missing model terminator/status disagreement");
                        for (int v = 1; v <= f.variables; v++) {
                            if (!model[v])
                                model[v] = r.values[v] ? r.values[v] : 1;
                        }
                        finish(id, "SAT", model);
                    } else
                        error(id, "worker-unknown-or-malformed");
                } catch (const std::exception &e) {
                    error(id, e.what());
                }
            }
            it = active.erase(it);
        }
    };
    if (!resume)
        save(true);
    probe();
    try {
        while (true) {
            collect();
            propagate();
            if (terminal(s["cubes"][0]))
                break;
            if (stopping)
                break;
            if (checkpoint && seconds(last_save) >= config["checkpoint-interval"].get<double>())
                save(false);
            bool progress = false;
            for (size_t id = 0; id < s["cubes"].size() && !stopping; id++) {
                if (checkpoint && seconds(last_save) >= config["checkpoint-interval"].get<double>())
                    save(false);
                auto state = s["cubes"][id]["state"].get<std::string>();
                if (state != "QUEUED" && state != "PENDING_SPLIT")
                    continue;
                if (state == "QUEUED" && active.size() >= config["workers"].get<size_t>())
                    break;
                progress = true;
                auto fixed = s["cubes"][id]["fixed"].get<std::vector<int>>();
                auto residual_started = Clock::now();
                auto r = residual(f, fixed);
                s["phase_seconds"]["residual"] =
                    s["phase_seconds"].value("residual", 0.0) + seconds(residual_started);
                s["cubes"][id]["propagated"] = r.values;
                if (r.conflict) {
                    finish(id, "UNSAT", {});
                    continue;
                }
                if (r.formula.clauses.empty()) {
                    finish(id, "SAT", complete(r.values));
                    continue;
                }
                if (state == "PENDING_SPLIT") {
                    int depth = s["cubes"][id]["depth"];
                    if (depth >= config["max-depth"].get<int>() ||
                        s["cubes"].size() + 2 > config["max-cubes"].get<size_t>()) {
                        s["cubes"][id]["state"] = "UNRESOLVED";
                        s["cubes"][id]["block"] = "split-limit";
                        pending_results.push_back(id);
                        probe();
                        continue;
                    }
                    std::vector<int> orbit;
                    json evidence;
                    auto analysis_started = Clock::now();
                    if (config["symmetry"]) {
                        evidence = bounded_analysis(r.formula, true, config["symmetry-seconds"],
                                                    temp.path, config["seed"]);
                        if (!evidence.is_null())
                            orbit = evidence["orbit"].get<std::vector<int>>();
                    }
                    s["phase_seconds"]["symmetry"] =
                        s["phase_seconds"].value("symmetry", 0.0) + seconds(analysis_started);
                    if (stopping)
                        break;
                    if (orbit.empty()) {
                        auto look_started = Clock::now();
                        auto decision = lookahead(r.formula, config["march"],
                                                  config["lookahead-seconds"], temp.path);
                        s["phase_seconds"]["lookahead"] =
                            s["phase_seconds"].value("lookahead", 0.0) + seconds(look_started);
                        if (stopping)
                            break;
                        if (decision.outcome == "SAT") {
                            auto model = decision.model;
                            for (int v = 1; v <= f.variables; v++)
                                if (r.values[v])
                                    model[v] = r.values[v];
                            finish(id, "SAT", model);
                            count(s, "lookahead_terminal");
                            continue;
                        }
                        if (decision.outcome == "UNSAT") {
                            finish(id, "UNSAT", {});
                            count(s, "lookahead_terminal");
                            continue;
                        }
                        if (decision.outcome != "BRANCH") {
                            auto &blocked = s["cubes"][id];
                            blocked["state"] = "UNRESOLVED";
                            blocked["block"] = "lookahead-unavailable";
                            pending_results.push_back(id);
                            count(s, "lookahead_unavailable");
                            log(id, "lookahead-unavailable");
                            continue;
                        }
                        orbit = {decision.variable};
                        count(s, "lookahead_splits");
                    } else
                        count(s, "orbital_splits");
                    if (stopping)
                        break;
                    auto a = fixed, b = fixed;
                    a.push_back(orbit[0]);
                    for (int v : orbit)
                        b.push_back(-v);
                    int child = s["cubes"].size();
                    auto prepare = [&](int child_id, const std::vector<int> &literals,
                                       const std::string &reason) {
                        auto record = cube(child_id, id, depth + 1, literals, reason);
                        auto reduced = residual(f, literals);
                        record["propagated"] = reduced.values;
                        if (reduced.conflict)
                            record["state"] = "UNSAT";
                        else if (reduced.formula.clauses.empty()) {
                            auto model = complete(reduced.values);
                            require(f.valid(model, literals), "invalid propagated child model");
                            record["state"] = "SAT";
                            record["model"] = model;
                        }
                        return record;
                    };
                    // Publish the prepared children and their parent in one scheduler transaction.
                    auto left = prepare(child, a, "split-true"),
                         right = prepare(child + 1, b, "split-false");
                    s["cubes"].push_back(std::move(left));
                    s["cubes"].push_back(std::move(right));
                    fault("during-split");
                    auto &c = s["cubes"][id];
                    c["children"] = {child, child + 1};
                    c["orbit"] = orbit;
                    c["state"] = "SPLIT";
                    dependents.resize(s["cubes"].size());
                    dependents[child].push_back(id);
                    dependents[child + 1].push_back(id);
                    if (terminal(s["cubes"][child]))
                        pending_results.push_back(child);
                    if (terminal(s["cubes"][child + 1]))
                        pending_results.push_back(child + 1);
                    if (orbit.size() > 1) {
                        c["generators"] = evidence["generators"];
                        c["orbit_variables"] = evidence["variables"];
                    }
                    log(id, "split");
                    probe();
                    collect();
                    continue;
                }
                auto &c = s["cubes"][id];
                if (config["deduplicate"] && !c["canonical_done"].get<bool>()) {
                    auto canonical_started = Clock::now();
                    auto canon =
                        bounded_analysis(r.formula, false, config["canonical-seconds"], temp.path);
                    if (stopping)
                        break;
                    s["phase_seconds"]["canonical"] =
                        s["phase_seconds"].value("canonical", 0.0) + seconds(canonical_started);
                    c["canonical_done"] = true;
                    if (!canon.is_null()) {
                        size_t bytes = memory_charge(canon) + 256;
                        if (index_used + bytes <= config["index-bytes"].get<size_t>()) {
                            c["canonical"] = canon;
                            add_index(id);
                        } else
                            count(s, "index_full");
                    } else
                        count(s, "canonical_unavailable");
                }
                if (!c["canonical"].is_null() && c["representative"] == -1) {
                    auto bucket = index.find(digest(c["canonical"]["key"].get<std::string>()));
                    require(bucket != index.end(), "canonical index entry missing");
                    for (int j : bucket->second) {
                        if (size_t(j) == id)
                            continue;
                        auto &d = s["cubes"][j];
                        if (d["canonical"].is_null() || d["representative"] != -1 ||
                            d["state"] == "UNRESOLVED")
                            continue;
                        if (!same_canonical(c["canonical"], d["canonical"]))
                            continue;
                        std::set<int> seen;
                        if (dependency_reaches(s["cubes"], j, id, seen)) {
                            count(s, "cyclic_matches_rejected");
                            continue;
                        }
                        require(mapped(r.formula, c["canonical"]["mapping"]) ==
                                    mapped(residual(f, d["fixed"].get<std::vector<int>>()).formula,
                                           d["canonical"]["mapping"]),
                                "equivalence mapping invalid");
                        c["representative"] = j;
                        c["state"] = "WAITING_EQUIVALENT";
                        dependents[j].push_back(id);
                        if (terminal(d) || d["state"] == "UNRESOLVED")
                            pending_results.push_back(j);
                        fault("after-alias");
                        count(s, "duplicates_avoided");
                        log(id, "equivalent");
                        probe();
                        break;
                    }
                    if (c["state"] == "WAITING_EQUIVALENT")
                        continue;
                }
                auto attempt = s["next_attempt"].get<long long>();
                s["next_attempt"] = attempt + 1;
                c["attempt"] = attempt;
                fs::path file = temp.path / (std::to_string(attempt) + ".icnf"),
                         out = temp.path / (std::to_string(attempt) + ".out");
                std::ostringstream stream;
                stream << "p inccnf\n";
                for (auto &cl : f.clauses) {
                    for (int l : cl)
                        stream << l << ' ';
                    stream << "0\n";
                }
                stream << "a ";
                for (int l : fixed)
                    stream << l << ' ';
                stream << "0\n";
                write(file, stream.str());
                std::vector<std::string> args = {config["solver"], file.string()};
                if (config["backend"] == "iglucose")
                    args.push_back("-verb=0");
                else
                    args.push_back("1");
                auto process = launch(args, out, test_stop_attempt(attempt));
                active[id] = std::make_unique<Process>(std::move(process));
                c["state"] = "RUNNING";
                count(s, "attempts");
                s["counters"]["peak_workers"] =
                    std::max(s["counters"].value("peak_workers", size_t(0)), active.size());
                log(id, "dispatch");
                probe();
                collect();
            }
            propagate();
            if (terminal(s["cubes"][0]))
                break;
            if (!progress && active.empty())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        collect();
        propagate();
        for (auto &[id, p] : active) {
            p->cancel();
            auto &c = s["cubes"][id];
            c["active_seconds"] = c["active_seconds"].get<double>() + seconds(p->started);
            c["state"] = "QUEUED";
        }
        active.clear();
        std::string outcome = terminal(s["cubes"][0])
                                  ? s["cubes"][0]["state"].get<std::string>()
                                  : (stopping && checkpoint ? "PAUSED" : "UNKNOWN");
        s["outcome"] = outcome;
        struct rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        s["counters"]["maxrss_platform_units"] = usage.ru_maxrss;
        s["counters"]["generated_cubes"] = s["cubes"].size();
        int depth = 0;
        double conquer = 0;
        for (auto &c : s["cubes"]) {
            depth = std::max(depth, c["depth"].get<int>());
            conquer += c["active_seconds"].get<double>();
        }
        s["counters"]["max_depth"] = depth;
        s["counters"]["index_bytes_charged"] = index_used;
        s["phase_seconds"]["conquer"] = conquer;
        save(true);
        std::cerr << "c phase_seconds " << s["phase_seconds"].dump() << '\n';
        std::cerr << "c counters " << s["counters"].dump() << '\n';
        if (outcome == "SAT") {
            std::cout << "s SATISFIABLE\nv ";
            auto m = s["cubes"][0]["model"].get<std::vector<int>>();
            for (int i = 1; i <= f.variables; i++)
                std::cout << (m[i] > 0 ? i : -i) << ' ';
            std::cout << "0\n";
            return 10;
        }
        if (outcome == "UNSAT") {
            std::cout << "s UNSATISFIABLE\n";
            return 20;
        }
        std::cout << "s UNKNOWN\n";
        if (outcome == "PAUSED") {
            std::cerr << "c PAUSED; resume: cnc --resume " << std::quoted(location) << '\n';
            return 3;
        }
        return 0;
    } catch (...) {
        for (auto &[id, p] : active) {
            p->cancel();
            s["cubes"][id]["state"] = "QUEUED";
        }
        active.clear();
        s["outcome"] = "ERROR";
        save(false);
        throw;
    }
}
} // namespace cnc
