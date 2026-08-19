#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class Policy {
  FIFO,
  LRU,
  OPT,
};

struct Result {
  int hits = 0;
  int misses = 0;
  int coldMisses = 0;
  int reloadMisses = 0;
  int evictions = 0;
  std::vector<int> finalCache;
  std::unordered_map<int, int> keyHits;
  std::unordered_map<int, int> keyColdMisses;
  std::unordered_map<int, int> keyReloadMisses;
  std::unordered_map<int, int> keyEvictions;
};

struct TraceStats {
  int uniqueKeys = 0;
  int repeatedAccesses = 0;
  int hottestKey = 0;
  int hottestKeyCount = 0;
};

struct PhaseSummary {
  int phaseIndex = 0;
  int startAccess = 0;
  int endAccess = 0;
  TraceStats stats;
  Result fifo;
  Result lru;
  Result opt;
};

struct PhasePolicyCounts {
  int lruWins = 0;
  int fifoWins = 0;
  int ties = 0;
};

struct CommandLineOptions {
  std::string traceFile;
  std::vector<int> capacities;
  std::string csvOutPath;
  std::string markdownOutPath;
  std::string jsonOutPath;
  int phaseWindow = 0;
  int topKeys = 5;
  bool selfTest = false;
};

struct KeyPressureRow {
  int key = 0;
  int hits = 0;
  int coldMisses = 0;
  int reloadMisses = 0;
  int evictions = 0;
  int totalMisses = 0;
};

int mapValueOrZero(const std::unordered_map<int, int>& values, int key) {
  const auto found = values.find(key);
  return found == values.end() ? 0 : found->second;
}

