/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2018 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/

#include "mcts/search.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include "mcts/node.h"
#include "neural/cache.h"
#include "neural/encoder.h"
#include "utils/random.h"

int64_t bonus_time_ms = -1;

namespace lczero {

const char* Search::kMiniBatchSizeStr = "Minibatch size for NN inference";
const char* Search::kMaxPrefetchBatchStr = "Max prefetch nodes, per NN call";
const char* Search::kCpuctStr = "Cpuct MCTS option";
const char* Search::kTemperatureStr = "Initial temperature";
const char* Search::kTempDecayMovesStr = "Moves with temperature decay";
const char* Search::kNoiseStr = "Add Dirichlet noise at root node";
const char* Search::kVerboseStatsStr = "Display verbose move stats";
const char* Search::kAggressiveTimePruningStr =
    "Aversion to search if change unlikely";
const char* Search::kFpuReductionStr = "First Play Urgency Reduction";
const char* Search::kCacheHistoryLengthStr =
    "Length of history to include in cache";
const char* Search::kPolicySoftmaxTempStr = "Policy softmax temperature";
const char* Search::kAllowedNodeCollisionsStr =
    "Allowed node collisions, per batch";
const char* Search::kOutOfOrderEvalStr = "Out-of-order cache backpropagation";
const char* Search::kStickyCheckmateStr = "Ignore alternatives to checkmate";

namespace {
const int kSmartPruningToleranceNodes = 100;
const int kSmartPruningToleranceMs = 200;
// Maximum delay between outputting "uci info" when nothing interesting happens.
const int kUciInfoMinimumFrequencyMs = 5000;
}  // namespace

void Search::PopulateUciParams(OptionsParser* options) {
  // Here the "safe defaults" are listed.
  // Many of them are overridden with optimized defaults in engine.cc and
  // tournament.cc

  options->Add<IntOption>(kMiniBatchSizeStr, 1, 1024, "minibatch-size") = 1;
  options->Add<IntOption>(kMaxPrefetchBatchStr, 0, 1024, "max-prefetch") = 32;
  options->Add<FloatOption>(kCpuctStr, 0.0f, 100.0f, "cpuct") = 1.2f;
  options->Add<FloatOption>(kTemperatureStr, 0.0f, 100.0f, "temperature") =
      0.0f;
  options->Add<IntOption>(kTempDecayMovesStr, 0, 100, "tempdecay-moves") = 0;
  options->Add<BoolOption>(kNoiseStr, "noise", 'n') = false;
  options->Add<BoolOption>(kVerboseStatsStr, "verbose-move-stats") = false;
  options->Add<FloatOption>(kAggressiveTimePruningStr, 0.0f, 10.0f,
                            "futile-search-aversion") = 1.33f;
  options->Add<FloatOption>(kFpuReductionStr, -100.0f, 100.0f,
                            "fpu-reduction") = 0.0f;
  options->Add<IntOption>(kCacheHistoryLengthStr, 0, 7,
                          "cache-history-length") = 7;
  options->Add<FloatOption>(kPolicySoftmaxTempStr, 0.1f, 10.0f,
                            "policy-softmax-temp") = 1.0f;
  options->Add<IntOption>(kAllowedNodeCollisionsStr, 0, 1024,
                          "allowed-node-collisions") = 0;
  options->Add<BoolOption>(kOutOfOrderEvalStr, "out-of-order-eval") = false;
  options->Add<BoolOption>(kStickyCheckmateStr, "sticky-checkmate") = false;
}

Search::Search(const NodeTree& tree, Network* network,
               BestMoveInfo::Callback best_move_callback,
               ThinkingInfo::Callback info_callback, const SearchLimits& limits,
               const OptionsDict& options, NNCache* cache,
               SyzygyTablebase* syzygy_tb)
    : root_node_(tree.GetCurrentHead()),
      cache_(cache),
      syzygy_tb_(syzygy_tb),
      played_history_(tree.GetPositionHistory()),
      network_(network),
      limits_(limits),
      start_time_(std::chrono::steady_clock::now()),
      initial_visits_(root_node_->GetN()),
      best_move_callback_(best_move_callback),
      info_callback_(info_callback),
      kMiniBatchSize(options.Get<int>(kMiniBatchSizeStr)),
      kMaxPrefetchBatch(options.Get<int>(kMaxPrefetchBatchStr)),
      kCpuct(options.Get<float>(kCpuctStr)),
      kTemperature(options.Get<float>(kTemperatureStr)),
      kTempDecayMoves(options.Get<int>(kTempDecayMovesStr)),
      kNoise(options.Get<bool>(kNoiseStr)),
      kVerboseStats(options.Get<bool>(kVerboseStatsStr)),
      kAggressiveTimePruning(options.Get<float>(kAggressiveTimePruningStr)),
      kFpuReduction(options.Get<float>(kFpuReductionStr)),
      kCacheHistoryLength(options.Get<int>(kCacheHistoryLengthStr)),
      kPolicySoftmaxTemp(options.Get<float>(kPolicySoftmaxTempStr)),
      kAllowedNodeCollisions(options.Get<int>(kAllowedNodeCollisionsStr)),
      kOutOfOrderEval(options.Get<bool>(kOutOfOrderEvalStr)),
      kStickyCheckmate(options.Get<bool>(kStickyCheckmateStr)) {}

namespace {
void ApplyDirichletNoise(Node* node, float eps, double alpha) {
  float total = 0;
  std::vector<float> noise;

  for (int i = 0; i < node->GetNumEdges(); ++i) {
    float eta = Random::Get().GetGamma(alpha, 1.0);
    noise.emplace_back(eta);
    total += eta;
  }

  if (total < std::numeric_limits<float>::min()) return;

  int noise_idx = 0;
  for (const auto& child : node->Edges()) {
    auto* edge = child.edge();
    edge->SetP(edge->GetP() * (1 - eps) + eps * noise[noise_idx++] / total);
  }
}
}  // namespace

void Search::SendUciInfo() REQUIRES(nodes_mutex_) {
  if (!best_move_edge_) return;
  last_outputted_best_move_edge_ = best_move_edge_.edge();
  uci_info_.depth = cum_depth_ / (total_playouts_ ? total_playouts_ : 1);
  uci_info_.seldepth = max_depth_;
  uci_info_.time = GetTimeSinceStart();
  uci_info_.nodes = total_playouts_ + initial_visits_;
  uci_info_.hashfull =
      cache_->GetSize() * 1000LL / std::max(cache_->GetCapacity(), 1);
  uci_info_.nps =
      uci_info_.time ? (total_playouts_ * 1000 / uci_info_.time) : 0;
  uci_info_.score = 290.680623072 * tan(1.548090806 * best_move_edge_.GetQ(0));
  uci_info_.tb_hits = tb_hits_.load(std::memory_order_acquire);
  uci_info_.pv.clear();

  bool flip = played_history_.IsBlackToMove();
  for (auto iter = best_move_edge_; iter;
       iter = GetBestChildNoTemperature(iter.node()), flip = !flip) {
    uci_info_.pv.push_back(iter.GetMove(flip));
    if (!iter.node()) break;  // Last edge was dangling, cannot continue.
  }
  uci_info_.comment.clear();
  info_callback_(uci_info_);
}

// Decides whether anything important changed in stats and new info should be
// shown to a user.
void Search::MaybeOutputInfo() {
  SharedMutex::Lock lock(nodes_mutex_);
  Mutex::Lock counters_lock(counters_mutex_);
  if (!responded_bestmove_ && best_move_edge_ &&
      (best_move_edge_.edge() != last_outputted_best_move_edge_ ||
       uci_info_.depth !=
           static_cast<int>(cum_depth_ /
                            (total_playouts_ ? total_playouts_ : 1)) ||
       uci_info_.seldepth != max_depth_ ||
       uci_info_.time + kUciInfoMinimumFrequencyMs < GetTimeSinceStart())) {
    SendUciInfo();
  }
}

int64_t Search::GetTimeSinceStart() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start_time_)
      .count();
}

