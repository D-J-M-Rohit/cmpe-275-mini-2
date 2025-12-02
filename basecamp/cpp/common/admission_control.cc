#include "cpp/common/admission_control.h"
#include <iostream>

FairScheduler::FairScheduler(int max_inflight_per_team)
    : max_per_team_(max_inflight_per_team) {}

bool FairScheduler::TryAcquire(const std::string &team) {
  std::lock_guard<std::mutex> lock(mu_);

  int current = inflight_[team];
  if (current >= max_per_team_) {
    return false; // team at capacity
  }

  // DRR fairness check
  int &def = deficit_[team];

  // If this team was recently served and deficit is negative, reject
  if (last_served_ == team && def < 0) {
    return false;
  }

  // Acquire slot
  inflight_[team]++;
  def--; // decrement deficit
  last_served_ = team;

  // Reset deficit if it goes too negative
  if (def < -QUANTUM) {
    def = 0;
  }

  return true;
}

void FairScheduler::Release(const std::string &team) {
  std::lock_guard<std::mutex> lock(mu_);

  if (inflight_[team] > 0) {
    inflight_[team]--;
  }

  // Add quantum to deficit when releasing
  deficit_[team] += QUANTUM;

  // Cap deficit to avoid unbounded growth
  if (deficit_[team] > QUANTUM * 3) {
    deficit_[team] = QUANTUM * 3;
  }
}

int FairScheduler::GetInflight(const std::string &team) const {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = inflight_.find(team);
  if (it == inflight_.end()) {
    return 0;
  }
  return it->second;
}

// === AdmissionGuard ===

AdmissionGuard::AdmissionGuard(FairScheduler *sched, const std::string &team)
    : sched_(sched), team_(team), acquired_(false) {
  if (sched_) {
    acquired_ = sched_->TryAcquire(team_);
  }
}

AdmissionGuard::~AdmissionGuard() {
  if (acquired_ && sched_) {
    sched_->Release(team_);
  }
}
