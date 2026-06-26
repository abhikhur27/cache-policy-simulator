#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
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

struct CommandLineOptions {
  std::string traceFile;
  std::vector<int> capacities;
  std::string csvOutPath;
};

std::vector<int> parseCapacities(const std::string& raw) {
  std::vector<int> capacities;
  if (raw.find('-') != std::string::npos) {
    const size_t dash = raw.find('-');
    const int start = std::stoi(raw.substr(0, dash));
    const int end = std::stoi(raw.substr(dash + 1));
    if (start <= 0 || end <= 0) {
      throw std::invalid_argument("Capacity values must be positive.");
    }
    if (start > end) {
      throw std::invalid_argument("Capacity range must be ascending.");
    }
    for (int capacity = start; capacity <= end; ++capacity) {
      capacities.push_back(capacity);
    }
    return capacities;
  }

  std::stringstream ss(raw);
  std::string token;
  while (std::getline(ss, token, ',')) {
    if (token.empty()) continue;
    const int capacity = std::stoi(token);
    if (capacity <= 0) {
      throw std::invalid_argument("Capacity values must be positive.");
    }
    capacities.push_back(capacity);
  }

  if (capacities.empty()) {
    throw std::invalid_argument("At least one capacity is required.");
  }

  std::sort(capacities.begin(), capacities.end());
  capacities.erase(std::unique(capacities.begin(), capacities.end()), capacities.end());
  return capacities;
}

