CXX ?= c++
PREFIX ?= $(shell if test -d /opt/homebrew; then echo /opt/homebrew; else echo /usr; fi)
CPPFLAGS += -Ivendor -I$(PREFIX)/include -I$(PREFIX)/include/nauty
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter
LDFLAGS += -L$(PREFIX)/lib
LDLIBS += -lnauty -lcrypto
SOURCES = main.cpp problem.cpp process.cpp analysis.cpp checkpoint.cpp scheduler.cpp
HEADERS = core.hpp fault.hpp vendor/json.hpp $(wildcard vendor/dejavu/*.h)
cnc: $(SOURCES) $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
clean:
	rm -f cnc unit_tests cnc_fault cnc_asan
.PHONY: clean test
test: cnc unit_tests cnc_fault
	./unit_tests
	python3 ../tests/orchestrator/test_cnc.py
	python3 ../tests/ramsey/test_encoding.py
unit_tests: ../tests/orchestrator/unit.cpp problem.cpp process.cpp analysis.cpp $(HEADERS)
	$(CXX) $(CPPFLAGS) -I. $(CXXFLAGS) ../tests/orchestrator/unit.cpp problem.cpp process.cpp analysis.cpp $(LDFLAGS) $(LDLIBS) -o $@

cnc_fault: $(SOURCES) $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DCNC_FAULT_TESTING $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

cnc_asan: $(SOURCES) $(HEADERS)
	$(CXX) $(CPPFLAGS) -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