void Search::SendMovesStats() const {
  const float parent_q =
      -root_node_->GetQ() -
      kFpuReduction * std::sqrt(root_node_->GetVisitedPolicy());
  const float U_coeff =
      kCpuct * std::sqrt(std::max(root_node_->GetChildrenVisits(), 1u));

  std::vector<EdgeAndNode> edges;
  for (const auto& edge : root_node_->Edges()) edges.push_back(edge);

  std::sort(edges.begin(), edges.end(),
            [&parent_q, &U_coeff](EdgeAndNode a, EdgeAndNode b) {
              return std::forward_as_tuple(a.GetN(),
                                           a.GetQ(parent_q) + a.GetU(U_coeff)) <
                     std::forward_as_tuple(b.GetN(),
                                           b.GetQ(parent_q) + b.GetU(U_coeff));
            });

  const bool is_black_to_move = played_history_.IsBlackToMove();
  ThinkingInfo info;
  for (const auto& edge : edges) {
    std::ostringstream oss;
    oss << std::fixed;

    oss << std::left << std::setw(5)
        << edge.GetMove(is_black_to_move).as_string();

    oss << " (" << std::setw(4) << edge.GetMove().as_nn_index() << ")";

    oss << " N: " << std::right << std::setw(7) << edge.GetN() << " (+"
        << std::setw(2) << edge.GetNInFlight() << ") ";

    oss << "(P: " << std::setw(5) << std::setprecision(2) << edge.GetP() * 100
        << "%) ";

    oss << "(Q: " << std::setw(8) << std::setprecision(5) << edge.GetQ(parent_q)
        << ") ";

    oss << "(U: " << std::setw(6) << std::setprecision(5) << edge.GetU(U_coeff)
        << ") ";

    oss << "(Q+U: " << std::setw(8) << std::setprecision(5)
        << edge.GetQ(parent_q) + edge.GetU(U_coeff) << ") ";

    oss << "(V: ";
    optional<float> v;
    if (edge.IsTerminal()) {
      v = edge.node()->GetQ();
    } else {
      NNCacheLock nneval = GetCachedFirstPlyResult(edge);
      if (nneval) v = -nneval->q;
    }
    if (v) {
      oss << std::setw(7) << std::setprecision(4) << *v;
    } else {
      oss << " -.----";
    }
    oss << ") ";

    if (edge.IsTerminal()) oss << "(T) ";

    info.comment = oss.str();
    info_callback_(info);
  }
}

