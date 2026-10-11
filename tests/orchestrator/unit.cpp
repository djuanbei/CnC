#include "core.hpp"
#include <iostream>
#include <random>
using namespace cnc;
bool oracle(const Problem &p, const std::vector<int> &assumptions = {}) {
  for (int mask = 0; mask < (1 << p.variables); mask++) {
    std::vector<int> v(p.variables + 1);
    for (int i = 1; i <= p.variables; i++)
      v[i] = (mask >> (i - 1) & 1) ? 1 : -1;
    if (p.valid(v, assumptions))
      return true;
  }
  return false;
}
int main() {
  try {
    require(
        digest("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA256 vector");
    json ca = {{"key", "[[1,2]]"}, {"hash", "forced-collision"}};
    json cb = {{"key", "[[-1,-2]]"}, {"hash", "forced-collision"}};
    require(!same_canonical(ca, cb),
            "hash collision merged different canonical forms");
    Clauses universe = {{},     {1},     {-1},    {2},     {-2},
                        {1, 2}, {1, -2}, {-1, 2}, {-1, -2}};
    for (unsigned mask = 0; mask < (1u << universe.size()); mask++) {
      Problem p;
      p.variables = 2;
      for (size_t i = 0; i < universe.size(); i++)
        if (mask & (1u << i))
          p.clauses.push_back(universe[i]);
      normalize(p.clauses);
      auto r = residual(p, {});
      require(r.conflict ? !oracle(p) : oracle(p) == oracle(r.formula),
              "exhaustive propagation mismatch");
      if (!r.conflict && !r.formula.clauses.empty()) {
        auto sym = analyze(r.formula, true);
        auto orbit = sym["orbit"].get<std::vector<int>>();
        if (!orbit.empty()) {
          std::vector<int> negative;
          for (int v : orbit)
            negative.push_back(-v);
          require(oracle(r.formula) == (oracle(r.formula, {orbit[0]}) ||
                                        oracle(r.formula, negative)),
                  "exhaustive orbital mismatch");
        }
      }
    }

    std::mt19937 rng(123);
    for (int test = 0; test < 160; test++) {
      Problem p;
      p.variables = 1 + rng() % 6;
      for (unsigned c = 0; c < rng() % 18; c++) {
        std::vector<int> cl;
        for (unsigned k = 0; k < 1 + rng() % 4; k++) {
          int v = 1 + rng() % p.variables;
          cl.push_back(rng() % 2 ? v : -v);
        }
        p.clauses.push_back(cl);
      }
      normalize(p.clauses);
      auto parsed = Problem::parse(p.dimacs());
      require(parsed.clauses == p.clauses, "DIMACS round trip");
      auto r = residual(p, {});
      if (r.conflict) {
        require(!oracle(p), "false propagated conflict");
        continue;
      }
      require(oracle(r.formula) == oracle(p), "propagation changed decision");
      auto a = analyze(r.formula, false);
      std::vector<int> permutation(p.variables);
      std::iota(permutation.begin(), permutation.end(), 1);
      std::shuffle(permutation.begin(), permutation.end(), rng);
      Problem renamed = r.formula;
      for (auto &c : renamed.clauses)
        for (int &l : c)
          l = (l > 0 ? 1 : -1) * permutation[std::abs(l) - 1];
      normalize(renamed.clauses);
      require(a["key"] == analyze(renamed, false)["key"],
              "canonical relabeling mismatch");
      auto sym = analyze(r.formula, true);
      auto orbit = sym["orbit"].get<std::vector<int>>();
      if (orbit.empty())
        continue;
      std::vector<int> negative;
      for (int v : orbit)
        negative.push_back(-v);
      require(oracle(r.formula) == (oracle(r.formula, {orbit[0]}) ||
                                    oracle(r.formula, negative)),
              "orbital branching unsound");
    }
    auto emerging = Problem::parse("p cnf 4 3\n-1 2 3 0\n1 2 0\n1 2 4 0\n");
    auto simplified = residual(emerging, {1});
    require(analyze(simplified.formula, true)["orbit"] == json({2, 3}),
            "new residual symmetry not found");
    auto p = Problem::parse("p cnf 3 3\n1 2 0\n1 3 0\n2 3 0\n");
    require(analyze(p, true)["orbit"] == json({1, 2, 3}),
            "known orbit missing");
    require(analyze(Problem::parse("p cnf 2 1\n1 2 0\n"), false)["key"] !=
                analyze(Problem::parse("p cnf 2 1\n-1 -2 0\n"), false)["key"],
            "polarity collapsed");
    require(residual(p, {1, -1}).conflict, "contradictory assumptions");
    for (auto text :
         {"p cnf 1 1\n2 0\n", "p cnf 1 1\n1\n", "p cnf 1 0\n1 0\n"}) {
      bool rejected = false;
      try {
        Problem::parse(text);
      } catch (...) {
        rejected = true;
      }
      require(rejected, "invalid DIMACS accepted");
    }
    json dag =
        json::array({{{"children", {1, 2}}, {"representative", -1}},
                     {{"children", json::array()}, {"representative", -1}},
                     {{"children", json::array()}, {"representative", 1}}});
    std::set<int> seen;
    require(dependency_reaches(dag, 0, 1, seen), "ancestor alias cycle missed");
    seen.clear();
    require(dependency_reaches(dag, 2, 1, seen), "alias dependency missed");
    seen.clear();
    require(!dependency_reaches(dag, 1, 2, seen),
            "independent sibling rejected");
    std::cout
        << "unit: residual, canonical, symmetry, and parser checks passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
