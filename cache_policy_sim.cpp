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
  long long missBytes = 0;
  std::vector<int> finalCache;
  std::unordered_map<int, int> keyHits;
  std::unordered_map<int, int> keyColdMisses;
  std::unordered_map<int, int> keyReloadMisses;
  std::unordered_map<int, int> keyEvictions;
  std::unordered_map<int, long long> keyMissBytes;
};

using KeyBytes = std::unordered_map<int, long long>;

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
  Result continuousFifo;
  Result continuousLru;
  Result continuousOpt;
  Result warmupFifo;
  Result warmupLru;
  Result warmupOpt;
  Result steadyFifo;
  Result steadyLru;
  Result steadyOpt;
};

struct PhasePolicyCounts {
  int lruWins = 0;
  int fifoWins = 0;
  int ties = 0;
};

struct PhaseComparisonSummary {
  PhasePolicyCounts isolatedCounts;
  PhasePolicyCounts continuousCounts;
  int fifoCarryHitDelta = 0;
  int lruCarryHitDelta = 0;
  int optCarryHitDelta = 0;
  int changedOnlineConclusions = 0;
  PhasePolicyCounts steadyByteCounts;
  long long fifoWarmupMissBytes = 0;
  long long lruWarmupMissBytes = 0;
  long long fifoSteadyMissBytes = 0;
  long long lruSteadyMissBytes = 0;
};

struct CommandLineOptions {
  std::string traceFile;
  std::vector<int> capacities;
  std::string csvOutPath;
  std::string markdownOutPath;
  std::string jsonOutPath;
  std::string keyBytesPath;
  int phaseWindow = 0;
  int phaseWarmup = 0;
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
  long long missBytes = 0;
};

int mapValueOrZero(const std::unordered_map<int, int>& values, int key) {
  const auto found = values.find(key);
  return found == values.end() ? 0 : found->second;
}