NNCacheLock Search::GetCachedFirstPlyResult(EdgeAndNode edge) const {
  if (!edge.HasNode()) return {};
  assert(edge.node()->GetParent() == root_node_);
  // It would be relatively straightforward to generalize this to fetch NN
  // results for an abitrary move.
  optional<float> retval;
  PositionHistory history(played_history_);  // Is it worth it to move this
  // initialization to SendMoveStats, reducing n memcpys to 1? Probably not.
  history.Append(edge.GetMove());
  auto hash = history.HashLast(kCacheHistoryLength + 1);
  NNCacheLock nneval(cache_, hash);
  return nneval;
}

void Search::MaybeTriggerStop() {
  SharedMutex::Lock nodes_lock(nodes_mutex_);
  Mutex::Lock lock(counters_mutex_);
  // Already responded bestmove, nothing to do here.
  if (responded_bestmove_) return;
  // Don't stop when the root node is not yet expanded.
  if (total_playouts_ == 0) return;
  // If smart pruning tells to stop (best move found), stop.
  if (found_best_move_) {
    stop_ = true;
  }
  // Stop if reached playouts limit.
  if (limits_.playouts >= 0 && total_playouts_ >= limits_.playouts) {
    stop_ = true;
  }
  // Stop if reached visits limit.
  if (limits_.visits >= 0 &&
      total_playouts_ + initial_visits_ >= limits_.visits) {
    stop_ = true;
  }
  // Stop if reached time limit.
  if (limits_.time_ms >= 0 && GetTimeSinceStart() >= limits_.time_ms) {
    stop_ = true;
  }
  // If we are the first to see that stop is needed.
  if (stop_ && !responded_bestmove_) {
    SendUciInfo();
    if (kVerboseStats) SendMovesStats();
    best_move_ = GetBestMoveInternal();
    best_move_callback_({best_move_.first, best_move_.second});
    responded_bestmove_ = true;
    best_move_edge_ = EdgeAndNode();

    if (found_best_move_) {
      // Since we ran out of other moves before running out of time, use the
      // saved time on the next move. If the next move is also smart pruned,
      // extra time will continue rolling forward.  This way we spend time saved
      // by smart pruning on the earliest move with a real decision to make,
      // instead of adding a small bit of time to the average time to the
      // rest of the time curve.
      auto time_since_start = GetTimeSinceStart();
      // This max() might not be needed. Can we ever be over time? Adding it
      // just in case for now.
      bonus_time_ms = std::max<long int>(0, limits_.time_ms - time_since_start);
      std::cerr << "Storing bonus time " << limits_.time_ms << "-" << time_since_start << " = " << bonus_time_ms << std::endl;
    }
  }
}

void Search::UpdateRemainingMoves() {
  if (kAggressiveTimePruning <= 0.0f) return;
  SharedMutex::Lock lock(nodes_mutex_);
  remaining_playouts_ = std::numeric_limits<int>::max();
  // Check for how many playouts there is time remaining.
  if (limits_.time_ms >= 0) {
    auto time_since_start = GetTimeSinceStart();
    if (time_since_start > kSmartPruningToleranceMs) {
      auto nps = (1000LL * total_playouts_ + kSmartPruningToleranceNodes) /
                     (time_since_start - kSmartPruningToleranceMs) +
                 1;
      int64_t remaining_time = limits_.time_ms - time_since_start;
      // Put early_exit scaler here so calculation doesn't have to be done on
      // every node.
      int64_t remaining_playouts =
          remaining_time * nps / kAggressiveTimePruning / 1000;
      // Don't assign directly to remaining_playouts_ as overflow is possible.
      if (remaining_playouts < remaining_playouts_)
        remaining_playouts_ = remaining_playouts;
    }
  }
  // Check how many visits are left.
  if (limits_.visits >= 0) {
    // Add kMiniBatchSize, as it's possible to exceed visits limit by that
    // number.
    auto remaining_visits =
        limits_.visits - total_playouts_ - initial_visits_ + kMiniBatchSize - 1;

    if (remaining_visits < remaining_playouts_)
      remaining_playouts_ = remaining_visits;
  }
  if (limits_.playouts >= 0) {
    // Add kMiniBatchSize, as it's possible to exceed visits limit by that
    // number.
    auto remaining_playouts =
        limits_.visits - total_playouts_ + kMiniBatchSize + 1;
    if (remaining_playouts < remaining_playouts_)
      remaining_playouts_ = remaining_playouts;
  }
  // Even if we exceeded limits, don't go crazy by not allowing any playouts.
  if (remaining_playouts_ <= 1) remaining_playouts_ = 1;
}

