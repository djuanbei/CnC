#include "core.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
namespace cnc {
Process::Process(Process &&o) noexcept
    : pid(o.pid), status(o.status), started(o.started), output(std::move(o.output)) {
    o.pid = -1;
}
Process::~Process() {
    if (pid > 0)
        cancel();
}
bool Process::poll() {
    if (pid < 0)
        return true;
    int s;
    int r = waitpid(pid, &s, WNOHANG);
    if (r == 0 || (r < 0 && errno == EINTR))
        return false;
    require(r > 0, "waitpid failed");
    status = WIFEXITED(s) ? WEXITSTATUS(s) : 128 + WTERMSIG(s);
    // Any descendants of a completed worker belong to that attempt, too.
    kill(-pid, SIGKILL);
    pid = -1;
    return true;
}
void Process::cancel(double grace) {
    if (pid < 0)
        return;
    kill(-pid, SIGTERM);
    auto t = Clock::now();
    while (!poll() && seconds(t) < grace)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (pid > 0) {
        kill(-pid, SIGKILL);
        int s;
        while (waitpid(pid, &s, 0) < 0 && errno == EINTR) {
        }
        pid = -1;
        status = 137;
    }
}
Process launch(const std::vector<std::string> &args, const fs::path &out, bool stop_before_exec) {
    require(!args.empty(), "empty command");
    Process p;
    p.output = out;
    p.started = Clock::now();
    int fd = open(out.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    require(fd >= 0, "cannot create worker output");
    p.pid = fork();
    if (p.pid < 0) {
        close(fd);
        throw std::runtime_error("fork failed");
    }
    if (p.pid == 0) {
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        int null = open("/dev/null", O_RDONLY);
        dup2(null, STDIN_FILENO);
        close(null);
        std::vector<char *> argv;
        for (auto &s : args)
            argv.push_back(const_cast<char *>(s.c_str()));
        argv.push_back(nullptr);
        if (stop_before_exec)
            raise(SIGSTOP); // Deterministic lifecycle tests stop before the solver can finish.
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(fd);
    setpgid(p.pid, p.pid);
    return p;
}
LookaheadResult lookahead(const Problem &p, const std::string &binary, double budget,
                          const fs::path &dir) {
    auto in = dir / "look.cnf", out = dir / "look.out";
    write(in, p.dimacs());
    auto process = launch(
        {binary, in.string(), "-branch-only", "-p", "-o", (dir / "look.cubes").string()}, out);
    while (!process.poll()) {
        if (stopping || seconds(process.started) >= budget) {
            process.cancel();
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    LookaheadResult result;
    if (process.status != 0 && process.status != 10 && process.status != 20)
        return result;
    result.model.resize(p.variables + 1);
    bool ended = false;
    std::string status;
    std::istringstream text(read(out));
    std::string line;
    while (std::getline(text, line)) {
        std::istringstream row(line);
        std::string tag, extra;
        if (!(row >> tag))
            continue;
        if (tag == "b") {
            long long v;
            if (!(row >> v) || row >> extra || v == 0 || v > p.variables || v < -p.variables ||
                result.variable)
                return {};
            result.variable = std::abs(v);
        } else if (tag == "s") {
            if (!status.empty() || !(row >> status) || row >> extra)
                return {};
        } else if (tag == "v") {
            long long v;
            while (row >> v) {
                if (v == 0) {
                    ended = true;
                    continue;
                }
                if (ended || v > p.variables || v < -p.variables)
                    return {};
                int sign = v > 0 ? 1 : -1;
                int &value = result.model[std::abs(v)];
                if (value && value != sign)
                    return {};
                value = sign;
            }
            if (!row.eof())
                return {};
        }
    }
    if (result.variable && status.empty() && process.status == 0) {
        bool present = false;
        for (auto &c : p.clauses)
            for (int v : c)
                present |= std::abs(v) == result.variable;
        if (present) {
            result.outcome = "BRANCH";
            return result;
        }
        return {};
    }
    if (result.variable)
        return {};
    if (status == "UNSATISFIABLE" && process.status == 20) {
        result.outcome = "UNSAT";
        return result;
    }
    if (status == "SATISFIABLE" && process.status == 10 && ended) {
        for (int i = 1; i <= p.variables; i++)
            if (!result.model[i])
                result.model[i] = 1;
        if (p.valid(result.model)) {
            result.outcome = "SAT";
            return result;
        }
    }
    return {};
}
} // namespace cnc
