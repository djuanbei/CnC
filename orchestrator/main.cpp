#include "core.hpp"
#include <cmath>
#include <iostream>
#include <unistd.h>
namespace {
void stop(int) { cnc::stopping = 1; }
} // namespace
int main(int argc, char **argv) {
    using namespace cnc;
    try {
        fs::path executable = fs::weakly_canonical(fs::absolute(argv[0]));
        json config = defaults(executable.parent_path().parent_path());
        std::string input, location;
        bool resume = false, interval = false, solver = false;
        std::set<std::string> overrides;
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                std::cout << "Usage: cnc [input.cnf] [options]\n"
                             "  --backend iglucose|ilingeling   --solver PATH   --march PATH\n"
                             "  --symmetry                    --deduplicate\n"
                             "  --workers N (1)               --attempt-seconds S (10)\n"
                             "  --symmetry-seconds S (1)       --canonical-seconds S (1)\n"
                             "  --lookahead-seconds S (1)      --retries N (1)\n"
                             "  --max-depth N (100000)        --max-cubes N (1000000)\n"
                             "  --index-bytes N (67108864)    --seed N (0)\n"
                             "  --checkpoint-dir PATH         --checkpoint-interval S (60)\n"
                             "  --resume PATH\n"
                             "Exit: SAT 10, UNSAT 20, UNKNOWN 0, PAUSED 3, ERROR 2.\n";
                return 0;
            }
            if (arg == "--symmetry" || arg == "--deduplicate") {
                auto key = arg.substr(2);
                require(overrides.insert(key).second, "duplicate option");
                config[key] = true;
                continue;
            }
            if (arg.rfind("--", 0) != 0) {
                require(input.empty(), "multiple input files");
                input = arg;
                continue;
            }
            require(i + 1 < argc, "missing option value: " + arg);
            std::string value = argv[++i], key = arg.substr(2);
            if (key == "checkpoint-dir" || key == "resume") {
                require(location.empty(),
                        "choose only one checkpoint directory or resume directory");
                location = fs::absolute(value).string();
                resume = key == "resume";
                continue;
            }
            require(config.contains(key), "unknown option: " + arg);
            require(overrides.insert(key).second, "duplicate option: " + arg);
            if (config[key].is_string()) {
                config[key] = value;
                if (key == "solver")
                    solver = true;
            } else if (config[key].is_number_float()) {
                size_t end;
                double n = std::stod(value, &end);
                require(end == value.size() && std::isfinite(n) && n > 0,
                        "invalid positive duration");
                config[key] = n;
            } else if (config[key].is_number_integer()) {
                size_t end;
                long long n = std::stoll(value, &end);
                require(end == value.size() && n >= 0 && n <= 2147483647LL,
                        "invalid nonnegative integer");
                config[key] = n;
            } else
                throw std::runtime_error("invalid flag: " + arg);
            if (key == "checkpoint-interval")
                interval = true;
        }
        require(!interval || !location.empty(),
                "checkpoint interval needs --checkpoint-dir or --resume");
        require(resume || !input.empty(), "input CNF required");
        require(config["workers"].get<int>() > 0 && config["max-cubes"].get<int>() > 0,
                "workers and max-cubes must be positive");
        require(config["backend"] == "iglucose" || config["backend"] == "ilingeling",
                "unsupported backend");
        if (!solver && config["backend"] == "ilingeling")
            config["solver"] =
                (executable.parent_path().parent_path() / "lingeling/ilingeling").string();
        for (auto key : {"solver", "march"})
            config[key] = fs::absolute(config[key].get<std::string>()).string();
        signal(SIGINT, stop);
        signal(SIGTERM, stop);
        return solve(config, input, location, resume, overrides);
    } catch (const std::exception &e) {
        std::cerr << "c ERROR: " << e.what() << '\n';
        return 2;
    }
}