// Return the evaluation of the actual best child, regardless of temperature
// settings. This differs from GetBestMove, which does obey any temperature
// settings. So, somethimes, they may return results of different moves.
float Search::GetBestEval() const {
  SharedMutex::SharedLock lock(nodes_mutex_);
  Mutex::Lock counters_lock(counters_mutex_);
  float parent_q = -root_node_->GetQ();
  if (!root_node_->HasChildren()) return parent_q;
  EdgeAndNode best_edge = GetBestChildNoTemperature(root_node_);
  return best_edge.GetQ(parent_q);
}

std::pair<Move, Move> Search::GetBestMove() const {
  SharedMutex::SharedLock lock(nodes_mutex_);
  Mutex::Lock counters_lock(counters_mutex_);
  return GetBestMoveInternal();
}

// Returns the best move, maybe with temperature (according to the settings).
std::pair<Move, Move> Search::GetBestMoveInternal() const
    REQUIRES_SHARED(nodes_mutex_) REQUIRES_SHARED(counters_mutex_) {
  if (responded_bestmove_) return best_move_;
  if (!root_node_->HasChildren()) return {};

  float temperature = kTemperature;
  if (temperature && kTempDecayMoves) {
    int moves = played_history_.Last().GetGamePly() / 2;
    if (moves >= kTempDecayMoves) {
      temperature = 0.0;
    } else {
      temperature *=
          static_cast<float>(kTempDecayMoves - moves) / kTempDecayMoves;
    }
  }

  auto best_node = temperature && root_node_->GetChildrenVisits() > 0
                       ? GetBestChildWithTemperature(root_node_, temperature)
                       : GetBestChildNoTemperature(root_node_);

  Move ponder_move;  // Default is "null move" which means "don't display
                     // anything".
  if (best_node.HasNode() && best_node.node()->HasChildren()) {
    ponder_move = GetBestChildNoTemperature(best_node.node())
                      .GetMove(!played_history_.IsBlackToMove());
  }
  return {best_node.GetMove(played_history_.IsBlackToMove()), ponder_move};
}

// Returns a child with most visits.
EdgeAndNode Search::GetBestChildNoTemperature(Node* parent) const {
  EdgeAndNode best_edge;
  // Best child is selected using the following criteria:
  // * Largest number of playouts.
  // * If two nodes have equal number:
  //   * If that number is 0, the one with larger prior wins.
  //   * If that number is larger than 0, the one with larger eval wins.
  std::tuple<int, float, float> best(-1, 0.0, 0.0);
  for (auto edge : parent->Edges()) {
    if (parent == root_node_ && !limits_.searchmoves.empty() &&
        std::find(limits_.searchmoves.begin(), limits_.searchmoves.end(),
                  edge.GetMove()) == limits_.searchmoves.end()) {
      continue;
    }
    std::tuple<int, float, float> val(edge.GetN(), edge.GetQ(-10.0),
                                      edge.GetP());
    if (val > best) {
      best = val;
      best_edge = edge;
    }
  }
  return best_edge;
}

// Returns a child chosen according to weighted-by-temperature visit count.
EdgeAndNode Search::GetBestChildWithTemperature(Node* parent,
                                                float temperature) const {
  assert(parent->GetChildrenVisits() > 0);
  std::vector<float> cumulative_sums;
  float sum = 0.0;
  const float n_parent = parent->GetN();

  for (auto edge : parent->Edges()) {
    if (parent == root_node_ && !limits_.searchmoves.empty() &&
        std::find(limits_.searchmoves.begin(), limits_.searchmoves.end(),
                  edge.GetMove()) == limits_.searchmoves.end()) {
      continue;
    }
    sum += std::pow(edge.GetN() / n_parent, 1 / temperature);
    cumulative_sums.push_back(sum);
  }

  float toss = Random::Get().GetFloat(cumulative_sums.back());
  int idx =
      std::lower_bound(cumulative_sums.begin(), cumulative_sums.end(), toss) -
      cumulative_sums.begin();

  for (auto edge : parent->Edges()) {
    if (parent == root_node_ && !limits_.searchmoves.empty() &&
        std::find(limits_.searchmoves.begin(), limits_.searchmoves.end(),
                  edge.GetMove()) == limits_.searchmoves.end()) {
      continue;
    }
    if (idx-- == 0) return edge;
  }
  assert(false);
  return {};
}

void Search::StartThreads(size_t how_many) {
  Mutex::Lock lock(threads_mutex_);
  // First thread is a watchdog thread.
  if (threads_.size() == 0) {
    threads_.emplace_back([this]() { WatchdogThread(); });
  }
  // Start working threads.
  while (threads_.size() <= how_many) {
    threads_.emplace_back([this]() {
      SearchWorker worker(this);
      worker.RunBlocking();
    });
  }
}

void Search::RunBlocking(size_t threads) {
  StartThreads(threads);
  Wait();
}

bool Search::IsSearchActive() const {
  Mutex::Lock lock(counters_mutex_);
  return !stop_;
}

