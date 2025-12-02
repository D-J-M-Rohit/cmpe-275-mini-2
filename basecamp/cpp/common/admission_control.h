#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <string>

// Deficit Round Robin scheduler for fairness between teams
class FairScheduler {
public:
  explicit FairScheduler(int max_inflight_per_team);

  // Try to acquire slot for team, returns false if rejected
  bool TryAcquire(const std::string &team);

  // Release slot
  void Release(const std::string &team);

  // Get current inflight count for team
  int GetInflight(const std::string &team) const;

private:
  const int max_per_team_;
  mutable std::mutex mu_;
  std::map<std::string, int> inflight_; // team -> current count
  std::map<std::string, int> deficit_;  // DRR deficit counter
  std::string last_served_;             // for round-robin

  static constexpr int QUANTUM = 5; // DRR quantum
};

// RAII guard for admission control
class AdmissionGuard {
public:
  AdmissionGuard(FairScheduler *sched, const std::string &team);
  ~AdmissionGuard();

  // Disable copy/move
  AdmissionGuard(const AdmissionGuard &) = delete;
  AdmissionGuard &operator=(const AdmissionGuard &) = delete;

  bool Acquired() const { return acquired_; }

private:
  FairScheduler *sched_;
  std::string team_;
  bool acquired_;
};