std::vector<int> parseTrace(std::istream& input) {
  std::vector<int> trace;
  std::string line;
  while (std::getline(input, line)) {
    if (line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
      line.erase(0, 3);
    }
    std::replace(line.begin(), line.end(), ',', ' ');
    std::stringstream ss(line);
    int value = 0;
    while (ss >> value) {
      trace.push_back(value);
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

void printSweepSummary(const std::vector<int>& capacities, const std::vector<Result>& fifoResults,
                       const std::vector<Result>& lruResults, int totalAccesses) {
  std::cout << "Capacity sweep\n";
  std::cout << "Cap | FIFO hit% | LRU hit% | Delta hits | Winner\n";
  for (size_t i = 0; i < capacities.size(); ++i) {
    const double fifoRate =
        totalAccesses > 0 ? static_cast<double>(fifoResults[i].hits) / static_cast<double>(totalAccesses) : 0.0;
    const double lruRate =
        totalAccesses > 0 ? static_cast<double>(lruResults[i].hits) / static_cast<double>(totalAccesses) : 0.0;
    const int hitDelta = lruResults[i].hits - fifoResults[i].hits;
    const std::string winner = hitDelta > 0 ? "LRU" : (hitDelta < 0 ? "FIFO" : "Tie");
    std::cout << std::setw(3) << capacities[i] << " | " << std::setw(8) << std::fixed << std::setprecision(2)
              << (fifoRate * 100.0) << "% | " << std::setw(7) << (lruRate * 100.0) << "% | " << std::setw(10)
              << hitDelta << " | " << winner << "\n";
  }
}

void printBeladyAlerts(const std::vector<int>& capacities, const std::vector<Result>& fifoResults,
                       const std::vector<Result>& lruResults) {
  std::vector<std::string> fifoAlerts;
  std::vector<std::string> lruNotes;

  for (size_t i = 1; i < capacities.size(); ++i) {
    if (fifoResults[i].misses > fifoResults[i - 1].misses) {
      std::ostringstream row;
      row << "  FIFO anomaly between capacity " << capacities[i - 1] << " and " << capacities[i]
          << ": misses rose from " << fifoResults[i - 1].misses << " to " << fifoResults[i].misses << ".";
      fifoAlerts.push_back(row.str());
    }
    if (lruResults[i].misses > lruResults[i - 1].misses) {
      std::ostringstream row;
      row << "  LRU misses rose from " << lruResults[i - 1].misses << " to " << lruResults[i].misses
          << " between capacity " << capacities[i - 1] << " and " << capacities[i]
          << ". Re-check the trace because LRU should not show Belady anomalies.";
      lruNotes.push_back(row.str());
    }
  }

  std::cout << "\nBelady anomaly check\n";
  if (fifoAlerts.empty()) {
    std::cout << "  FIFO showed no Belady anomaly across the requested capacities.\n";
  } else {
    std::cout << "  FIFO anomaly detected:\n";
    for (const std::string& alert : fifoAlerts) {
      std::cout << alert << "\n";
    }
  }

  if (!lruNotes.empty()) {
    for (const std::string& note : lruNotes) {
      std::cout << note << "\n";
    }
  }
}

CommandLineOptions parseOptions(int argc, char* argv[]) {
  if (argc < 3) {
    throw std::invalid_argument("Usage");
  }

  CommandLineOptions options;
  options.traceFile = argv[1];
  options.capacities = parseCapacities(argv[2]);

  for (int index = 3; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--csv-out") {
      if (index + 1 >= argc) {
        throw std::invalid_argument("Missing CSV output path after --csv-out.");
      }
      options.csvOutPath = argv[++index];
      continue;
    }
    throw std::invalid_argument("Unknown argument: " + arg);
  }

  return options;
}

void writeCsvReport(const std::string& path, const std::vector<int>& capacities, const std::vector<Result>& fifoResults,
                    const std::vector<Result>& lruResults, const TraceStats& stats, int totalAccesses) {
  std::ofstream out(path);
  if (!out.is_open()) {
    throw std::runtime_error("Could not open CSV output path: " + path);
  }

  const double reuseRate =
      totalAccesses > 0 ? static_cast<double>(stats.repeatedAccesses) / static_cast<double>(totalAccesses) : 0.0;

  out << "capacity,policy,hits,misses,cold_misses,reload_misses,evictions,hit_rate,unique_keys,reuse_rate,hottest_key,"
         "hottest_key_count\n";

  const auto writeRow = [&](int capacity, const char* policy, const Result& result) {
    const double hitRate = totalAccesses > 0 ? static_cast<double>(result.hits) / static_cast<double>(totalAccesses) : 0.0;
    out << capacity << ',' << policy << ',' << result.hits << ',' << result.misses << ',' << result.coldMisses << ','
        << result.reloadMisses << ',' << result.evictions << ',' << std::fixed << std::setprecision(4) << hitRate << ','
        << stats.uniqueKeys << ',' << reuseRate << ',' << stats.hottestKey << ',' << stats.hottestKeyCount << "\n";
  };

  for (size_t i = 0; i < capacities.size(); ++i) {
    writeRow(capacities[i], "FIFO", fifoResults[i]);
    writeRow(capacities[i], "LRU", lruResults[i]);
  }
}

int main(int argc, char* argv[]) {
  CommandLineOptions options;
  try {
    options = parseOptions(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "Usage: cache_policy_sim <trace_file> <cache_capacity|start-end|c1,c2,...> [--csv-out report.csv]\n";
    if (std::string(error.what()) != "Usage") {
      std::cerr << error.what() << "\n";
    }
    return 1;
  }

  std::ifstream file(options.traceFile);
  if (!file.is_open()) {
    std::cerr << "Could not open trace file: " << options.traceFile << "\n";
    return 1;
  }

  const std::vector<int> trace = parseTrace(file);
  if (trace.empty()) {
    std::cerr << "Trace file has no integer accesses.\n";
    return 1;
  }

  const TraceStats stats = analyzeTrace(trace);
  const double reuseRate =
      trace.empty() ? 0.0 : static_cast<double>(stats.repeatedAccesses) / static_cast<double>(trace.size());

  std::cout << "Cache Policy Simulator\n";
  std::cout << "Accesses: " << trace.size() << " | Capacities: ";
  for (size_t i = 0; i < options.capacities.size(); ++i) {
    if (i) std::cout << ", ";
    std::cout << options.capacities[i];
  }
  std::cout << "\n\n";
  std::cout << "Trace profile\n";
  std::cout << "  Unique keys: " << stats.uniqueKeys << "\n";
  std::cout << "  Reuse rate: " << std::fixed << std::setprecision(2) << (reuseRate * 100.0) << "%\n";
  std::cout << "  Hottest key: " << stats.hottestKey << " (" << stats.hottestKeyCount << " accesses)\n\n";

  std::vector<Result> fifoResults;
  std::vector<Result> lruResults;
  fifoResults.reserve(options.capacities.size());
  lruResults.reserve(options.capacities.size());

  for (int capacity : options.capacities) {
    fifoResults.push_back(runSimulation(trace, capacity, Policy::FIFO));
    lruResults.push_back(runSimulation(trace, capacity, Policy::LRU));
  }

  if (!options.csvOutPath.empty()) {
    try {
      writeCsvReport(options.csvOutPath, options.capacities, fifoResults, lruResults, stats, static_cast<int>(trace.size()));
      std::cout << "CSV report: " << options.csvOutPath << "\n\n";
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\n";
      return 1;
    }
  }

  if (options.capacities.size() == 1) {
    printResult("FIFO", fifoResults[0], static_cast<int>(trace.size()));
    std::cout << "\n";
    printResult("LRU", lruResults[0], static_cast<int>(trace.size()));

    const int hitDelta = lruResults[0].hits - fifoResults[0].hits;
    if (hitDelta > 0) {
      std::cout << "\nLRU gained " << hitDelta << " extra hits on this workload.\n";
    } else if (hitDelta < 0) {
      std::cout << "\nFIFO gained " << -hitDelta << " extra hits on this workload.\n";
    } else {
      std::cout << "\nBoth policies produced the same hit count on this workload.\n";
    }
    return 0;
  }

  printSweepSummary(options.capacities, fifoResults, lruResults, static_cast<int>(trace.size()));
  printBeladyAlerts(options.capacities, fifoResults, lruResults);

  return 0;
}