void Search::WatchdogThread() {
  while (IsSearchActive()) {
    {
      using namespace std::chrono_literals;
      constexpr auto kMaxWaitTime = 100ms;
      Mutex::Lock lock(counters_mutex_);
      auto remaining_time = (limits_.time_ms - GetTimeSinceStart()) * 1ms;
      watchdog_cv_.wait_for(
          lock.get_raw(), std::min(remaining_time, kMaxWaitTime),
          [this]() NO_THREAD_SAFETY_ANALYSIS { return stop_; });
    }
    MaybeTriggerStop();
  }
  MaybeTriggerStop();
}

void Search::Stop() {
  Mutex::Lock lock(counters_mutex_);
  stop_ = true;
}

void Search::Abort() {
  Mutex::Lock lock(counters_mutex_);
  responded_bestmove_ = true;
  stop_ = true;
}

void Search::Wait() {
  Mutex::Lock lock(threads_mutex_);
  while (!threads_.empty()) {
    threads_.back().join();
    threads_.pop_back();
  }
}

Search::~Search() {
  Abort();
  Wait();
}

//////////////////////////////////////////////////////////////////////////////
// SearchWorker
//////////////////////////////////////////////////////////////////////////////

void SearchWorker::ExecuteOneIteration() {
  // 1. Initialize internal structures.
  InitializeIteration(search_->network_->NewComputation());

  // 2. Gather minibatch.
  GatherMinibatch();

  // 3. Prefetch into cache.
  MaybePrefetchIntoCache();

  // 4. Run NN computation.
  RunNNComputation();

  // 5. Retrieve NN computations (and terminal values) into nodes.
  FetchMinibatchResults();

  // 6. Propagate the new nodes' information to all their parents in the tree.
  DoBackupUpdate();

  // 7. Update the Search's status and progress information.
  UpdateCounters();
}

// 1. Initialize internal structures.
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
void SearchWorker::InitializeIteration(
    std::unique_ptr<NetworkComputation> computation) {
  computation_ = std::make_unique<CachingComputation>(std::move(computation),
                                                      search_->cache_);
  minibatch_.clear();
}

// 2. Gather minibatch.
// ~~~~~~~~~~~~~~~~~~~~
void SearchWorker::GatherMinibatch() {
  // Total number of nodes to process.
  int minibatch_size = 0;
  int collisions_found = 0;
  // Number of nodes processed out of order.
  int number_out_of_order = 0;

  // Gather nodes to process in the current batch.
  // If we had too many (kMiniBatchSize) nodes out of order, also interrupt the
  // iteration so that search can exit.
  // TODO(crem) change that to checking search_->stop_ when bestmove reporting
  // is in a separate thread.
  while (minibatch_size < search_->kMiniBatchSize &&
         number_out_of_order < search_->kMiniBatchSize) {
    // If there's something to process without touching slow neural net, do it.
    if (minibatch_size > 0 && computation_->GetCacheMisses() == 0) return;
    // Pick next node to extend.
    minibatch_.emplace_back(PickNodeToExtend());
    auto& picked_node = minibatch_.back();
    auto* node = picked_node.node;

    // There was a collision. If limit has been reached, return, otherwise
    // just start search of another node.
    if (picked_node.is_collision) {
      if (++collisions_found > search_->kAllowedNodeCollisions) return;
      continue;
    }
    ++minibatch_size;

    // If node is already known as terminal (win/loss/draw according to rules
    // of the game), it means that we already visited this node before.
    if (!node->IsTerminal()) {
      // Node was never visited, extend it.
      ExtendNode(node);

      // Only send non-terminal nodes to a neural network.
      if (!node->IsTerminal()) {
        picked_node.nn_queried = true;
        picked_node.is_cache_hit = AddNodeToComputation(node, true);
      }
    }

    // If out of order eval is enabled and the node to compute we added last
    // doesn't require NN eval (i.e. it's a cache hit or terminal node), do
    // out of order eval for it.
    if (search_->kOutOfOrderEval) {
      if (node->IsTerminal() || picked_node.is_cache_hit) {
        // Perform out of order eval for the last entry in minibatch_.
        FetchSingleNodeResult(&picked_node, computation_->GetBatchSize() - 1);
        DoBackupUpdateSingleNode(picked_node);

        // Remove last entry in minibatch_, as it has just been
        // processed.
        // If NN eval was already processed out of order, remove it.
        if (picked_node.nn_queried) computation_->PopCacheHit();
        minibatch_.pop_back();
        --minibatch_size;
        ++number_out_of_order;
      }
    }
  }
}

