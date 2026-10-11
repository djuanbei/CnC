#include "core.hpp"
#include <iomanip>
#include <limits>
#include <openssl/evp.h>
#include <random>
namespace cnc {
volatile sig_atomic_t stopping = 0;
std::string read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    require(bool(f), "cannot read " + p.string());
    std::ostringstream s;
    s << f.rdbuf();
    require(!f.bad(), "read failed " + p.string());
    return s.str();
}
void write(const fs::path &p, const std::string &s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
    f.close();
    require(bool(f), "write failed " + p.string());
}
std::string digest(const std::string &s) {
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    require(EVP_Digest(s.data(), s.size(), bytes, &n, EVP_sha256(), nullptr) == 1, "SHA256 failed");
    std::ostringstream o;
    for (unsigned i = 0; i < n; i++)
        o << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
    return o.str();
}
std::string unique_id() {
    std::random_device r;
    std::ostringstream o;
    for (int i = 0; i < 4; i++)
        o << std::hex << r();
    return o.str();
}
void normalize(Clauses &cs) {
    for (auto &c : cs) {
        std::sort(c.begin(), c.end());
        c.erase(std::unique(c.begin(), c.end()), c.end());
    }
    cs.erase(std::remove_if(cs.begin(), cs.end(),
                            [](const auto &c) {
                                for (int l : c)
                                    if (std::binary_search(c.begin(), c.end(), -l))
                                        return true;
                                return false;
                            }),
             cs.end());
    std::sort(cs.begin(), cs.end());
    cs.erase(std::unique(cs.begin(), cs.end()), cs.end());
}
Problem Problem::parse(const std::string &text) {
    Problem p;
    std::istringstream in(text);
    std::string line;
    bool header = false;
    long long count = 0, expected = 0;
    std::vector<int> clause;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        std::string token;
        if (!(row >> token) || token == "c")
            continue;
        if (token == "p") {
            std::string format, extra;
            long long vars;
            require(!header && count == 0 && clause.empty(), "duplicate/misplaced DIMACS header");
            require(bool(row >> format >> vars >> expected) && format == "cnf" && vars >= 0 &&
                        vars < 100000000 && expected >= 0 && !(row >> extra),
                    "invalid DIMACS header");
            p.variables = int(vars);
            header = true;
            continue;
        }
        require(header, "missing DIMACS header");
        do {
            size_t end = 0;
            long long v;
            try {
                v = std::stoll(token, &end);
            } catch (...) {
                throw std::runtime_error("invalid literal");
            }
            require(end == token.size() && v >= -p.variables && v <= p.variables,
                    "out-of-range/invalid literal");
            if (v == 0) {
                p.clauses.push_back(clause);
                clause.clear();
                ++count;
            } else
                clause.push_back(int(v));
        } while (row >> token);
    }
    require(header && clause.empty() && count == expected,
            "DIMACS clause count or terminator mismatch");
    normalize(p.clauses);
    return p;
}
std::string Problem::dimacs() const {
    std::ostringstream o;
    o << "p cnf " << variables << ' ' << clauses.size() << '\n';
    for (auto &c : clauses) {
        for (int l : c)
            o << l << ' ';
        o << "0\n";
    }
    return o.str();
}
bool Problem::valid(const std::vector<int> &v, const std::vector<int> &a) const {
    if (v.size() != size_t(variables + 1))
        return false;
    for (int i = 1; i <= variables; i++)
        if (v[i] != 1 && v[i] != -1)
            return false;
    auto sat = [&](int l) { return v[std::abs(l)] == (l > 0 ? 1 : -1); };
    for (int l : a)
        if (!sat(l))
            return false;
    for (auto &c : clauses)
        if (!std::any_of(c.begin(), c.end(), sat))
            return false;
    return true;
}
Residual residual(const Problem &p, const std::vector<int> &a) {
    Residual r;
    r.formula = p;
    r.values.resize(p.variables + 1);
    for (int l : a) {
        require(l != 0 && std::abs((long long)l) <= p.variables, "invalid cube literal");
        int &v = r.values[std::abs(l)];
        int sign = l > 0 ? 1 : -1;
        if (v && v != sign) {
            r.conflict = true;
            return r;
        }
        v = sign;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        Clauses next;
        for (auto &c : r.formula.clauses) {
            bool sat = false;
            std::vector<int> left;
            for (int l : c) {
                int v = r.values[std::abs(l)];
                if (v == (l > 0 ? 1 : -1)) {
                    sat = true;
                    break;
                }
                if (!v)
                    left.push_back(l);
            }
            if (sat)
                continue;
            if (left.empty()) {
                r.conflict = true;
                return r;
            }
            if (left.size() == 1) {
                int l = left[0];
                r.values[std::abs(l)] = l > 0 ? 1 : -1;
                changed = true;
            }
            next.push_back(std::move(left));
        }
        r.formula.clauses = std::move(next);
    }
    normalize(r.formula.clauses);
    return r;
}
bool dependency_reaches(const json &cubes, int from, int target, std::set<int> &seen) {
    std::vector<int> pending{from};
    while (!pending.empty()) {
        int id = pending.back();
        pending.pop_back();
        if (id == target)
            return true;
        if (!seen.insert(id).second)
            continue;
        auto &c = cubes.at(id);
        for (auto &child : c.at("children"))
            pending.push_back(child.get<int>());
        int rep = c.at("representative");
        if (rep >= 0)
            pending.push_back(rep);
    }
    return false;
}
} // namespace cnc
