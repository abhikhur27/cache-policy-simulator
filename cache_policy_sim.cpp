#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class Policy {
  FIFO,
  LRU,
};

struct Result {
  int hits = 0;
  int misses = 0;
  int coldMisses = 0;
  int reloadMisses = 0;
  int evictions = 0;
  std::vector<int> finalCache;
};

struct TraceStats {
  int uniqueKeys = 0;
  int repeatedAccesses = 0;
  int hottestKey = 0;
  int hottestKeyCount = 0;
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
  std::unordered_set<int> seenKeys;

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
    if (seenKeys.insert(key).second) {
      result.coldMisses += 1;
    } else {
      result.reloadMisses += 1;
    }
    if (static_cast<int>(cache.size()) >= capacity) {
      positions.erase(cache.front());
      cache.erase(cache.begin());
      result.evictions += 1;
    }
    cache.push_back(key);
    positions[key] = static_cast<int>(cache.size() - 1);
  }

  result.finalCache = cache;
  return result;
}

TraceStats analyzeTrace(const std::vector<int>& trace) {
  TraceStats stats;
  std::unordered_map<int, int> frequencies;

  for (int key : trace) {
    const int nextCount = ++frequencies[key];
    if (nextCount > 1) {
      stats.repeatedAccesses += 1;
    }
    if (nextCount > stats.hottestKeyCount) {
      stats.hottestKey = key;
      stats.hottestKeyCount = nextCount;
    }
  }

  stats.uniqueKeys = static_cast<int>(frequencies.size());
  return stats;
}

void printResult(const std::string& label, const Result& result, int totalAccesses) {
  const double hitRate = totalAccesses > 0 ? static_cast<double>(result.hits) / totalAccesses : 0.0;
  std::cout << label << "\n";
  std::cout << "  Hits: " << result.hits << "\n";
  std::cout << "  Misses: " << result.misses << "\n";
  std::cout << "  Cold misses: " << result.coldMisses << "\n";
  std::cout << "  Reload misses: " << result.reloadMisses << "\n";
  std::cout << "  Evictions: " << result.evictions << "\n";
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
  const TraceStats stats = analyzeTrace(trace);
  const double reuseRate =
      trace.empty() ? 0.0 : static_cast<double>(stats.repeatedAccesses) / static_cast<double>(trace.size());

  std::cout << "Cache Policy Simulator\n";
  std::cout << "Accesses: " << trace.size() << " | Capacity: " << capacity << "\n\n";
  std::cout << "Trace profile\n";
  std::cout << "  Unique keys: " << stats.uniqueKeys << "\n";
  std::cout << "  Reuse rate: " << std::fixed << std::setprecision(2) << (reuseRate * 100.0) << "%\n";
  std::cout << "  Hottest key: " << stats.hottestKey << " (" << stats.hottestKeyCount << " accesses)\n\n";

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