// Returns node and whether there's been a search collision on the node.
SearchWorker::NodeToProcess SearchWorker::PickNodeToExtend() {
  // Starting from search_->root_node_, generate a playout, choosing a
  // node at each level according to the MCTS formula. n_in_flight_ is
  // incremented for each node in the playout (via TryStartScoreUpdate()).

  Node* node = search_->root_node_;
  Node::Iterator best_edge;
  // Initialize position sequence with pre-move position.
  history_.Trim(search_->played_history_.GetLength());

  SharedMutex::Lock lock(search_->nodes_mutex_);

  // Fetch the current best root node visits for possible smart pruning.
  int best_node_n = search_->best_move_edge_.GetN();

  // True on first iteration, false as we dive deeper.
  bool is_root_node = true;
  uint16_t depth = 0;

  while (true) {
    // First, terminate if we find collisions or leaf nodes.
    // Set 'node' to point to the node that was picked on previous iteration,
    // possibly spawning it.
    // TODO(crem) This statement has to be in the end of the loop rather than
    //            in the beginning (and there would be no need for "if
    //            (!is_root_node)"), but that would mean extra mutex lock.
    //            Will revisit that after rethinking locking strategy.
    if (!is_root_node) node = best_edge.GetOrSpawnNode(/* parent */ node);
    depth++;
    // n_in_flight_ is incremented. If the method returns false, then there is
    // a search collision, and this node is already being expanded.
    if (!node->TryStartScoreUpdate()) return {node, true, depth};
    // Either terminal or unexamined leaf node -- the end of this playout.
    if (!node->HasChildren()) return {node, false, depth};
    // If we fall through, then n_in_flight_ has been incremented but this
    // playout remains incomplete; we must go deeper.
    float puct_mult =
        search_->kCpuct * std::sqrt(std::max(node->GetChildrenVisits(), 1u));
    float best = -100.0f;
    int possible_moves = 0;
    float parent_q =
        ((is_root_node && search_->kNoise) || !search_->kFpuReduction)
            ? -node->GetQ()
            : -node->GetQ() -
                  search_->kFpuReduction * std::sqrt(node->GetVisitedPolicy());
    for (auto child : node->Edges()) {
      if (is_root_node) {
        // If there's no chance to catch up to the current best node with
        // remaining playouts, don't consider it.
        // best_move_node_ could have changed since best_node_n was retrieved.
        // To ensure we have at least one node to expand, always include
        // current best node.
        if (child != search_->best_move_edge_ &&
            search_->remaining_playouts_ <
                best_node_n - static_cast<int>(child.GetN())) {
          continue;
        }
        // If searchmoves was sent, restrict the search only in that moves
        if (!search_->limits_.searchmoves.empty() &&
            std::find(search_->limits_.searchmoves.begin(),
                      search_->limits_.searchmoves.end(),
                      child.GetMove()) == search_->limits_.searchmoves.end()) {
          continue;
        }
        ++possible_moves;
      }
      float Q = child.GetQ(parent_q);
      if (search_->kStickyCheckmate && Q == 1.0f && child.IsTerminal()) {
        // If we find a checkmate, then the confidence is infinite, so ignore U.
        best_edge = child;
        break;
      }
      const float score = child.GetU(puct_mult) + Q;
      if (score > best) {
        best = score;
        best_edge = child;
      }
    }

    history_.Append(best_edge.GetMove());
    if (is_root_node && possible_moves <= 1 && !search_->limits_.infinite) {
      // If there is only one move theoretically possible within remaining time,
      // output it.
      Mutex::Lock counters_lock(search_->counters_mutex_);
      search_->found_best_move_ = true;
    }
    is_root_node = false;
  }
}

void SearchWorker::ExtendNode(Node* node) {
  // We don't need the mutex because other threads will see that N=0 and
  // N-in-flight=1 and will not touch this node.
  const auto& board = history_.Last().GetBoard();
  auto legal_moves = board.GenerateLegalMoves();

  // Check whether it's a draw/lose by position. Importantly, we must check
  // these before doing the by-rule checks below.
  if (legal_moves.empty()) {
    // Could be a checkmate or a stalemate
    if (board.IsUnderCheck()) {
      node->MakeTerminal(GameResult::WHITE_WON);
    } else {
      node->MakeTerminal(GameResult::DRAW);
    }
    return;
  }

  // We can shortcircuit these draws-by-rule only if they aren't root;
  // if they are root, then thinking about them is the point.
  if (node != search_->root_node_) {
    if (!board.HasMatingMaterial()) {
      node->MakeTerminal(GameResult::DRAW);
      return;
    }

    if (history_.Last().GetNoCaptureNoPawnPly() >= 100) {
      node->MakeTerminal(GameResult::DRAW);
      return;
    }

    if (history_.Last().GetRepetitions() >= 2) {
      node->MakeTerminal(GameResult::DRAW);
      return;
    }

    // Neither by-position or by-rule termination, but maybe it's a TB position.
    if (search_->syzygy_tb_ && board.castlings().no_legal_castle() &&
        history_.Last().GetNoCaptureNoPawnPly() == 0 &&
        (board.ours() + board.theirs()).count() <=
            search_->syzygy_tb_->max_cardinality()) {
      ProbeState state;
      WDLScore wdl = search_->syzygy_tb_->probe_wdl(history_.Last(), &state);
      // Only fail state means the WDL is wrong, probe_wdl may produce correct
      // result with a stat other than OK.
      if (state != FAIL) {
        // If the colors seem backwards, check the checkmate check above.
        if (wdl == WDL_WIN) {
          node->MakeTerminal(GameResult::BLACK_WON);
        } else if (wdl == WDL_LOSS) {
          node->MakeTerminal(GameResult::WHITE_WON);
        } else {  // Cursed wins and blessed losses count as draws.
          node->MakeTerminal(GameResult::DRAW);
        }
        search_->tb_hits_.fetch_add(1, std::memory_order_acq_rel);
        return;
      }
    }
  }

  // Add legal moves as edges of this node.
  node->CreateEdges(legal_moves);
}

