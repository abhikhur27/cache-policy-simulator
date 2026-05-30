#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

enum class Policy {
  FIFO,
  LRU,
};

struct Result {
  int hits = 0;
  int misses = 0;
  std::vector<int> finalCache;
};

std::vector<int> parseTrace(std::istream& input) {
  std::vector<int> trace;
  std::string line;
  while (std::getline(input, line)) {
    std::stringstream ss(line);
    int value = 0;
    while (ss >> value) {
      trace.push_back(value);
      if (ss.peek() == ',') ss.ignore();
    }
  }
  return trace;
}

Result runSimulation(const std::vector<int>& trace, int capacity, Policy policy) {
  Result result;
  if (capacity <= 0) return result;

  std::vector<int> cache;
  cache.reserve(static_cast<size_t>(capacity));
  std::unordered_map<int, int> positions;

  for (int key : trace) {
    auto found = positions.find(key);
    if (found != positions.end()) {
      result.hits += 1;
      if (policy == Policy::LRU) {
        const int index = found->second;
        const int value = cache[static_cast<size_t>(index)];
        cache.erase(cache.begin() + index);
        cache.push_back(value);
        for (size_t i = 0; i < cache.size(); ++i) {
          positions[cache[i]] = static_cast<int>(i);
        }
      }
      continue;
    }

    result.misses += 1;
    if (static_cast<int>(cache.size()) >= capacity) {
      positions.erase(cache.front());
      cache.erase(cache.begin());
    }
    cache.push_back(key);
    positions[key] = static_cast<int>(cache.size() - 1);
  }

  result.finalCache = cache;
  return result;
}

void printResult(const std::string& label, const Result& result, int totalAccesses) {
  const double hitRate = totalAccesses > 0 ? static_cast<double>(result.hits) / totalAccesses : 0.0;
  std::cout << label << "\n";
  std::cout << "  Hits: " << result.hits << "\n";
  std::cout << "  Misses: " << result.misses << "\n";
  std::cout << "  Hit rate: " << std::fixed << std::setprecision(2) << (hitRate * 100.0) << "%\n";
  std::cout << "  Final cache: [";
  for (size_t i = 0; i < result.finalCache.size(); ++i) {
    if (i) std::cout << ", ";
    std::cout << result.finalCache[i];
  }
  std::cout << "]\n";
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cerr << "Usage: cache_policy_sim <trace_file> <cache_capacity>\n";
    return 1;
  }

  std::ifstream file(argv[1]);
  if (!file.is_open()) {
    std::cerr << "Could not open trace file: " << argv[1] << "\n";
    return 1;
  }

  int capacity = 0;
  try {
    capacity = std::stoi(argv[2]);
  } catch (...) {
    std::cerr << "Cache capacity must be an integer.\n";
    return 1;
  }

  if (capacity <= 0) {
    std::cerr << "Cache capacity must be > 0.\n";
    return 1;
  }

  const std::vector<int> trace = parseTrace(file);
  if (trace.empty()) {
    std::cerr << "Trace file has no integer accesses.\n";
    return 1;
  }

  const Result fifo = runSimulation(trace, capacity, Policy::FIFO);
  const Result lru = runSimulation(trace, capacity, Policy::LRU);

  std::cout << "Cache Policy Simulator\n";
  std::cout << "Accesses: " << trace.size() << " | Capacity: " << capacity << "\n\n";

  printResult("FIFO", fifo, static_cast<int>(trace.size()));
  std::cout << "\n";
  printResult("LRU", lru, static_cast<int>(trace.size()));

  const int hitDelta = lru.hits - fifo.hits;
  if (hitDelta > 0) {
    std::cout << "\nLRU gained " << hitDelta << " extra hits on this workload.\n";
  } else if (hitDelta < 0) {
    std::cout << "\nFIFO gained " << -hitDelta << " extra hits on this workload.\n";
  } else {
    std::cout << "\nBoth policies produced the same hit count on this workload.\n";
  }

  return 0;
}
