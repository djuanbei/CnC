#pragma once
// Fault injection is compiled only into the test executable. Production builds
// do not consult environment variables or expose failure controls.
#ifdef CNC_FAULT_TESTING
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
namespace cnc {
inline bool test_snapshot_requested(const std::string &state) {
    const char *value = std::getenv("CNC_TEST_SNAPSHOT_STATE");
    return value && state == value;
}
inline bool test_stop_attempt(long long attempt) {
    const char *value = std::getenv("CNC_TEST_STOP_ATTEMPTS");
    return value && attempt <= std::atoll(value);
}
inline bool test_analysis_failure(bool symmetry) {
    const char *value = std::getenv("CNC_TEST_ANALYSIS_FAILURE");
    return value && std::string(value) == (symmetry ? "symmetry" : "canonical");
}
inline void fault(const char *point) {
    const char *requested = std::getenv("CNC_FAULT_POINT");
    if (!requested || std::string(requested) != point)
        return;
    static std::map<std::string, int> hits;
    int at = 1;
    if (const char *value = std::getenv("CNC_FAULT_AT"))
        at = std::atoi(value);
    if (++hits[point] != at)
        return;
    if (std::getenv("CNC_FAULT_THROW"))
        throw std::runtime_error("injected storage failure");
    std::_Exit(99);
}
} // namespace cnc
#else
namespace cnc {
inline void fault(const char *) {}
inline bool test_snapshot_requested(const std::string &) { return false; }
inline bool test_stop_attempt(long long) { return false; }
inline bool test_analysis_failure(bool) { return false; }
} // namespace cnc
#endif