// Returns whether node was already in cache.
bool SearchWorker::AddNodeToComputation(Node* node, bool add_if_cached) {
  auto hash = history_.HashLast(search_->kCacheHistoryLength + 1);
  // If already in cache, no need to do anything.
  if (add_if_cached) {
    if (computation_->AddInputByHash(hash)) return true;
  } else {
    if (search_->cache_->ContainsKey(hash)) return true;
  }
  auto planes = EncodePositionForNN(history_, 8);

  std::vector<uint16_t> moves;

  if (node && node->HasChildren()) {
    // Legal moves are known, use them.
    for (auto edge : node->Edges()) {
      moves.emplace_back(edge.GetMove().as_nn_index());
    }
  } else {
    // Cache pseudolegal moves. A bit of a waste, but faster.
    const auto& pseudolegal_moves =
        history_.Last().GetBoard().GeneratePseudolegalMoves();
    moves.reserve(pseudolegal_moves.size());
    for (auto iter = pseudolegal_moves.begin(), end = pseudolegal_moves.end();
         iter != end; ++iter) {
      moves.emplace_back(iter->as_nn_index());
    }
  }

  computation_->AddInput(hash, std::move(planes), std::move(moves));
  return false;
}

// 3. Prefetch into cache.
// ~~~~~~~~~~~~~~~~~~~~~~~
void SearchWorker::MaybePrefetchIntoCache() {
  // TODO(mooskagh) Remove prefetch into cache if node collisions work well.
  // If there are requests to NN, but the batch is not full, try to prefetch
  // nodes which are likely useful in future.
  if (computation_->GetCacheMisses() > 0 &&
      computation_->GetCacheMisses() < search_->kMaxPrefetchBatch) {
    history_.Trim(search_->played_history_.GetLength());
    SharedMutex::SharedLock lock(search_->nodes_mutex_);
    PrefetchIntoCache(search_->root_node_, search_->kMaxPrefetchBatch -
                                               computation_->GetCacheMisses());
  }
}

// Prefetches up to @budget nodes into cache. Returns number of nodes
// prefetched.
int SearchWorker::PrefetchIntoCache(Node* node, int budget) {
  if (budget <= 0) return 0;

  // We are in a leaf, which is not yet being processed.
  if (!node || node->GetNStarted() == 0) {
    if (AddNodeToComputation(node, false)) {
      // Make it return 0 to make it not use the slot, so that the function
      // tries hard to find something to cache even among unpopular moves.
      // In practice that slows things down a lot though, as it's not always
      // easy to find what to cache.
      return 1;
    }
    return 1;
  }

  assert(node);
  // n = 0 and n_in_flight_ > 0, that means the node is being extended.
  if (node->GetN() == 0) return 0;
  // The node is terminal; don't prefetch it.
  if (node->IsTerminal()) return 0;

  // Populate all subnodes and their scores.
  typedef std::pair<float, EdgeAndNode> ScoredEdge;
  std::vector<ScoredEdge> scores;
  float puct_mult =
      search_->kCpuct * std::sqrt(std::max(node->GetChildrenVisits(), 1u));
  // FPU reduction is not taken into account.
  const float parent_q = -node->GetQ();
  for (auto edge : node->Edges()) {
    if (edge.GetP() == 0.0f) continue;
    // Flip the sign of a score to be able to easily sort.
    scores.emplace_back(-edge.GetU(puct_mult) - edge.GetQ(parent_q), edge);
  }

  size_t first_unsorted_index = 0;
  int total_budget_spent = 0;
  int budget_to_spend = budget;  // Initialize for the case where there's only
                                 // one child.
  for (size_t i = 0; i < scores.size(); ++i) {
    if (budget <= 0) break;

    // Sort next chunk of a vector. 3 at a time. Most of the time it's fine.
    if (first_unsorted_index != scores.size() &&
        i + 2 >= first_unsorted_index) {
      const int new_unsorted_index =
          std::min(scores.size(), budget < 2 ? first_unsorted_index + 2
                                             : first_unsorted_index + 3);
      std::partial_sort(scores.begin() + first_unsorted_index,
                        scores.begin() + new_unsorted_index, scores.end(),
                        [](const ScoredEdge& a, const ScoredEdge& b) {
                          return a.first < b.first;
                        });
      first_unsorted_index = new_unsorted_index;
    }

    auto edge = scores[i].second;
    // Last node gets the same budget as prev-to-last node.
    if (i != scores.size() - 1) {
      // Sign of the score was flipped for sorting, so flip it back.
      const float next_score = -scores[i + 1].first;
      const float q = edge.GetQ(-parent_q);
      if (next_score > q) {
        budget_to_spend =
            std::min(budget, int(edge.GetP() * puct_mult / (next_score - q) -
                                 edge.GetNStarted()) +
                                 1);
      } else {
        budget_to_spend = budget;
      }
    }
    history_.Append(edge.GetMove());
    const int budget_spent = PrefetchIntoCache(edge.node(), budget_to_spend);
    history_.Pop();
    budget -= budget_spent;
    total_budget_spent += budget_spent;
  }
  return total_budget_spent;
}