long long mapValueOrZero(const std::unordered_map<int, long long>& values, int key) {
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
    row.missBytes = mapValueOrZero(result.keyMissBytes, key);
    rows.push_back(row);
  }

  std::sort(rows.begin(), rows.end(), [](const KeyPressureRow& left, const KeyPressureRow& right) {
    if (left.missBytes != right.missBytes) return left.missBytes > right.missBytes;
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

KeyBytes parseKeyBytes(std::istream& input) {
  KeyBytes keyBytes;
  std::string line;
  int lineNumber = 0;
  while (std::getline(input, line)) {
    lineNumber += 1;
    const size_t comment = line.find('#');
    if (comment != std::string::npos) line.erase(comment);
    std::replace(line.begin(), line.end(), ',', ' ');
    std::stringstream ss(line);
    int key = 0;
    long long bytes = 0;
    if (!(ss >> key)) continue;
    if (!(ss >> bytes) || bytes <= 0) {
      throw std::invalid_argument("Invalid key-byte row at line " + std::to_string(lineNumber) + ".");
    }
    std::string extra;
    if (ss >> extra) {
      throw std::invalid_argument("Unexpected value in key-byte row at line " + std::to_string(lineNumber) + ".");
    }
    if (!keyBytes.emplace(key, bytes).second) {
      throw std::invalid_argument("Duplicate key in key-byte file at line " + std::to_string(lineNumber) + ".");
    }
  }
  if (keyBytes.empty()) {
    throw std::invalid_argument("Key-byte file has no mappings.");
  }
  return keyBytes;
}

long long bytesForKey(const KeyBytes& keyBytes, int key) {
  if (keyBytes.empty()) return 0;
  const auto found = keyBytes.find(key);
  if (found == keyBytes.end()) {
    throw std::invalid_argument("No byte size provided for key " + std::to_string(key) + ".");
  }
  return found->second;
}

Result runSimulation(const std::vector<int>& trace, int capacity, Policy policy, int phaseWindow = 0,
                     std::vector<Result>* continuousPhases = nullptr, const KeyBytes& keyBytes = {},
                     int phaseWarmup = 0, std::vector<Result>* warmupPhases = nullptr,
                     std::vector<Result>* steadyPhases = nullptr) {
  Result phaseResult;
  Result warmupResult;
  Result steadyResult;
  const auto recordHit = [&](Result& result, int key) {
    result.hits += 1;
    result.keyHits[key] += 1;
  };
  const auto recordMiss = [&](Result& result, int key, bool isCold) {
    result.misses += 1;
    const long long bytes = bytesForKey(keyBytes, key);
    result.missBytes += bytes;
    result.keyMissBytes[key] += bytes;
    if (isCold) {
      result.coldMisses += 1;
      result.keyColdMisses[key] += 1;
    } else {
      result.reloadMisses += 1;
      result.keyReloadMisses[key] += 1;
    }
  };
  const auto recordEviction = [&](Result& result, int key) {
    result.evictions += 1;
    result.keyEvictions[key] += 1;
  };
  const auto finishAccess = [&](size_t index, const std::vector<int>& cache) {
    if (continuousPhases == nullptr || phaseWindow <= 0) return;
    if (warmupPhases != nullptr && steadyPhases != nullptr && phaseWarmup > 0 &&
        static_cast<int>(index % static_cast<size_t>(phaseWindow)) + 1 == phaseWarmup) {
      warmupResult.finalCache = cache;
    }
    const bool phaseEnded = (index + 1) % static_cast<size_t>(phaseWindow) == 0 || index + 1 == trace.size();
    if (!phaseEnded) return;
    phaseResult.finalCache = cache;
    continuousPhases->push_back(phaseResult);
    phaseResult = Result{};
    if (warmupPhases != nullptr && steadyPhases != nullptr && phaseWarmup > 0) {
      if (warmupResult.finalCache.empty()) warmupResult.finalCache = cache;
      steadyResult.finalCache = cache;
      warmupPhases->push_back(warmupResult);
      steadyPhases->push_back(steadyResult);
      warmupResult = Result{};
      steadyResult = Result{};
    }
  };
  const auto segmentAt = [&](size_t index) -> Result* {
    if (warmupPhases == nullptr || steadyPhases == nullptr || phaseWindow <= 0 || phaseWarmup <= 0) return nullptr;
    const int phaseOffset = static_cast<int>(index % static_cast<size_t>(phaseWindow));
    return phaseOffset < phaseWarmup ? &warmupResult : &steadyResult;
  };

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
        recordHit(result, key);
        if (continuousPhases != nullptr) recordHit(phaseResult, key);
        if (Result* segment = segmentAt(static_cast<size_t>(index))) recordHit(*segment, key);
        finishAccess(static_cast<size_t>(index), cache);
        continue;
      }

      const bool isCold = seenKeys.insert(key).second;
      recordMiss(result, key, isCold);
      if (continuousPhases != nullptr) recordMiss(phaseResult, key, isCold);
      if (Result* segment = segmentAt(static_cast<size_t>(index))) recordMiss(*segment, key, isCold);

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
        recordEviction(result, evictedKey);
        if (continuousPhases != nullptr) recordEviction(phaseResult, evictedKey);
        if (Result* segment = segmentAt(static_cast<size_t>(index))) recordEviction(*segment, evictedKey);
      }

      cache.push_back(key);
      cacheSet.insert(key);
      finishAccess(static_cast<size_t>(index), cache);
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

  for (size_t accessIndex = 0; accessIndex < trace.size(); ++accessIndex) {
    const int key = trace[accessIndex];
    auto found = positions.find(key);
    if (found != positions.end()) {
      recordHit(result, key);
      if (continuousPhases != nullptr) recordHit(phaseResult, key);
      if (Result* segment = segmentAt(accessIndex)) recordHit(*segment, key);
      if (policy == Policy::LRU) {
        const int index = found->second;
        const int value = cache[static_cast<size_t>(index)];
        cache.erase(cache.begin() + index);
        cache.push_back(value);
        for (size_t i = 0; i < cache.size(); ++i) {
          positions[cache[i]] = static_cast<int>(i);
        }
      }
      finishAccess(accessIndex, cache);
      continue;
    }

    const bool isCold = seenKeys.insert(key).second;
    recordMiss(result, key, isCold);
    if (continuousPhases != nullptr) recordMiss(phaseResult, key, isCold);
    if (Result* segment = segmentAt(accessIndex)) recordMiss(*segment, key, isCold);
    if (static_cast<int>(cache.size()) >= capacity) {
      const int evictedKey = cache.front();
      positions.erase(evictedKey);
      cache.erase(cache.begin());
      recordEviction(result, evictedKey);
      if (continuousPhases != nullptr) recordEviction(phaseResult, evictedKey);
      if (Result* segment = segmentAt(accessIndex)) recordEviction(*segment, evictedKey);
    }
    cache.push_back(key);
    positions[key] = static_cast<int>(cache.size() - 1);
    finishAccess(accessIndex, cache);
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

std::string formatBytes(long long bytes);

void printResult(const std::string& label, const Result& result, int totalAccesses, bool byteWeighted = false) {
  const double hitRate = totalAccesses > 0 ? static_cast<double>(result.hits) / totalAccesses : 0.0;
  std::cout << label << "\n";
  std::cout << "  Hits: " << result.hits << "\n";
  std::cout << "  Misses: " << result.misses << "\n";
  std::cout << "  Cold misses: " << result.coldMisses << "\n";
  std::cout << "  Reload misses: " << result.reloadMisses << "\n";
  if (byteWeighted) std::cout << "  Miss volume: " << formatBytes(result.missBytes) << "\n";
  std::cout << "  Evictions: " << result.evictions << "\n";
  std::cout << "  Hit rate: " << std::fixed << std::setprecision(2) << (hitRate * 100.0) << "%\n";
  std::cout << "  Final cache: [";
  for (size_t i = 0; i < result.finalCache.size(); ++i) {
    if (i) std::cout << ", ";
    std::cout << result.finalCache[i];
  }
  std::cout << "]\n";
}

void printKeyPressureSummary(const std::string& label, const Result& result, int topKeys, bool byteWeighted = false) {
  const std::vector<KeyPressureRow> rows = buildKeyPressureRows(result, topKeys);
  if (rows.empty()) return;

  std::cout << label << " key pressure (top " << rows.size() << ")\n";
  std::cout << "  Key | Reload misses | Cold misses | Evictions | Hits";
  if (byteWeighted) std::cout << " | Miss volume";
  std::cout << "\n";
  for (const KeyPressureRow& row : rows) {
    std::cout << std::setw(5) << row.key << " | "
              << std::setw(14) << row.reloadMisses << " | "
              << std::setw(11) << row.coldMisses << " | "
              << std::setw(9) << row.evictions << " | "
              << std::setw(4) << row.hits;
    if (byteWeighted) std::cout << " | " << formatBytes(row.missBytes);
    std::cout << "\n";
  }
}

int lruHitDelta(const Result& fifoResult, const Result& lruResult) {
  return lruResult.hits - fifoResult.hits;
}

const char* onlinePolicyLabel(const Result& fifoResult, const Result& lruResult) {
  const int delta = lruHitDelta(fifoResult, lruResult);
  return delta > 0 ? "LRU" : (delta < 0 ? "FIFO" : "TIE");
}

long long lruMissByteSavings(const Result& fifoResult, const Result& lruResult) {
  return fifoResult.missBytes - lruResult.missBytes;
}

const char* bytePolicyLabel(const Result& fifoResult, const Result& lruResult) {
  const long long savings = lruMissByteSavings(fifoResult, lruResult);
  return savings > 0 ? "LRU" : (savings < 0 ? "FIFO" : "TIE");
}

std::string formatBytes(long long bytes) {
  static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  const bool negative = bytes < 0;
  double value = static_cast<double>(negative ? -bytes : bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit < 4) {
    value /= 1024.0;
    unit += 1;
  }
  std::ostringstream out;
  if (negative) out << '-';
  out << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << ' ' << units[unit];
  return out.str();
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

void printByteSweepSummary(const std::vector<int>& capacities, const std::vector<Result>& fifoResults,
                           const std::vector<Result>& lruResults, const std::vector<Result>& optResults) {
  std::cout << "\nByte-weighted miss volume\n";
  std::cout << "Cap | FIFO volume | LRU volume | Hit-OPT vol | Byte pick | LRU savings\n";
  for (size_t i = 0; i < capacities.size(); ++i) {
    std::cout << std::setw(3) << capacities[i] << " | " << std::setw(11) << formatBytes(fifoResults[i].missBytes)
              << " | " << std::setw(10) << formatBytes(lruResults[i].missBytes) << " | " << std::setw(10)
              << formatBytes(optResults[i].missBytes) << " | " << std::setw(11)
              << bytePolicyLabel(fifoResults[i], lruResults[i]) << " | " << std::showpos
              << formatBytes(lruMissByteSavings(fifoResults[i], lruResults[i])) << std::noshowpos << "\n";
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

PhasePolicyCounts countPhasePolicyConclusions(const std::vector<PhaseSummary>& phases, bool continuous = false) {
  PhasePolicyCounts counts;
  for (const PhaseSummary& phase : phases) {
    const Result& fifo = continuous ? phase.continuousFifo : phase.fifo;
    const Result& lru = continuous ? phase.continuousLru : phase.lru;
    const int delta = lruHitDelta(fifo, lru);
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

PhaseComparisonSummary summarizePhaseComparison(const std::vector<PhaseSummary>& phases) {
  PhaseComparisonSummary summary;
  summary.isolatedCounts = countPhasePolicyConclusions(phases);
  summary.continuousCounts = countPhasePolicyConclusions(phases, true);
  for (const PhaseSummary& phase : phases) {
    summary.changedOnlineConclusions += std::string(onlinePolicyLabel(phase.fifo, phase.lru)) !=
                                        onlinePolicyLabel(phase.continuousFifo, phase.continuousLru);
    summary.fifoCarryHitDelta += phase.continuousFifo.hits - phase.fifo.hits;
    summary.lruCarryHitDelta += phase.continuousLru.hits - phase.lru.hits;
    summary.optCarryHitDelta += phase.continuousOpt.hits - phase.opt.hits;
    summary.fifoWarmupMissBytes += phase.warmupFifo.missBytes;
    summary.lruWarmupMissBytes += phase.warmupLru.missBytes;
    summary.fifoSteadyMissBytes += phase.steadyFifo.missBytes;
    summary.lruSteadyMissBytes += phase.steadyLru.missBytes;
    const long long savings = lruMissByteSavings(phase.steadyFifo, phase.steadyLru);
    if (savings > 0) {
      summary.steadyByteCounts.lruWins += 1;
    } else if (savings < 0) {
      summary.steadyByteCounts.fifoWins += 1;
    } else {
      summary.steadyByteCounts.ties += 1;
    }
  }
  return summary;
}

std::vector<PhaseSummary> buildPhaseSummaries(const std::vector<int>& trace, int capacity, int phaseWindow,
                                              const KeyBytes& keyBytes = {}, int phaseWarmup = 0) {
  std::vector<PhaseSummary> phases;
  if (phaseWindow <= 0) return phases;

  std::vector<Result> continuousFifo;
  std::vector<Result> continuousLru;
  std::vector<Result> continuousOpt;
  std::vector<Result> fifoWarmups;
  std::vector<Result> lruWarmups;
  std::vector<Result> optWarmups;
  std::vector<Result> fifoSteady;
  std::vector<Result> lruSteady;
  std::vector<Result> optSteady;
  runSimulation(trace, capacity, Policy::FIFO, phaseWindow, &continuousFifo, keyBytes, phaseWarmup,
                &fifoWarmups, &fifoSteady);
  runSimulation(trace, capacity, Policy::LRU, phaseWindow, &continuousLru, keyBytes, phaseWarmup,
                &lruWarmups, &lruSteady);
  runSimulation(trace, capacity, Policy::OPT, phaseWindow, &continuousOpt, keyBytes, phaseWarmup,
                &optWarmups, &optSteady);

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
    summary.fifo = runSimulation(phaseTrace, capacity, Policy::FIFO, 0, nullptr, keyBytes);
    summary.lru = runSimulation(phaseTrace, capacity, Policy::LRU, 0, nullptr, keyBytes);
    summary.opt = runSimulation(phaseTrace, capacity, Policy::OPT, 0, nullptr, keyBytes);
    const size_t continuousIndex = static_cast<size_t>(summary.phaseIndex - 1);
    summary.continuousFifo = continuousFifo.at(continuousIndex);
    summary.continuousLru = continuousLru.at(continuousIndex);
    summary.continuousOpt = continuousOpt.at(continuousIndex);
    if (phaseWarmup > 0) {
      summary.warmupFifo = fifoWarmups.at(continuousIndex);
      summary.warmupLru = lruWarmups.at(continuousIndex);
      summary.warmupOpt = optWarmups.at(continuousIndex);
      summary.steadyFifo = fifoSteady.at(continuousIndex);
      summary.steadyLru = lruSteady.at(continuousIndex);
      summary.steadyOpt = optSteady.at(continuousIndex);
    }
    phases.push_back(summary);
  }

  return phases;
}

void printPhaseSummary(const std::vector<PhaseSummary>& phases, int phaseCapacity, int phaseWarmup = 0,
                       bool byteWeighted = false) {
  if (phases.empty()) return;

  const PhaseComparisonSummary comparison = summarizePhaseComparison(phases);

  std::cout << "\nPhase analysis (window " << (phases.front().endAccess - phases.front().startAccess + 1)
            << ", capacity " << phaseCapacity << ")\n";
  std::cout << "Isolated conclusions: LRU led " << comparison.isolatedCounts.lruWins << ", FIFO led "
            << comparison.isolatedCounts.fifoWins << ", tied " << comparison.isolatedCounts.ties << ".\n";
  std::cout << "Continuous conclusions: LRU led " << comparison.continuousCounts.lruWins << ", FIFO led "
            << comparison.continuousCounts.fifoWins << ", tied " << comparison.continuousCounts.ties << ".\n";
  std::cout << "Carry-over effect: FIFO " << std::showpos << comparison.fifoCarryHitDelta << " hit(s), LRU "
            << comparison.lruCarryHitDelta << " hit(s) versus isolated resets; " << std::noshowpos
            << comparison.changedOnlineConclusions
            << " online conclusion(s) changed.\n";
  std::cout << "Phase | Access range | Reset pick | Live pick | Reset LRU delta | Live LRU delta | FIFO carry | LRU carry\n";
  for (const PhaseSummary& phase : phases) {
    std::cout << std::setw(5) << phase.phaseIndex << " | "
              << std::setw(3) << phase.startAccess << "-" << std::setw(3) << phase.endAccess << " | "
              << std::setw(10) << onlinePolicyLabel(phase.fifo, phase.lru) << " | "
              << std::setw(9) << onlinePolicyLabel(phase.continuousFifo, phase.continuousLru) << " | "
              << std::setw(15) << lruHitDelta(phase.fifo, phase.lru) << " | "
              << std::setw(14) << lruHitDelta(phase.continuousFifo, phase.continuousLru) << " | "
              << std::setw(10) << std::showpos << (phase.continuousFifo.hits - phase.fifo.hits) << " | "
              << std::setw(9) << (phase.continuousLru.hits - phase.lru.hits) << std::noshowpos << "\n";
  }

  if (phaseWarmup > 0) {
    std::cout << "\nWarm-up / steady-state decomposition (first " << phaseWarmup << " access(es) per phase)\n";
    if (byteWeighted) {
      std::cout << "Steady-state byte picks: LRU led " << comparison.steadyByteCounts.lruWins << ", FIFO led "
                << comparison.steadyByteCounts.fifoWins << ", tied " << comparison.steadyByteCounts.ties << ".\n";
      std::cout << "Aggregate miss volume: warm-up FIFO " << formatBytes(comparison.fifoWarmupMissBytes)
                << ", LRU " << formatBytes(comparison.lruWarmupMissBytes) << "; steady FIFO "
                << formatBytes(comparison.fifoSteadyMissBytes) << ", LRU "
                << formatBytes(comparison.lruSteadyMissBytes) << ".\n";
    }
    std::cout << "Phase | Warm FIFO miss | Warm LRU miss | Steady FIFO miss | Steady LRU miss | Steady pick\n";
    for (const PhaseSummary& phase : phases) {
      std::cout << std::setw(5) << phase.phaseIndex << " | ";
      if (byteWeighted) {
        std::cout << std::setw(14) << formatBytes(phase.warmupFifo.missBytes) << " | "
                  << std::setw(13) << formatBytes(phase.warmupLru.missBytes) << " | "
                  << std::setw(16) << formatBytes(phase.steadyFifo.missBytes) << " | "
                  << std::setw(15) << formatBytes(phase.steadyLru.missBytes) << " | "
                  << bytePolicyLabel(phase.steadyFifo, phase.steadyLru) << "\n";
      } else {
        std::cout << std::setw(14) << phase.warmupFifo.misses << " | " << std::setw(13)
                  << phase.warmupLru.misses << " | " << std::setw(16) << phase.steadyFifo.misses << " | "
                  << std::setw(15) << phase.steadyLru.misses << " | "
                  << onlinePolicyLabel(phase.steadyFifo, phase.steadyLru) << "\n";
      }
    }
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
    if (arg == "--key-bytes") {
      if (index + 1 >= argc) {
        throw std::invalid_argument("Missing mapping path after --key-bytes.");
      }
      options.keyBytesPath = argv[++index];
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
    if (arg == "--phase-warmup") {
      if (index + 1 >= argc) {
        throw std::invalid_argument("Missing integer value after --phase-warmup.");
      }
      options.phaseWarmup = std::stoi(argv[++index]);
      if (options.phaseWarmup <= 0) {
        throw std::invalid_argument("Phase warm-up length must be positive.");
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

  if (options.phaseWarmup > 0 && options.phaseWindow <= 0) {
    throw std::invalid_argument("--phase-warmup requires --phase-window.");
  }
  if (options.phaseWarmup >= options.phaseWindow && options.phaseWarmup > 0) {
    throw std::invalid_argument("Phase warm-up length must be smaller than the phase window.");
  }

  return options;
}

void writeCsvReport(const std::string& path, const std::vector<int>& capacities, const std::vector<Result>& fifoResults,
                    const std::vector<Result>& lruResults, const std::vector<Result>& optResults,
                    const TraceStats& stats, int totalAccesses, bool byteWeighted) {
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

  out << "capacity,policy,hits,misses,cold_misses,reload_misses,miss_bytes,byte_weighted,evictions,hit_rate,online_choice,"
         "lru_hit_delta,byte_choice,lru_miss_byte_savings,unique_keys,reuse_rate,hottest_key,hottest_key_count\n";

  const auto writeRow = [&](int capacity, const char* policy, const Result& result, const char* onlineChoice,
                            int lruDelta) {
    const double hitRate = totalAccesses > 0 ? static_cast<double>(result.hits) / static_cast<double>(totalAccesses) : 0.0;
    out << capacity << ',' << policy << ',' << result.hits << ',' << result.misses << ',' << result.coldMisses << ','
        << result.reloadMisses << ',' << result.missBytes << ',' << (byteWeighted ? "true" : "false") << ','
        << result.evictions << ',' << std::fixed
        << std::setprecision(4) << hitRate << ',' << onlineChoice << ',' << lruDelta << ',';
  };

  for (size_t i = 0; i < capacities.size(); ++i) {
    const char* onlineChoice = onlinePolicyLabel(fifoResults[i], lruResults[i]);
    const int lruDelta = lruHitDelta(fifoResults[i], lruResults[i]);
    const char* byteChoice = bytePolicyLabel(fifoResults[i], lruResults[i]);
    const long long byteSavings = lruMissByteSavings(fifoResults[i], lruResults[i]);
    const auto writeCompletedRow = [&](const char* policy, const Result& result) {
      writeRow(capacities[i], policy, result, onlineChoice, lruDelta);
      out << byteChoice << ',' << byteSavings << ',' << stats.uniqueKeys << ',' << reuseRate << ','
          << stats.hottestKey << ',' << stats.hottestKeyCount << "\n";
    };
    writeCompletedRow("FIFO", fifoResults[i]);
    writeCompletedRow("LRU", lruResults[i]);
    writeCompletedRow("OPT", optResults[i]);
  }
}

void writeMarkdownReport(const std::string& path, const std::vector<int>& capacities,
                         const std::vector<Result>& fifoResults, const std::vector<Result>& lruResults,
                         const std::vector<Result>& optResults, const TraceStats& stats, int totalAccesses,
                         const std::vector<PhaseSummary>& phaseSummaries, int phaseCapacity, int topKeys,
                         int phaseWarmup, bool byteWeighted) {
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

  if (byteWeighted) {
    out << "\n### Byte-weighted miss volume\n\n";
    out << "| Capacity | FIFO volume | LRU volume | Hit-OPT volume | Byte pick | LRU savings |\n";
    out << "| --- | ---: | ---: | ---: | --- | ---: |\n";
    for (size_t i = 0; i < capacities.size(); ++i) {
      out << "| " << capacities[i] << " | " << formatBytes(fifoResults[i].missBytes) << " | "
          << formatBytes(lruResults[i].missBytes) << " | " << formatBytes(optResults[i].missBytes) << " | "
          << bytePolicyLabel(fifoResults[i], lruResults[i]) << " | "
          << formatBytes(lruMissByteSavings(fifoResults[i], lruResults[i])) << " |\n";
    }
  }

  if (!phaseSummaries.empty()) {
    const PhaseComparisonSummary comparison = summarizePhaseComparison(phaseSummaries);
    out << "\n## Phase analysis\n\n";
    out << "The isolated view resets each window to compare locality regimes. The continuous view carries real cache "
           "state across boundaries to expose warm-start gains or transition penalties.\n\n";
    out << "- Phase window: `" << (phaseSummaries.front().endAccess - phaseSummaries.front().startAccess + 1) << "` accesses\n";
    out << "- Phase capacity: `" << phaseCapacity << "`\n";
    out << "- Isolated conclusions: LRU `" << comparison.isolatedCounts.lruWins << "`, FIFO `"
        << comparison.isolatedCounts.fifoWins << "`, tie `" << comparison.isolatedCounts.ties << "`\n";
    out << "- Continuous conclusions: LRU `" << comparison.continuousCounts.lruWins << "`, FIFO `"
        << comparison.continuousCounts.fifoWins << "`, tie `" << comparison.continuousCounts.ties << "`\n";
    out << "- Carry-over effect: FIFO `" << std::showpos << comparison.fifoCarryHitDelta << "` hit(s), LRU `"
        << comparison.lruCarryHitDelta << "` hit(s) versus isolated resets; `" << std::noshowpos
        << comparison.changedOnlineConclusions
        << "` online conclusion(s) changed\n\n";
    out << "| Phase | Access range | Isolated pick | Continuous pick | Isolated LRU delta | Continuous LRU delta | FIFO carry | LRU carry |\n";
    out << "| --- | --- | --- | --- | ---: | ---: | ---: | ---: |\n";
    for (const PhaseSummary& phase : phaseSummaries) {
      out << "| " << phase.phaseIndex << " | " << phase.startAccess << "-" << phase.endAccess << " | "
          << onlinePolicyLabel(phase.fifo, phase.lru) << " | "
          << onlinePolicyLabel(phase.continuousFifo, phase.continuousLru) << " | "
          << lruHitDelta(phase.fifo, phase.lru) << " | "
          << lruHitDelta(phase.continuousFifo, phase.continuousLru) << " | "
          << std::showpos << (phase.continuousFifo.hits - phase.fifo.hits) << " | "
          << (phase.continuousLru.hits - phase.lru.hits) << std::noshowpos << " |\n";
    }
    if (phaseWarmup > 0) {
      out << "\n### Warm-up and steady state\n\n";
      out << "The first `" << phaseWarmup << "` accesses of each live phase are classified as transition warm-up; "
             "the remaining accesses represent steady-state behavior. Policy advice below uses steady-state miss ";
      out << (byteWeighted ? "volume." : "count.") << "\n\n";
      if (byteWeighted) {
        out << "- Steady-state byte conclusions: LRU `" << comparison.steadyByteCounts.lruWins << "`, FIFO `"
            << comparison.steadyByteCounts.fifoWins << "`, tie `" << comparison.steadyByteCounts.ties << "`\n";
        out << "- Aggregate warm-up miss volume: FIFO `" << formatBytes(comparison.fifoWarmupMissBytes)
            << "`, LRU `" << formatBytes(comparison.lruWarmupMissBytes) << "`\n";
        out << "- Aggregate steady miss volume: FIFO `" << formatBytes(comparison.fifoSteadyMissBytes)
            << "`, LRU `" << formatBytes(comparison.lruSteadyMissBytes) << "`\n\n";
      }
      out << "| Phase | Warm FIFO miss | Warm LRU miss | Steady FIFO miss | Steady LRU miss | Steady pick |\n";
      out << "| --- | ---: | ---: | ---: | ---: | --- |\n";
      for (const PhaseSummary& phase : phaseSummaries) {
        if (byteWeighted) {
          out << "| " << phase.phaseIndex << " | " << formatBytes(phase.warmupFifo.missBytes) << " | "
              << formatBytes(phase.warmupLru.missBytes) << " | " << formatBytes(phase.steadyFifo.missBytes)
              << " | " << formatBytes(phase.steadyLru.missBytes) << " | "
              << bytePolicyLabel(phase.steadyFifo, phase.steadyLru) << " |\n";
        } else {
          out << "| " << phase.phaseIndex << " | " << phase.warmupFifo.misses << " | "
              << phase.warmupLru.misses << " | " << phase.steadyFifo.misses << " | "
              << phase.steadyLru.misses << " | " << onlinePolicyLabel(phase.steadyFifo, phase.steadyLru)
              << " |\n";
        }
      }
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
      out << "| Key | Reload misses | Cold misses | Evictions | Hits";
      if (byteWeighted) out << " | Miss volume";
      out << " |\n| --- | ---: | ---: | ---: | ---:";
      if (byteWeighted) out << " | ---:";
      out << " |\n";
      for (const KeyPressureRow& row : rows) {
        out << "| " << row.key << " | " << row.reloadMisses << " | " << row.coldMisses << " | "
            << row.evictions << " | " << row.hits;
        if (byteWeighted) out << " | " << formatBytes(row.missBytes);
        out << " |\n";
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
                     int phaseCapacity, int topKeys, int phaseWarmup, bool byteWeighted) {
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
        << "\"miss_bytes\":" << result.missBytes << ","
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
          << "\"miss_bytes\":" << row.missBytes << ","
          << "\"evictions\":" << row.evictions
          << "}";
    }
    out << "]}";
  };

  out << "{\n";
  out << "  \"trace\": {\n";
  out << "    \"accesses\": " << totalAccesses << ",\n";
  out << "    \"byte_weighted\": " << (byteWeighted ? "true" : "false") << ",\n";
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
    out << "      \"byte_choice\": \"" << bytePolicyLabel(fifoResults[i], lruResults[i]) << "\",\n";
    out << "      \"lru_miss_byte_savings\": " << lruMissByteSavings(fifoResults[i], lruResults[i]) << ",\n";
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
    const PhaseComparisonSummary comparison = summarizePhaseComparison(phaseSummaries);
    out << ",\n  \"phase_local\": {\n";
    out << "    \"capacity\": " << phaseCapacity << ",\n";
    out << "    \"window\": " << (phaseSummaries.front().endAccess - phaseSummaries.front().startAccess + 1) << ",\n";
    out << "    \"warmup_accesses\": " << phaseWarmup << ",\n";
    out << "    \"policy_conclusions\": {\"lru\":" << comparison.isolatedCounts.lruWins << ",\"fifo\":"
        << comparison.isolatedCounts.fifoWins << ",\"tie\":" << comparison.isolatedCounts.ties << "},\n";
    out << "    \"continuous_policy_conclusions\": {\"lru\":" << comparison.continuousCounts.lruWins
        << ",\"fifo\":" << comparison.continuousCounts.fifoWins << ",\"tie\":"
        << comparison.continuousCounts.ties << "},\n";
    out << "    \"carry_effect\": {\"fifo_hit_delta\":" << comparison.fifoCarryHitDelta
        << ",\"lru_hit_delta\":" << comparison.lruCarryHitDelta << ",\"opt_hit_delta\":"
        << comparison.optCarryHitDelta << ",\"changed_online_conclusions\":"
        << comparison.changedOnlineConclusions << "},\n";
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
      out << ",\n        \"continuous\": {\n";
      out << "          \"winner\": \"" << winnerLabel(phase.continuousFifo, phase.continuousLru, phase.continuousOpt)
          << "\",\n";
      out << "          \"online_choice\": \"" << onlinePolicyLabel(phase.continuousFifo, phase.continuousLru)
          << "\",\n";
      out << "          \"lru_hit_delta\": " << lruHitDelta(phase.continuousFifo, phase.continuousLru) << ",\n";
      out << "          \"fifo\": ";
      writeResultObject(phase.continuousFifo, accesses);
      out << ",\n          \"lru\": ";
      writeResultObject(phase.continuousLru, accesses);
      out << ",\n          \"opt\": ";
      writeResultObject(phase.continuousOpt, accesses);
      out << "\n        },\n";
      out << "        \"carry_effect\": {\"fifo_hit_delta\":"
          << (phase.continuousFifo.hits - phase.fifo.hits) << ",\"lru_hit_delta\":"
          << (phase.continuousLru.hits - phase.lru.hits) << ",\"opt_hit_delta\":"
          << (phase.continuousOpt.hits - phase.opt.hits) << "}";
      if (phaseWarmup > 0) {
        const int phaseAccesses = phase.endAccess - phase.startAccess + 1;
        const int warmupAccesses = std::min(phaseWarmup, phaseAccesses);
        const int steadyAccesses = phaseAccesses - warmupAccesses;
        out << ",\n        \"transition\": {\n";
        out << "          \"steady_byte_choice\": \"" << bytePolicyLabel(phase.steadyFifo, phase.steadyLru)
            << "\",\n";
        out << "          \"warmup\": {\"accesses\":" << warmupAccesses << ",\"fifo\":";
        writeResultObject(phase.warmupFifo, warmupAccesses);
        out << ",\"lru\":";
        writeResultObject(phase.warmupLru, warmupAccesses);
        out << ",\"opt\":";
        writeResultObject(phase.warmupOpt, warmupAccesses);
        out << "},\n          \"steady_state\": {\"accesses\":" << steadyAccesses << ",\"fifo\":";
        writeResultObject(phase.steadyFifo, steadyAccesses);
        out << ",\"lru\":";
        writeResultObject(phase.steadyLru, steadyAccesses);
        out << ",\"opt\":";
        writeResultObject(phase.steadyOpt, steadyAccesses);
        out << "}\n        }";
      }
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

  std::istringstream keyByteStream("# key,bytes\n1,4096\n2 16384\n3,1048576 # object\n");
  const KeyBytes parsedKeyBytes = parseKeyBytes(keyByteStream);
  require(parsedKeyBytes.size() == 3 && bytesForKey(parsedKeyBytes, 3) == 1048576,
          "Key-byte parsing should support CSV, whitespace, and comments.");

  const std::vector<int> lruTrace = {1, 2, 1, 3, 1, 2};
  const Result lruResult = runSimulation(lruTrace, 2, Policy::LRU);
  require(lruResult.hits == 2 && lruResult.misses == 4, "LRU baseline counts changed unexpectedly.");
  require(lruResult.missBytes == 0, "Unweighted runs should not invent byte volume.");
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
  const PhasePolicyCounts continuousConclusionCounts = countPhasePolicyConclusions(policyPhases, true);
  require(continuousConclusionCounts.lruWins == 2 && continuousConclusionCounts.fifoWins == 0 &&
              continuousConclusionCounts.ties == 1,
          "Continuous phase conclusions should preserve two LRU wins and one tie.");
  require(policyPhases[1].continuousFifo.hits - policyPhases[1].fifo.hits == 2 &&
              policyPhases[1].continuousLru.hits - policyPhases[1].lru.hits == 4,
          "The transition phase should quantify FIFO +2 and LRU +4 warm-state hits.");
  require(std::string(onlinePolicyLabel(policyPhases[1].continuousFifo, policyPhases[1].continuousLru)) == "LRU",
          "Carrying live state should reverse the isolated FIFO recommendation in phase two.");
  require(policyPhases[1].continuousLru.coldMisses == 1 && policyPhases[1].continuousLru.reloadMisses == 5,
          "Continuous miss classification should retain global seen-key history.");
  const PhaseComparisonSummary comparison = summarizePhaseComparison(policyPhases);
  require(comparison.fifoCarryHitDelta == 2 && comparison.lruCarryHitDelta == 4 &&
              comparison.optCarryHitDelta == 3 && comparison.changedOnlineConclusions == 1,
          "Aggregate carry-over effects should match the frozen three-phase contract.");
  const Result fullFifo = runSimulation(multiPhaseTrace, 3, Policy::FIFO);
  const Result fullLru = runSimulation(multiPhaseTrace, 3, Policy::LRU);
  int continuousFifoHits = 0;
  int continuousLruHits = 0;
  for (const PhaseSummary& phase : policyPhases) {
    continuousFifoHits += phase.continuousFifo.hits;
    continuousLruHits += phase.continuousLru.hits;
  }
  require(continuousFifoHits == fullFifo.hits && continuousLruHits == fullLru.hits,
          "Continuous phase hit totals should reconcile with the full-trace simulations.");

  const KeyBytes weightedBytes = {
      {1, 4096}, {2, 16384}, {3, 1048576}, {4, 8388608},
      {5, 67108864}, {7, 65536}, {8, 131072}, {9, 262144},
  };
  const Result weightedFifo = runSimulation(multiPhaseTrace, 3, Policy::FIFO, 0, nullptr, weightedBytes);
  const Result weightedLru = runSimulation(multiPhaseTrace, 3, Policy::LRU, 0, nullptr, weightedBytes);
  require(weightedFifo.missBytes == 105398272 && weightedLru.missBytes == 164098048,
          "Byte-weighted miss totals should match the skewed-object fixture.");
  require(std::string(onlinePolicyLabel(weightedFifo, weightedLru)) == "LRU" &&
              std::string(bytePolicyLabel(weightedFifo, weightedLru)) == "FIFO",
          "The fixture should preserve the count-versus-byte recommendation reversal.");
  const std::vector<PhaseSummary> weightedPhases =
      buildPhaseSummaries(multiPhaseTrace, 3, 12, weightedBytes, 3);
  const PhaseComparisonSummary weightedComparison = summarizePhaseComparison(weightedPhases);
  require(weightedPhases[1].warmupFifo.missBytes == 1048576 &&
              weightedPhases[1].steadyFifo.missBytes == 84955136 &&
              weightedPhases[1].steadyLru.missBytes == 143659008,
          "Warm-up and steady-state miss volume should stay frozen across the working-set shift.");
  require(weightedComparison.steadyByteCounts.lruWins == 1 &&
              weightedComparison.steadyByteCounts.fifoWins == 1 &&
              weightedComparison.steadyByteCounts.ties == 1,
          "Steady-state byte recommendations should cover one LRU, one FIFO, and one tie phase.");

  std::cout << "Self-test passed: parsing, simulation, byte-weighted cost, and phase-transition contracts are stable.\n";
  return true;
}

int main(int argc, char* argv[]) {
  CommandLineOptions options;
  try {
    options = parseOptions(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "Usage: cache_policy_sim <trace_file> <cache_capacity|start-end|c1,c2,...> "
                 "[--csv-out report.csv] [--markdown-out report.md] [--json-out report.json] "
                 "[--key-bytes key-bytes.csv] [--phase-window accesses] [--phase-warmup accesses] "
                 "[--top-keys count]\n"
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

  KeyBytes keyBytes;
  if (!options.keyBytesPath.empty()) {
    std::ifstream keyBytesFile(options.keyBytesPath);
    if (!keyBytesFile.is_open()) {
      std::cerr << "Could not open key-byte file: " << options.keyBytesPath << "\n";
      return 1;
    }
    try {
      keyBytes = parseKeyBytes(keyBytesFile);
      for (int key : trace) (void)bytesForKey(keyBytes, key);
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\n";
      return 1;
    }
  }
  const bool byteWeighted = !keyBytes.empty();

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
  if (byteWeighted) {
    std::cout << "  Miss weighting: bytes from " << options.keyBytesPath << "\n\n";
  }

  std::vector<Result> fifoResults;
  std::vector<Result> lruResults;
  std::vector<Result> optResults;
  fifoResults.reserve(options.capacities.size());
  lruResults.reserve(options.capacities.size());
  optResults.reserve(options.capacities.size());

  for (int capacity : options.capacities) {
    fifoResults.push_back(runSimulation(trace, capacity, Policy::FIFO, 0, nullptr, keyBytes));
    lruResults.push_back(runSimulation(trace, capacity, Policy::LRU, 0, nullptr, keyBytes));
    optResults.push_back(runSimulation(trace, capacity, Policy::OPT, 0, nullptr, keyBytes));
  }

  const int phaseCapacity = options.capacities.back();
  const std::vector<PhaseSummary> phaseSummaries =
      buildPhaseSummaries(trace, phaseCapacity, options.phaseWindow, keyBytes, options.phaseWarmup);

  if (!options.csvOutPath.empty()) {
    try {
      writeCsvReport(options.csvOutPath, options.capacities, fifoResults, lruResults, optResults, stats,
                     static_cast<int>(trace.size()), byteWeighted);
      std::cout << "CSV report: " << options.csvOutPath << "\n\n";
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\n";
      return 1;
    }
  }

  if (!options.markdownOutPath.empty()) {
    try {
      writeMarkdownReport(options.markdownOutPath, options.capacities, fifoResults, lruResults, optResults, stats,
                          static_cast<int>(trace.size()), phaseSummaries, phaseCapacity, options.topKeys,
                          options.phaseWarmup, byteWeighted);
      std::cout << "Markdown report: " << options.markdownOutPath << "\n\n";
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\n";
      return 1;
    }
  }

  if (!options.jsonOutPath.empty()) {
    try {
      writeJsonReport(options.jsonOutPath, options.capacities, fifoResults, lruResults, optResults, stats,
                      static_cast<int>(trace.size()), phaseSummaries, phaseCapacity, options.topKeys,
                      options.phaseWarmup, byteWeighted);
      std::cout << "JSON report: " << options.jsonOutPath << "\n\n";
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\n";
      return 1;
    }
  }

  if (options.capacities.size() == 1) {
    printResult("FIFO", fifoResults[0], static_cast<int>(trace.size()), byteWeighted);
    printKeyPressureSummary("FIFO", fifoResults[0], options.topKeys, byteWeighted);
    std::cout << "\n";
    printResult("LRU", lruResults[0], static_cast<int>(trace.size()), byteWeighted);
    printKeyPressureSummary("LRU", lruResults[0], options.topKeys, byteWeighted);
    std::cout << "\n";
    printResult("OPT", optResults[0], static_cast<int>(trace.size()), byteWeighted);
    printKeyPressureSummary("OPT", optResults[0], options.topKeys, byteWeighted);

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
    if (byteWeighted) printByteSweepSummary(options.capacities, fifoResults, lruResults, optResults);
    printPhaseSummary(phaseSummaries, phaseCapacity, options.phaseWarmup, byteWeighted);
    return 0;
  }

  printSweepSummary(options.capacities, fifoResults, lruResults, optResults, static_cast<int>(trace.size()));
  if (byteWeighted) printByteSweepSummary(options.capacities, fifoResults, lruResults, optResults);
  printBeladyAlerts(options.capacities, fifoResults, lruResults);
  printPhaseSummary(phaseSummaries, phaseCapacity, options.phaseWarmup, byteWeighted);

  return 0;
}