std::vector<KeyPressureRow> buildKeyPressureRows(const Result& result, int limit) {
  std::unordered_set<int> keys;
  for (const auto& entry : result.keyHits) keys.insert(entry.first);
  for (const auto& entry : result.keyColdMisses) keys.insert(entry.first);
  for (const auto& entry : result.keyReloadMisses) keys.insert(entry.first);
  for (const auto& entry : result.keyEvictions) keys.insert(entry.first);

  std::vector<KeyPressureRow> rows;
  rows.reserve(keys.size());
  for (int key : keys) {
    KeyPressureRow row;
    row.key = key;
    row.hits = mapValueOrZero(result.keyHits, key);
    row.coldMisses = mapValueOrZero(result.keyColdMisses, key);
    row.reloadMisses = mapValueOrZero(result.keyReloadMisses, key);
    row.evictions = mapValueOrZero(result.keyEvictions, key);
    row.totalMisses = row.coldMisses + row.reloadMisses;
    rows.push_back(row);
  }

  std::sort(rows.begin(), rows.end(), [](const KeyPressureRow& left, const KeyPressureRow& right) {
    if (left.reloadMisses != right.reloadMisses) return left.reloadMisses > right.reloadMisses;
    if (left.evictions != right.evictions) return left.evictions > right.evictions;
    if (left.totalMisses != right.totalMisses) return left.totalMisses > right.totalMisses;
    if (left.hits != right.hits) return left.hits > right.hits;
    return left.key < right.key;
  });

  if (limit > 0 && static_cast<int>(rows.size()) > limit) {
    rows.resize(static_cast<size_t>(limit));
  }
  return rows;
}

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
  if (policy == Policy::OPT) {
    Result result;
    if (capacity <= 0) return result;

    std::unordered_map<int, std::vector<int>> futureAccesses;
    for (int index = 0; index < static_cast<int>(trace.size()); ++index) {
      futureAccesses[trace[static_cast<size_t>(index)]].push_back(index);
    }

    std::vector<int> cache;
    cache.reserve(static_cast<size_t>(capacity));
    std::unordered_set<int> cacheSet;
    std::unordered_set<int> seenKeys;

    for (int index = 0; index < static_cast<int>(trace.size()); ++index) {
      const int key = trace[static_cast<size_t>(index)];
      std::vector<int>& offsets = futureAccesses[key];
      if (!offsets.empty() && offsets.front() == index) {
        offsets.erase(offsets.begin());
      }

      if (cacheSet.find(key) != cacheSet.end()) {
        result.hits += 1;
        result.keyHits[key] += 1;
        continue;
      }

      result.misses += 1;
      if (seenKeys.insert(key).second) {
        result.coldMisses += 1;
        result.keyColdMisses[key] += 1;
      } else {
        result.reloadMisses += 1;
        result.keyReloadMisses[key] += 1;
      }

      if (static_cast<int>(cache.size()) >= capacity) {
        size_t victimIndex = 0;
        int farthestUse = -1;
        for (size_t cacheIndex = 0; cacheIndex < cache.size(); ++cacheIndex) {
          const int cachedKey = cache[cacheIndex];
          const auto& nextUses = futureAccesses[cachedKey];
          const int nextUse = nextUses.empty() ? std::numeric_limits<int>::max() : nextUses.front();
          if (nextUse > farthestUse) {
            farthestUse = nextUse;
            victimIndex = cacheIndex;
          }
        }
        const int evictedKey = cache[victimIndex];
        cacheSet.erase(evictedKey);
        cache.erase(cache.begin() + static_cast<std::ptrdiff_t>(victimIndex));
        result.evictions += 1;
        result.keyEvictions[evictedKey] += 1;
      }

      cache.push_back(key);
      cacheSet.insert(key);
    }

    result.finalCache = cache;
    return result;
  }

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
      result.keyHits[key] += 1;
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
      result.keyColdMisses[key] += 1;
    } else {
      result.reloadMisses += 1;
      result.keyReloadMisses[key] += 1;
    }
    if (static_cast<int>(cache.size()) >= capacity) {
      const int evictedKey = cache.front();
      positions.erase(evictedKey);
      cache.erase(cache.begin());
      result.evictions += 1;
      result.keyEvictions[evictedKey] += 1;
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

void printKeyPressureSummary(const std::string& label, const Result& result, int topKeys) {
  const std::vector<KeyPressureRow> rows = buildKeyPressureRows(result, topKeys);
  if (rows.empty()) return;

  std::cout << label << " key pressure (top " << rows.size() << ")\n";
  std::cout << "  Key | Reload misses | Cold misses | Evictions | Hits\n";
  for (const KeyPressureRow& row : rows) {
    std::cout << std::setw(5) << row.key << " | "
              << std::setw(14) << row.reloadMisses << " | "
              << std::setw(11) << row.coldMisses << " | "
              << std::setw(9) << row.evictions << " | "
              << std::setw(4) << row.hits << "\n";
  }
}

int lruHitDelta(const Result& fifoResult, const Result& lruResult) {
  return lruResult.hits - fifoResult.hits;
}

const char* onlinePolicyLabel(const Result& fifoResult, const Result& lruResult) {
  const int delta = lruHitDelta(fifoResult, lruResult);
  return delta > 0 ? "LRU" : (delta < 0 ? "FIFO" : "TIE");
}

void printSweepSummary(const std::vector<int>& capacities, const std::vector<Result>& fifoResults,
                       const std::vector<Result>& lruResults, const std::vector<Result>& optResults,
                       int totalAccesses) {
  std::cout << "Capacity sweep\n";
  std::cout << "Cap | FIFO hit% | LRU hit% | OPT hit% | Online pick | LRU delta | LRU regret | FIFO regret\n";
  for (size_t i = 0; i < capacities.size(); ++i) {
    const double fifoRate =
        totalAccesses > 0 ? static_cast<double>(fifoResults[i].hits) / static_cast<double>(totalAccesses) : 0.0;
    const double lruRate =
        totalAccesses > 0 ? static_cast<double>(lruResults[i].hits) / static_cast<double>(totalAccesses) : 0.0;
    const double optRate =
        totalAccesses > 0 ? static_cast<double>(optResults[i].hits) / static_cast<double>(totalAccesses) : 0.0;
    const int lruRegret = optResults[i].hits - lruResults[i].hits;
    const int fifoRegret = optResults[i].hits - fifoResults[i].hits;
    std::cout << std::setw(3) << capacities[i] << " | " << std::setw(8) << std::fixed << std::setprecision(2)
              << (fifoRate * 100.0) << "% | " << std::setw(7) << (lruRate * 100.0) << "% | " << std::setw(7)
              << (optRate * 100.0) << "% | " << std::setw(11)
              << onlinePolicyLabel(fifoResults[i], lruResults[i]) << " | " << std::setw(9)
              << lruHitDelta(fifoResults[i], lruResults[i]) << " | " << std::setw(10) << lruRegret << " | "
              << std::setw(11) << fifoRegret << "\n";
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

double hitRate(const Result& result, int totalAccesses) {
  return totalAccesses > 0 ? static_cast<double>(result.hits) / static_cast<double>(totalAccesses) : 0.0;
}

const char* winnerLabel(const Result& fifoResult, const Result& lruResult, const Result& optResult);

PhasePolicyCounts countPhasePolicyConclusions(const std::vector<PhaseSummary>& phases) {
  PhasePolicyCounts counts;
  for (const PhaseSummary& phase : phases) {
    const int delta = lruHitDelta(phase.fifo, phase.lru);
    if (delta > 0) {
      counts.lruWins += 1;
    } else if (delta < 0) {
      counts.fifoWins += 1;
    } else {
      counts.ties += 1;
    }
  }
  return counts;
}

std::vector<PhaseSummary> buildPhaseSummaries(const std::vector<int>& trace, int capacity, int phaseWindow) {
  std::vector<PhaseSummary> phases;
  if (phaseWindow <= 0) return phases;

  int phaseIndex = 1;
  for (size_t start = 0; start < trace.size(); start += static_cast<size_t>(phaseWindow)) {
    const size_t end = std::min(trace.size(), start + static_cast<size_t>(phaseWindow));
    const std::vector<int> phaseTrace(trace.begin() + static_cast<std::ptrdiff_t>(start),
                                      trace.begin() + static_cast<std::ptrdiff_t>(end));
    PhaseSummary summary;
    summary.phaseIndex = phaseIndex++;
    summary.startAccess = static_cast<int>(start) + 1;
    summary.endAccess = static_cast<int>(end);
    summary.stats = analyzeTrace(phaseTrace);
    summary.fifo = runSimulation(phaseTrace, capacity, Policy::FIFO);
    summary.lru = runSimulation(phaseTrace, capacity, Policy::LRU);
    summary.opt = runSimulation(phaseTrace, capacity, Policy::OPT);
    phases.push_back(summary);
  }

  return phases;
}

void printPhaseSummary(const std::vector<PhaseSummary>& phases, int phaseCapacity) {
  if (phases.empty()) return;

  const PhasePolicyCounts counts = countPhasePolicyConclusions(phases);
  std::cout << "\nPhase-local summary (window " << (phases.front().endAccess - phases.front().startAccess + 1)
            << ", capacity " << phaseCapacity << ")\n";
  std::cout << "Online conclusions: LRU led " << counts.lruWins << ", FIFO led " << counts.fifoWins
            << ", tied " << counts.ties << ".\n";
  std::cout << "Phase | Access range | Unique keys | Reuse rate | FIFO hit% | LRU hit% | OPT hit% | Online pick | LRU delta\n";
  for (const PhaseSummary& phase : phases) {
    const int accesses = phase.endAccess - phase.startAccess + 1;
    const double reuseRate =
        accesses > 0 ? static_cast<double>(phase.stats.repeatedAccesses) / static_cast<double>(accesses) : 0.0;
    std::cout << std::setw(5) << phase.phaseIndex << " | "
              << std::setw(3) << phase.startAccess << "-" << std::setw(3) << phase.endAccess << " | "
              << std::setw(11) << phase.stats.uniqueKeys << " | "
              << std::setw(9) << std::fixed << std::setprecision(2) << (reuseRate * 100.0) << "% | "
              << std::setw(8) << (hitRate(phase.fifo, accesses) * 100.0) << "% | "
              << std::setw(7) << (hitRate(phase.lru, accesses) * 100.0) << "% | "
              << std::setw(7) << (hitRate(phase.opt, accesses) * 100.0) << "% | "
              << std::setw(11) << onlinePolicyLabel(phase.fifo, phase.lru) << " | "
              << std::setw(9) << lruHitDelta(phase.fifo, phase.lru) << "\n";
  }
}

const char* winnerLabel(const Result& fifoResult, const Result& lruResult, const Result& optResult) {
  const int bestHits = std::max({fifoResult.hits, lruResult.hits, optResult.hits});
  return bestHits == optResult.hits ? "OPT" : (bestHits == lruResult.hits ? "LRU" : "FIFO");
}

CommandLineOptions parseOptions(int argc, char* argv[]) {
  if (argc == 2 && std::string(argv[1]) == "--self-test") {
    CommandLineOptions options;
    options.selfTest = true;
    return options;
  }

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
    if (arg == "--markdown-out") {
      if (index + 1 >= argc) {
        throw std::invalid_argument("Missing Markdown output path after --markdown-out.");
      }
      options.markdownOutPath = argv[++index];
      continue;
    }
    if (arg == "--json-out") {
      if (index + 1 >= argc) {
        throw std::invalid_argument("Missing JSON output path after --json-out.");
      }
      options.jsonOutPath = argv[++index];
      continue;
    }
    if (arg == "--phase-window") {
      if (index + 1 >= argc) {
        throw std::invalid_argument("Missing integer value after --phase-window.");
      }
      options.phaseWindow = std::stoi(argv[++index]);
      if (options.phaseWindow <= 0) {
        throw std::invalid_argument("Phase window must be positive.");
      }
      continue;
    }
    if (arg == "--top-keys") {
      if (index + 1 >= argc) {
        throw std::invalid_argument("Missing integer value after --top-keys.");
      }
      options.topKeys = std::stoi(argv[++index]);
      if (options.topKeys <= 0) {
        throw std::invalid_argument("Top-key count must be positive.");
      }
      continue;
    }
    throw std::invalid_argument("Unknown argument: " + arg);
  }

  return options;
}

void writeCsvReport(const std::string& path, const std::vector<int>& capacities, const std::vector<Result>& fifoResults,
                    const std::vector<Result>& lruResults, const std::vector<Result>& optResults,
                    const TraceStats& stats, int totalAccesses) {
  const std::filesystem::path outputPath(path);
  if (!outputPath.parent_path().empty()) {
    std::filesystem::create_directories(outputPath.parent_path());
  }
  std::ofstream out(outputPath);
  if (!out.is_open()) {
    throw std::runtime_error("Could not open CSV output path: " + path);
  }

  const double reuseRate =
      totalAccesses > 0 ? static_cast<double>(stats.repeatedAccesses) / static_cast<double>(totalAccesses) : 0.0;

  out << "capacity,policy,hits,misses,cold_misses,reload_misses,evictions,hit_rate,online_choice,lru_hit_delta,"
         "unique_keys,reuse_rate,hottest_key,hottest_key_count\n";

  const auto writeRow = [&](int capacity, const char* policy, const Result& result, const char* onlineChoice,
                            int lruDelta) {
    const double hitRate = totalAccesses > 0 ? static_cast<double>(result.hits) / static_cast<double>(totalAccesses) : 0.0;
    out << capacity << ',' << policy << ',' << result.hits << ',' << result.misses << ',' << result.coldMisses << ','
        << result.reloadMisses << ',' << result.evictions << ',' << std::fixed << std::setprecision(4) << hitRate << ','
        << onlineChoice << ',' << lruDelta << ',' << stats.uniqueKeys << ',' << reuseRate << ',' << stats.hottestKey
        << ',' << stats.hottestKeyCount << "\n";
  };

  for (size_t i = 0; i < capacities.size(); ++i) {
    const char* onlineChoice = onlinePolicyLabel(fifoResults[i], lruResults[i]);
    const int lruDelta = lruHitDelta(fifoResults[i], lruResults[i]);
    writeRow(capacities[i], "FIFO", fifoResults[i], onlineChoice, lruDelta);
    writeRow(capacities[i], "LRU", lruResults[i], onlineChoice, lruDelta);
    writeRow(capacities[i], "OPT", optResults[i], onlineChoice, lruDelta);
  }
}

void writeMarkdownReport(const std::string& path, const std::vector<int>& capacities,
                         const std::vector<Result>& fifoResults, const std::vector<Result>& lruResults,
                         const std::vector<Result>& optResults, const TraceStats& stats, int totalAccesses,
                         const std::vector<PhaseSummary>& phaseSummaries, int phaseCapacity, int topKeys) {
  const std::filesystem::path outputPath(path);
  if (!outputPath.parent_path().empty()) {
    std::filesystem::create_directories(outputPath.parent_path());
  }

  std::ofstream out(outputPath);
  if (!out.is_open()) {
    throw std::runtime_error("Could not open Markdown output path: " + path);
  }

  const double reuseRate =
      totalAccesses > 0 ? static_cast<double>(stats.repeatedAccesses) / static_cast<double>(totalAccesses) : 0.0;

  out << "# Cache Policy Brief\n\n";
  out << "- Accesses: `" << totalAccesses << "`\n";
  out << "- Unique keys: `" << stats.uniqueKeys << "`\n";
  out << "- Reuse rate: `" << std::fixed << std::setprecision(2) << (reuseRate * 100.0) << "%`\n";
  out << "- Hottest key: `" << stats.hottestKey << "` with `" << stats.hottestKeyCount << "` accesses\n\n";

  out << "## Capacity sweep\n\n";
  out << "| Capacity | FIFO hit % | LRU hit % | OPT hit % | Online pick | LRU hit delta | LRU regret | FIFO regret |\n";
  out << "| --- | ---: | ---: | ---: | --- | ---: | ---: | ---: |\n";
  for (size_t i = 0; i < capacities.size(); ++i) {
    out << "| " << capacities[i] << " | "
        << std::fixed << std::setprecision(2) << (hitRate(fifoResults[i], totalAccesses) * 100.0) << "% | "
        << (hitRate(lruResults[i], totalAccesses) * 100.0) << "% | "
        << (hitRate(optResults[i], totalAccesses) * 100.0) << "% | "
        << onlinePolicyLabel(fifoResults[i], lruResults[i]) << " | "
        << lruHitDelta(fifoResults[i], lruResults[i]) << " | "
        << (optResults[i].hits - lruResults[i].hits) << " | "
        << (optResults[i].hits - fifoResults[i].hits) << " |\n";
  }

  if (!phaseSummaries.empty()) {
    const PhasePolicyCounts counts = countPhasePolicyConclusions(phaseSummaries);
    out << "\n## Phase-local summary\n\n";
    out << "Phase-local analysis resets the cache at each window so locality shifts are easier to compare.\n\n";
    out << "- Phase window: `" << (phaseSummaries.front().endAccess - phaseSummaries.front().startAccess + 1) << "` accesses\n";
    out << "- Phase capacity: `" << phaseCapacity << "`\n\n";
    out << "Online policy conclusions: LRU led `" << counts.lruWins << "` phase(s), FIFO led `"
        << counts.fifoWins << "`, and `" << counts.ties << "` tied. OPT remains the offline ceiling.\n\n";
    out << "| Phase | Access range | Unique keys | Reuse rate | FIFO hit % | LRU hit % | OPT hit % | Online pick | LRU hit delta |\n";
    out << "| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: |\n";
    for (const PhaseSummary& phase : phaseSummaries) {
      const int accesses = phase.endAccess - phase.startAccess + 1;
      const double reuseRate =
          accesses > 0 ? static_cast<double>(phase.stats.repeatedAccesses) / static_cast<double>(accesses) : 0.0;
      out << "| " << phase.phaseIndex << " | " << phase.startAccess << "-" << phase.endAccess << " | "
          << phase.stats.uniqueKeys << " | " << std::fixed << std::setprecision(2) << (reuseRate * 100.0) << "% | "
          << (hitRate(phase.fifo, accesses) * 100.0) << "% | "
          << (hitRate(phase.lru, accesses) * 100.0) << "% | "
          << (hitRate(phase.opt, accesses) * 100.0) << "% | "
          << onlinePolicyLabel(phase.fifo, phase.lru) << " | "
          << lruHitDelta(phase.fifo, phase.lru) << " |\n";
    }
  }

  out << "\n## Policy details\n\n";
  for (size_t i = 0; i < capacities.size(); ++i) {
    out << "### Capacity " << capacities[i] << "\n\n";
    out << "| Policy | Hits | Misses | Cold misses | Reload misses | Evictions | Final cache |\n";
    out << "| --- | ---: | ---: | ---: | ---: | ---: | --- |\n";

    const auto writePolicyRow = [&](const char* label, const Result& result) {
      out << "| " << label << " | " << result.hits << " | " << result.misses << " | " << result.coldMisses << " | "
          << result.reloadMisses << " | " << result.evictions << " | [";
      for (size_t index = 0; index < result.finalCache.size(); ++index) {
        if (index) out << ", ";
        out << result.finalCache[index];
      }
      out << "] |\n";
    };

    writePolicyRow("FIFO", fifoResults[i]);
    writePolicyRow("LRU", lruResults[i]);
    writePolicyRow("OPT", optResults[i]);
    out << "\n";

    const auto writeKeyPressureTable = [&](const char* label, const Result& result) {
      const std::vector<KeyPressureRow> rows = buildKeyPressureRows(result, topKeys);
      if (rows.empty()) return;
      out << "#### " << label << " key pressure\n\n";
      out << "| Key | Reload misses | Cold misses | Evictions | Hits |\n";
      out << "| --- | ---: | ---: | ---: | ---: |\n";
      for (const KeyPressureRow& row : rows) {
        out << "| " << row.key << " | " << row.reloadMisses << " | " << row.coldMisses << " | "
            << row.evictions << " | " << row.hits << " |\n";
      }
      out << "\n";
    };

    writeKeyPressureTable("FIFO", fifoResults[i]);
    writeKeyPressureTable("LRU", lruResults[i]);
    writeKeyPressureTable("OPT", optResults[i]);
  }
}

void writeJsonReport(const std::string& path, const std::vector<int>& capacities, const std::vector<Result>& fifoResults,
                     const std::vector<Result>& lruResults, const std::vector<Result>& optResults,
                     const TraceStats& stats, int totalAccesses, const std::vector<PhaseSummary>& phaseSummaries,
                     int phaseCapacity, int topKeys) {
  const std::filesystem::path outputPath(path);
  if (!outputPath.parent_path().empty()) {
    std::filesystem::create_directories(outputPath.parent_path());
  }

  std::ofstream out(outputPath);
  if (!out.is_open()) {
    throw std::runtime_error("Could not open JSON output path: " + path);
  }

  const double reuseRate =
      totalAccesses > 0 ? static_cast<double>(stats.repeatedAccesses) / static_cast<double>(totalAccesses) : 0.0;

  const auto writeFinalCache = [&](const Result& result) {
    out << "[";
    for (size_t i = 0; i < result.finalCache.size(); ++i) {
      if (i) out << ", ";
      out << result.finalCache[i];
    }
    out << "]";
  };

  const auto writeResultObject = [&](const Result& result, int accesses) {
    const std::vector<KeyPressureRow> keyPressure = buildKeyPressureRows(result, topKeys);
    out << "{"
        << "\"hits\":" << result.hits << ","
        << "\"misses\":" << result.misses << ","
        << "\"cold_misses\":" << result.coldMisses << ","
        << "\"reload_misses\":" << result.reloadMisses << ","
        << "\"evictions\":" << result.evictions << ","
        << "\"hit_rate\":" << std::fixed << std::setprecision(4) << hitRate(result, accesses) << ","
        << "\"final_cache\":";
    writeFinalCache(result);
    out << ",\"key_pressure\":[";
    for (size_t i = 0; i < keyPressure.size(); ++i) {
      const KeyPressureRow& row = keyPressure[i];
      if (i) out << ",";
      out << "{"
          << "\"key\":" << row.key << ","
          << "\"hits\":" << row.hits << ","
          << "\"cold_misses\":" << row.coldMisses << ","
          << "\"reload_misses\":" << row.reloadMisses << ","
          << "\"evictions\":" << row.evictions
          << "}";
    }
    out << "]}";
  };

  out << "{\n";
  out << "  \"trace\": {\n";
  out << "    \"accesses\": " << totalAccesses << ",\n";
  out << "    \"unique_keys\": " << stats.uniqueKeys << ",\n";
  out << "    \"reuse_rate\": " << std::fixed << std::setprecision(4) << reuseRate << ",\n";
  out << "    \"hottest_key\": " << stats.hottestKey << ",\n";
  out << "    \"hottest_key_count\": " << stats.hottestKeyCount << "\n";
  out << "  },\n";
  out << "  \"capacity_sweep\": [\n";
  for (size_t i = 0; i < capacities.size(); ++i) {
    out << "    {\n";
    out << "      \"capacity\": " << capacities[i] << ",\n";
    out << "      \"winner\": \"" << winnerLabel(fifoResults[i], lruResults[i], optResults[i]) << "\",\n";
    out << "      \"online_choice\": \"" << onlinePolicyLabel(fifoResults[i], lruResults[i]) << "\",\n";
    out << "      \"lru_hit_delta\": " << lruHitDelta(fifoResults[i], lruResults[i]) << ",\n";
    out << "      \"lru_regret\": " << (optResults[i].hits - lruResults[i].hits) << ",\n";
    out << "      \"fifo_regret\": " << (optResults[i].hits - fifoResults[i].hits) << ",\n";
    out << "      \"fifo\": ";
    writeResultObject(fifoResults[i], totalAccesses);
    out << ",\n      \"lru\": ";
    writeResultObject(lruResults[i], totalAccesses);
    out << ",\n      \"opt\": ";
    writeResultObject(optResults[i], totalAccesses);
    out << "\n    }";
    if (i + 1 != capacities.size()) out << ",";
    out << "\n";
  }
  out << "  ]";

  if (!phaseSummaries.empty()) {
    const PhasePolicyCounts counts = countPhasePolicyConclusions(phaseSummaries);
    out << ",\n  \"phase_local\": {\n";
    out << "    \"capacity\": " << phaseCapacity << ",\n";
    out << "    \"window\": " << (phaseSummaries.front().endAccess - phaseSummaries.front().startAccess + 1) << ",\n";
    out << "    \"policy_conclusions\": {\"lru\":" << counts.lruWins << ",\"fifo\":" << counts.fifoWins
        << ",\"tie\":" << counts.ties << "},\n";
    out << "    \"phases\": [\n";
    for (size_t i = 0; i < phaseSummaries.size(); ++i) {
      const PhaseSummary& phase = phaseSummaries[i];
      const int accesses = phase.endAccess - phase.startAccess + 1;
      const double phaseReuseRate =
          accesses > 0 ? static_cast<double>(phase.stats.repeatedAccesses) / static_cast<double>(accesses) : 0.0;
      out << "      {\n";
      out << "        \"phase\": " << phase.phaseIndex << ",\n";
      out << "        \"start_access\": " << phase.startAccess << ",\n";
      out << "        \"end_access\": " << phase.endAccess << ",\n";
      out << "        \"unique_keys\": " << phase.stats.uniqueKeys << ",\n";
      out << "        \"reuse_rate\": " << std::fixed << std::setprecision(4) << phaseReuseRate << ",\n";
      out << "        \"winner\": \"" << winnerLabel(phase.fifo, phase.lru, phase.opt) << "\",\n";
      out << "        \"online_choice\": \"" << onlinePolicyLabel(phase.fifo, phase.lru) << "\",\n";
      out << "        \"lru_hit_delta\": " << lruHitDelta(phase.fifo, phase.lru) << ",\n";
      out << "        \"fifo\": ";
      writeResultObject(phase.fifo, accesses);
      out << ",\n        \"lru\": ";
      writeResultObject(phase.lru, accesses);
      out << ",\n        \"opt\": ";
      writeResultObject(phase.opt, accesses);
      out << "\n      }";
      if (i + 1 != phaseSummaries.size()) out << ",";
      out << "\n";
    }
    out << "    ]\n";
    out << "  }\n";
  } else {
    out << "\n";
  }

  out << "}\n";
}

bool runSelfTest() {
  auto require = [](bool condition, const std::string& message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  };

  const std::vector<int> capacities = parseCapacities("4,2,4,3");
  require(capacities == std::vector<int>({2, 3, 4}), "Capacity parsing should sort and dedupe values.");

  std::istringstream traceStream(std::string("\xEF\xBB\xBF") + "1,2,3\n2 1");
  const std::vector<int> parsedTrace = parseTrace(traceStream);
  require(parsedTrace == std::vector<int>({1, 2, 3, 2, 1}), "Trace parsing should support BOM, commas, and spaces.");

  const std::vector<int> lruTrace = {1, 2, 1, 3, 1, 2};
  const Result lruResult = runSimulation(lruTrace, 2, Policy::LRU);
  require(lruResult.hits == 2 && lruResult.misses == 4, "LRU baseline counts changed unexpectedly.");
  require(lruResult.coldMisses == 3 && lruResult.reloadMisses == 1, "LRU miss classification changed unexpectedly.");
  require(mapValueOrZero(lruResult.keyHits, 1) == 2 && mapValueOrZero(lruResult.keyReloadMisses, 2) == 1,
          "Per-key hit/reload accounting changed unexpectedly.");
  require(mapValueOrZero(lruResult.keyEvictions, 2) == 1 && mapValueOrZero(lruResult.keyEvictions, 1) == 1,
          "Per-key eviction accounting changed unexpectedly.");

  const std::vector<int> anomalyTrace = {1, 2, 3, 4, 1, 2, 5, 1, 2, 3, 4, 5};
  const Result fifoThree = runSimulation(anomalyTrace, 3, Policy::FIFO);
  const Result fifoFour = runSimulation(anomalyTrace, 4, Policy::FIFO);
  require(fifoThree.misses == 9 && fifoFour.misses == 10, "FIFO Belady anomaly regression failed.");
  const std::vector<KeyPressureRow> pressureRows = buildKeyPressureRows(fifoThree, 2);
  require(pressureRows.size() == 2 && pressureRows.front().reloadMisses >= pressureRows.back().reloadMisses,
          "Key-pressure ranking should stay deterministic.");

  const std::vector<PhaseSummary> phases = buildPhaseSummaries(anomalyTrace, 3, 4);
  require(phases.size() == 3, "Phase summary count should match trace windowing.");
  require(phases.front().startAccess == 1 && phases.front().endAccess == 4, "Phase boundaries are incorrect.");

  const std::vector<int> multiPhaseTrace = {
      1, 2, 3, 1, 4, 1, 2, 3, 1, 4, 1, 2,
      1, 2, 3, 4, 1, 2, 5, 1, 2, 3, 4, 5,
      7, 8, 9, 7, 8, 9, 7, 8, 9, 7, 8, 9,
  };
  const std::vector<PhaseSummary> policyPhases = buildPhaseSummaries(multiPhaseTrace, 3, 12);
  require(policyPhases.size() == 3, "Multi-phase fixture should produce exactly three policy windows.");
  require(lruHitDelta(policyPhases[0].fifo, policyPhases[0].lru) == 1 &&
              std::string(onlinePolicyLabel(policyPhases[0].fifo, policyPhases[0].lru)) == "LRU",
          "The locality phase should recommend LRU by one hit.");
  require(lruHitDelta(policyPhases[1].fifo, policyPhases[1].lru) == -1 &&
              std::string(onlinePolicyLabel(policyPhases[1].fifo, policyPhases[1].lru)) == "FIFO",
          "The cyclic scan phase should recommend FIFO by one hit.");
  require(lruHitDelta(policyPhases[2].fifo, policyPhases[2].lru) == 0 &&
              policyPhases[2].fifo.hits == 9 && policyPhases[2].lru.hits == 9,
          "The fitting hot-set phase should keep FIFO and LRU tied at nine hits.");
  const PhasePolicyCounts conclusionCounts = countPhasePolicyConclusions(policyPhases);
  require(conclusionCounts.lruWins == 1 && conclusionCounts.fifoWins == 1 && conclusionCounts.ties == 1,
          "Phase conclusion aggregation should preserve one LRU win, one FIFO win, and one tie.");

  std::cout << "Self-test passed: parsing, simulation, key pressure, and multi-phase policy conclusions are stable.\n";
  return true;
}

int main(int argc, char* argv[]) {
  CommandLineOptions options;
  try {
    options = parseOptions(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "Usage: cache_policy_sim <trace_file> <cache_capacity|start-end|c1,c2,...> "
                 "[--csv-out report.csv] [--markdown-out report.md] [--json-out report.json] "
                 "[--phase-window accesses] [--top-keys count]\n"
                 "   or: cache_policy_sim --self-test\n";
    if (std::string(error.what()) != "Usage") {
      std::cerr << error.what() << "\n";
    }
    return 1;
  }

  if (options.selfTest) {
    try {
      runSelfTest();
      return 0;
    } catch (const std::exception& error) {
      std::cerr << "Self-test failed: " << error.what() << "\n";
      return 1;
    }
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
  std::vector<Result> optResults;
  fifoResults.reserve(options.capacities.size());
  lruResults.reserve(options.capacities.size());
  optResults.reserve(options.capacities.size());

  for (int capacity : options.capacities) {
    fifoResults.push_back(runSimulation(trace, capacity, Policy::FIFO));
    lruResults.push_back(runSimulation(trace, capacity, Policy::LRU));
    optResults.push_back(runSimulation(trace, capacity, Policy::OPT));
  }

  const int phaseCapacity = options.capacities.back();
  const std::vector<PhaseSummary> phaseSummaries = buildPhaseSummaries(trace, phaseCapacity, options.phaseWindow);

  if (!options.csvOutPath.empty()) {
    try {
      writeCsvReport(options.csvOutPath, options.capacities, fifoResults, lruResults, optResults, stats,
                     static_cast<int>(trace.size()));
      std::cout << "CSV report: " << options.csvOutPath << "\n\n";
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\n";
      return 1;
    }
  }

  if (!options.markdownOutPath.empty()) {
    try {
      writeMarkdownReport(options.markdownOutPath, options.capacities, fifoResults, lruResults, optResults, stats,
                          static_cast<int>(trace.size()), phaseSummaries, phaseCapacity, options.topKeys);
      std::cout << "Markdown report: " << options.markdownOutPath << "\n\n";
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\n";
      return 1;
    }
  }

  if (!options.jsonOutPath.empty()) {
    try {
      writeJsonReport(options.jsonOutPath, options.capacities, fifoResults, lruResults, optResults, stats,
                      static_cast<int>(trace.size()), phaseSummaries, phaseCapacity, options.topKeys);
      std::cout << "JSON report: " << options.jsonOutPath << "\n\n";
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\n";
      return 1;
    }
  }

  if (options.capacities.size() == 1) {
    printResult("FIFO", fifoResults[0], static_cast<int>(trace.size()));
    printKeyPressureSummary("FIFO", fifoResults[0], options.topKeys);
    std::cout << "\n";
    printResult("LRU", lruResults[0], static_cast<int>(trace.size()));
    printKeyPressureSummary("LRU", lruResults[0], options.topKeys);
    std::cout << "\n";
    printResult("OPT", optResults[0], static_cast<int>(trace.size()));
    printKeyPressureSummary("OPT", optResults[0], options.topKeys);

    const int hitDelta = lruResults[0].hits - fifoResults[0].hits;
    const int lruRegret = optResults[0].hits - lruResults[0].hits;
    const int fifoRegret = optResults[0].hits - fifoResults[0].hits;
    if (hitDelta > 0) {
      std::cout << "\nLRU gained " << hitDelta << " extra hits on this workload.\n";
    } else if (hitDelta < 0) {
      std::cout << "\nFIFO gained " << -hitDelta << " extra hits on this workload.\n";
    } else {
      std::cout << "\nBoth policies produced the same hit count on this workload.\n";
    }
    std::cout << "LRU regret vs OPT: " << lruRegret << " hit(s)\n";
    std::cout << "FIFO regret vs OPT: " << fifoRegret << " hit(s)\n";
    printPhaseSummary(phaseSummaries, phaseCapacity);
    return 0;
  }

  printSweepSummary(options.capacities, fifoResults, lruResults, optResults, static_cast<int>(trace.size()));
  printBeladyAlerts(options.capacities, fifoResults, lruResults);
  printPhaseSummary(phaseSummaries, phaseCapacity);

  return 0;
}