// 4. Run NN computation.
// ~~~~~~~~~~~~~~~~~~~~~~
void SearchWorker::RunNNComputation() { computation_->ComputeBlocking(); }

// 5. Retrieve NN computations (and terminal values) into nodes.
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
void SearchWorker::FetchMinibatchResults() {
  // Populate NN/cached results, or terminal results, into nodes.
  int idx_in_computation = 0;
  for (auto& node_to_process : minibatch_) {
    FetchSingleNodeResult(&node_to_process, idx_in_computation);
    if (node_to_process.nn_queried) ++idx_in_computation;
  }
}

void SearchWorker::FetchSingleNodeResult(NodeToProcess* node_to_process,
                                         int idx_in_computation) {
  Node* node = node_to_process->node;
  if (!node_to_process->nn_queried) {
    // Terminal nodes don't involve the neural NetworkComputation, nor do
    // they require any further processing after value retrieval.
    node_to_process->v = node->GetQ();
    return;
  }
  // For NN results, we need to populate policy as well as value.
  // First the value...
  node_to_process->v = -computation_->GetQVal(idx_in_computation);
  // ...and secondly, the policy data.
  float total = 0.0;
  for (auto edge : node->Edges()) {
    float p =
        computation_->GetPVal(idx_in_computation, edge.GetMove().as_nn_index());
    if (search_->kPolicySoftmaxTemp != 1.0f) {
      p = pow(p, 1 / search_->kPolicySoftmaxTemp);
    }
    edge.edge()->SetP(p);
    // Edge::SetP does some rounding, so only add to the total after rounding.
    total += edge.edge()->GetP();
  }
  // Normalize P values to add up to 1.0.
  if (total > 0.0f) {
    float scale = 1.0f / total;
    for (auto edge : node->Edges()) edge.edge()->SetP(edge.GetP() * scale);
  }
  // Add Dirichlet noise if enabled and at root.
  if (search_->kNoise && node == search_->root_node_) {
    ApplyDirichletNoise(node, 0.25, 0.3);
  }
}

// 6. Propagate the new nodes' information to all their parents in the tree.
// ~~~~~~~~~~~~~~
void SearchWorker::DoBackupUpdate() {
  // Nodes mutex for doing node updates.
  SharedMutex::Lock lock(search_->nodes_mutex_);

  for (const NodeToProcess& node_to_process : minibatch_) {
    DoBackupUpdateSingleNode(node_to_process);
  }
}

void SearchWorker::DoBackupUpdateSingleNode(
    const NodeToProcess& node_to_process) REQUIRES(search_->nodes_mutex_) {
  Node* node = node_to_process.node;
  if (node_to_process.is_collision) {
    // If it was a collision, just undo counters.
    for (node = node->GetParent(); node != search_->root_node_->GetParent();
         node = node->GetParent()) {
      node->CancelScoreUpdate();
    }
    return;
  }

  // Backup V value up to a root. After 1 visit, V = Q.
  float v = node_to_process.v;
  for (Node* n = node; n != search_->root_node_->GetParent();
       n = n->GetParent()) {
    n->FinalizeScoreUpdate(v);
    // Q will be flipped for opponent.
    v = -v;

    // Update the stats.
    // Best move.
    if (n->GetParent() == search_->root_node_ &&
        search_->best_move_edge_.GetN() <= n->GetN()) {
      search_->best_move_edge_ =
          search_->GetBestChildNoTemperature(search_->root_node_);
    }
  }
  ++search_->total_playouts_;
  search_->cum_depth_ += node_to_process.depth;
  search_->max_depth_ = std::max(search_->max_depth_, node_to_process.depth);
}  // namespace lczero

// 7. Update the Search's status and progress information.
//~~~~~~~~~~~~~~~~~~~~
void SearchWorker::UpdateCounters() {
  search_->UpdateRemainingMoves();  // Updates smart pruning counters.
  search_->MaybeOutputInfo();
  search_->MaybeTriggerStop();

  // If this thread had no work, sleep for some milliseconds.
  // Collisions don't count as work, so have to enumerate to find out if there
  // was anything done.
  bool work_done = false;
  for (NodeToProcess& node_to_process : minibatch_) {
    if (!node_to_process.is_collision) {
      work_done = true;
      break;
    }
  }
  if (!work_done) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}  // namespace lczero
