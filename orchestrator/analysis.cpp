#include "core.hpp"
#include "fault.hpp"
#include "vendor/dejavu/dejavu.h"
#include <nausparse.h>
static_assert(NAUTYVERSIONID / 100 == 293, "cnc requires nauty 2.9.3");
#include <numeric>
#include <thread>
#include <unistd.h>
namespace cnc {
struct Encoding {
    std::vector<int> vars, colors;
    std::vector<std::vector<int>> adj;
    explicit Encoding(const Problem &p) {
        std::set<int> active;
        for (auto &c : p.clauses)
            for (int l : c)
                active.insert(std::abs(l));
        vars.assign(active.begin(), active.end());
        int n = vars.size();
        std::map<int, int> index;
        for (int i = 0; i < n; i++)
            index[vars[i]] = i;
        adj.resize(2 * n + p.clauses.size());
        colors.resize(adj.size());
        auto edge = [&](int a, int b) {
            adj[a].push_back(b);
            adj[b].push_back(a);
        };
        for (int i = 0; i < n; i++) {
            colors[i] = 0;
            colors[n + i] = 1;
            edge(i, n + i);
        }
        for (size_t k = 0; k < p.clauses.size(); k++) {
            int v = 2 * n + k;
            colors[v] = 2;
            for (int l : p.clauses[k])
                edge(v, index[std::abs(l)] + (l < 0 ? n : 0));
        }
    }
};
json analyze(const Problem &p, bool symmetry, int seed) {
    Encoding e(p);
    int n = e.vars.size(), nv = e.adj.size();
    if (!n)
        return json{{"key", json(p.clauses).dump()},
                    {"hash", digest(json(p.clauses).dump())},
                    {"mapping", json::object()},
                    {"orbit", json::array()},
                    {"generators", json::array()}};
    if (symmetry) {
        int edges = 0;
        for (auto &a : e.adj)
            edges += a.size();
        dejavu::static_graph g(nv, edges / 2);
        for (int i = 0; i < nv; i++)
            g.add_vertex(e.colors[i], e.adj[i].size());
        for (int i = 0; i < nv; i++)
            for (int j : e.adj[i])
                if (i < j)
                    g.add_edge(i, j);
        std::vector<int> parent(n);
        std::iota(parent.begin(), parent.end(), 0);
        auto root = [&](int v) {
            while (parent[v] != v)
                v = parent[v];
            return v;
        };
        json generators = json::array();
        dejavu_hook hook = [&](int size, const int *perm, int, const int *) {
            if (size != nv)
                return;
            std::vector<int> mapping(p.variables + 1);
            std::set<int> images;
            for (int i = 0; i < n; i++) {
                if (perm[i] < 0 || perm[i] >= n || perm[n + i] != n + perm[i])
                    return;
                mapping[e.vars[i]] = e.vars[perm[i]];
                images.insert(perm[i]);
            }
            if (images.size() != size_t(n))
                return;
            Clauses mapped = p.clauses;
            for (auto &c : mapped)
                for (int &l : c)
                    l = (l > 0 ? 1 : -1) * mapping[std::abs(l)];
            normalize(mapped);
            if (mapped != p.clauses)
                return;
            std::vector<int> compact;
            bool nontrivial = false;
            for (int i = 0; i < n; i++) {
                parent[root(i)] = root(perm[i]);
                compact.push_back(mapping[e.vars[i]]);
                nontrivial |= perm[i] != i;
            }
            if (nontrivial)
                generators.push_back(compact);
        };
        dejavu::solver solver;
        solver.set_print(false);
        solver.set_seed(seed);
        solver.automorphisms(&g, &hook);
        std::map<int, std::vector<int>> orbits;
        for (int i = 0; i < n; i++)
            orbits[root(i)].push_back(e.vars[i]);
        std::vector<int> best;
        for (auto &pair : orbits) {
            auto &o = pair.second;
            if (o.size() > 1 && (o.size() > best.size() || (o.size() == best.size() && o < best)))
                best = o;
        }
        return json{{"orbit", best}, {"generators", generators}, {"variables", e.vars}};
    }
    SG_DECL(g);
    SG_DECL(canon);
    SG_ALLOC(g, nv, 0, "graph");
    size_t count = 0;
    for (auto &a : e.adj)
        count += a.size();
    SG_ALLOC(g, nv, count, "graph");
    g.nv = nv;
    g.nde = count;
    size_t pos = 0;
    for (int i = 0; i < nv; i++) {
        g.v[i] = pos;
        g.d[i] = e.adj[i].size();
        for (int v : e.adj[i])
            g.e[pos++] = v;
    }
    std::vector<int> lab(nv), ptn(nv), orbits(nv);
    std::iota(lab.begin(), lab.end(), 0);
    for (int i = 0; i < nv; i++)
        ptn[i] = (i + 1 < nv && e.colors[i] == e.colors[i + 1]) ? 1 : 0;
    DEFAULTOPTIONS_SPARSEGRAPH(options);
    options.getcanon = TRUE;
    options.defaultptn = FALSE;
    statsblk stats;
    sparsenauty(&g, lab.data(), ptn.data(), orbits.data(), &options, &stats, &canon);
    require(!stats.errstatus, "nauty canonicalization failed");
    std::vector<int> mapping(p.variables + 1);
    int label = 0;
    json jmap = json::object();
    for (int v : lab)
        if (v < n) {
            mapping[e.vars[v]] = ++label;
            jmap[std::to_string(e.vars[v])] = label;
        }
    Clauses mapped = p.clauses;
    for (auto &c : mapped)
        for (int &l : c)
            l = (l > 0 ? 1 : -1) * mapping[std::abs(l)];
    normalize(mapped);
    SG_FREE(g);
    SG_FREE(canon);
    auto key = json(mapped).dump();
    return json{{"key", key}, {"hash", digest(key)}, {"mapping", jmap}};
}
json bounded_analysis(const Problem &p, bool symmetry, double budget, const fs::path &dir,
                      int seed) {
    if (test_analysis_failure(symmetry))
        return nullptr;
    fs::path output = dir / (symmetry ? "symmetry.json" : "canonical.json");
    Process child;
    child.output = output;
    child.started = Clock::now();
    child.pid = fork();
    require(child.pid >= 0, "fork analysis failed");
    if (!child.pid) {
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        try {
            write(output, analyze(p, symmetry, seed).dump());
            _exit(0);
        } catch (...) {
            _exit(1);
        }
    }
    setpgid(child.pid, child.pid);
    while (!child.poll()) {
        if (stopping || seconds(child.started) >= budget) {
            child.cancel();
            return nullptr;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (child.status != 0)
        return nullptr;
    try {
        return json::parse(read(output));
    } catch (...) {
        return nullptr;
    }
}
} // namespace cnc
