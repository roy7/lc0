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

#include <algorithm>
#include <cmath>
#include <functional>

#include "engine.h"
#include "mcts/search.h"
#include "neural/factory.h"
#include "neural/loader.h"
#include "utils/configfile.h"

namespace lczero {
namespace {
// TODO(mooskagh) Move threads parameter handling to search.
const int kDefaultThreads = 2;
const char* kThreadsOption = "Number of worker threads";
const char* kDebugLogStr = "Do debug logging into file";

// TODO(mooskagh) Move weights/backend/backend-opts parameter handling to
//                network factory.
const char* kWeightsStr = "Network weights file path";
const char* kNnBackendStr = "NN backend to use";
const char* kNnBackendOptionsStr = "NN backend parameters";
const char* kSlowMoverStr = "Scale thinking time";
const char* kMoveOverheadStr = "Move time overhead in milliseconds";
const char* kTimeCurveMidpoint = "Time curve midpoint ply";
const char* kTimeCurveSteepness = "Time curve steepness";
const char* kSyzygyTablebaseStr = "List of Syzygy tablebase directories";

const char* kAutoDiscover = "<autodiscover>";

float ComputeSurvivalAtPly(int ply, float midpoint, float steepness) {
  // This function is the survival function of the logistic distribution, it was
  // chosen as it fit empirical analysis finding P(game ended at ply).
  // We determine how many moves to plan time management for by summing over
  // this function from ply to infinity (or some other reasonably large value).
  // midpoint: The ply where the function is half its maximum value.
  // steepness: How quickly the function drops off from its maximum value.
  return 1 / (1 + std::pow(ply / midpoint, steepness));
}

}  // namespace

EngineController::EngineController(BestMoveInfo::Callback best_move_callback,
                                   ThinkingInfo::Callback info_callback,
                                   const OptionsDict& options)
    : options_(options),
      best_move_callback_(best_move_callback),
      info_callback_(info_callback) {}

void EngineController::PopulateOptions(OptionsParser* options) {
  using namespace std::placeholders;

  options->Add<StringOption>(kWeightsStr, "weights", 'w') = kAutoDiscover;
  options->Add<IntOption>(kThreadsOption, 1, 128, "threads", 't') =
      kDefaultThreads;
  options->Add<IntOption>(
      "NNCache size", 0, 999999999, "nncache", '\0',
      std::bind(&EngineController::SetCacheSize, this, _1)) = 200000;

  const auto backends = NetworkFactory::Get()->GetBackendsList();
  options->Add<ChoiceOption>(kNnBackendStr, backends, "backend") =
      backends.empty() ? "<none>" : backends[0];
  options->Add<StringOption>(kNnBackendOptionsStr, "backend-opts");
  options->Add<FloatOption>(kSlowMoverStr, 0.0f, 100.0f, "slowmover") = 1.00f;
  options->Add<IntOption>(kMoveOverheadStr, 0, 10000, "move-overhead") = 100;
  options->Add<FloatOption>(kTimeCurveMidpoint, 1.0f, 200.0f,
                            "time-curve-midpoint") = 101.5f;
  options->Add<FloatOption>(kTimeCurveSteepness, 1.0f, 100.0f,
                            "time-curve-steepness") = 6.8f;
  options->Add<StringOption>(kSyzygyTablebaseStr, "syzygy-paths", 's');
  // Add "Ponder" option to signal to GUIs that we support pondering.
  // This option is currently not used by lc0 in any way.
  options->Add<BoolOption>("Ponder", "ponder") = false;

  Search::PopulateUciParams(options);
  ConfigFile::PopulateOptions(options);

  auto defaults = options->GetMutableDefaultsOptions();

  defaults->Set<int>(Search::kMiniBatchSizeStr, 256);    // Minibatch = 256
  defaults->Set<float>(Search::kFpuReductionStr, 0.9f);  // FPU reduction = 0.9
  defaults->Set<float>(Search::kCpuctStr, 3.4f);         // CPUCT = 3.4
  defaults->Set<float>(Search::kPolicySoftmaxTempStr, 2.2f);  // Psoftmax = 2.2
  defaults->Set<int>(Search::kAllowedNodeCollisionsStr, 32);  // Node collisions
  // Cache key has a history of 1 ply back. That's to be compatible with old
  // bug. Also tests show that for now 1 has better strength than 7.
  // TODO(crem) Revisit this setting.
  defaults->Set<int>(Search::kCacheHistoryLengthStr, 1);
}

SearchLimits EngineController::PopulateSearchLimits(int ply, bool is_black,
                                                    const GoParams& params) {
  SearchLimits limits;
  limits.time_ms = params.movetime;
  int64_t time = (is_black ? params.btime : params.wtime);
  if (!params.searchmoves.empty()) {
    limits.searchmoves.reserve(params.searchmoves.size());
    for (const auto& move : params.searchmoves) {
      limits.searchmoves.emplace_back(move, is_black);
    }
  }
  limits.infinite = params.infinite || params.ponder;
  limits.visits = limits.infinite ? -1 : params.nodes;
  if (limits.infinite || time < 0) return limits;
  int increment = std::max(int64_t(0), is_black ? params.binc : params.winc);

  // Fix non-standard uci command.
  float movestogo = params.movestogo == 0 ? 1.0f : params.movestogo;

  // How to scale moves time.
  float slowmover = options_.Get<float>(kSlowMoverStr);
  int64_t move_overhead = options_.Get<int>(kMoveOverheadStr);

  float time_curve_midpoint = options_.Get<float>(kTimeCurveMidpoint);
  float time_curve_steepness = options_.Get<float>(kTimeCurveSteepness);

  // Sum over the survival function to guess how many moves ahead are worth
  // planning time for. All values must be scaled relative to the first value,
  // so compute the first ply separately.
  float this_move_survival =
      ComputeSurvivalAtPly(ply, time_curve_midpoint, time_curve_steepness);

  // Sum over a large range of plies to approximate summing to infinity.
  float guessed_movestogo = 0.0f;
  for (int i = ply + 2; i < ply + 300; i += 2) {
    guessed_movestogo +=
        ComputeSurvivalAtPly(i, time_curve_midpoint, time_curve_steepness);
  }

  // Normalize to account for the game being at the current ply.
  guessed_movestogo = guessed_movestogo / this_move_survival + 1;

  // If the guessed movestogo is greater than movestogo, then just use
  // movestogo so that we use all our time until the time control.
  if (movestogo <= 0 || guessed_movestogo < movestogo) {
    movestogo = guessed_movestogo;
  }

  // Total time, including increments, until time control.
  auto total_moves_time =
      std::max(0.0f, time + increment * (movestogo - 1) - move_overhead);

  if (bonus_time_ms > 0) {
    // Don't calculate the time curve using the bonus time, use the normal real
    // curve we'd expect without smart pruning.
    std::cerr << "Total time was " << total_moves_time << " remove bonus " << bonus_time_ms << std::endl;
    total_moves_time -= bonus_time_ms;
  }

  // Evenly split total time between all moves.
  float this_move_time = total_moves_time / movestogo;
  std::cerr << "this_move_time is  " << this_move_time << std::endl;

  // Only extend thinking time with slowmover if smart pruning can potentially
  // reduce it.
  constexpr int kSmartPruningToleranceMs = 200;
  if (slowmover < 1.0 ||
      this_move_time * slowmover > kSmartPruningToleranceMs) {
    this_move_time *= slowmover;
  }

  // If we saved time from smart pruning the prior move, add it to this move.
  if (bonus_time_ms > 0) {
    std::cerr << "Adding bonus time " << bonus_time_ms << " to " << this_move_time << std::endl;
    this_move_time += bonus_time_ms;
    bonus_time_ms = 0;
  }

  // Make sure we don't exceed current time limit with what we calculated.
  limits.time_ms = std::max(
      int64_t{0},
      std::min(static_cast<int64_t>(this_move_time), time - move_overhead));
  return limits;
}

void EngineController::UpdateTBAndNetwork() {
  SharedLock lock(busy_mutex_);

  std::string tb_paths = options_.Get<std::string>(kSyzygyTablebaseStr);
  if (!tb_paths.empty() && tb_paths != tb_paths_) {
    syzygy_tb_ = std::make_unique<SyzygyTablebase>();
    std::cerr << "Loading Syzygy tablebases from " << tb_paths << std::endl;
    if (!syzygy_tb_->init(tb_paths)) {
      std::cerr << "Failed to load Syzygy tablebases!" << std::endl;
      syzygy_tb_ = nullptr;
    } else {
      tb_paths_ = tb_paths;
    }
  }

  std::string network_path = options_.Get<std::string>(kWeightsStr);
  std::string backend = options_.Get<std::string>(kNnBackendStr);
  std::string backend_options = options_.Get<std::string>(kNnBackendOptionsStr);

  if (network_path == network_path_ && backend == backend_ &&
      backend_options == backend_options_)
    return;

  network_path_ = network_path;
  backend_ = backend;
  backend_options_ = backend_options;

  std::string net_path = network_path;
  if (net_path == kAutoDiscover) {
    net_path = DiscoverWeightsFile();
  } else {
    std::cerr << "Loading weights file from: " << net_path << std::endl;
  }
  Weights weights = LoadWeightsFromFile(net_path);

  OptionsDict network_options =
      OptionsDict::FromString(backend_options, &options_);

  network_ = NetworkFactory::Get()->Create(backend, weights, network_options);
}

void EngineController::SetCacheSize(int size) { cache_.SetCapacity(size); }

void EngineController::EnsureReady() {
  UpdateTBAndNetwork();
  std::unique_lock<RpSharedMutex> lock(busy_mutex_);
}

void EngineController::NewGame() {
  SharedLock lock(busy_mutex_);
  cache_.Clear();
  search_.reset();
  tree_.reset();
  current_position_.reset();
  UpdateTBAndNetwork();
}

void EngineController::SetPosition(const std::string& fen,
                                   const std::vector<std::string>& moves_str) {
  SharedLock lock(busy_mutex_);
  current_position_ = CurrentPosition{fen, moves_str};
  search_.reset();
}

void EngineController::SetupPosition(
    const std::string& fen, const std::vector<std::string>& moves_str) {
  SharedLock lock(busy_mutex_);
  search_.reset();

  if (!tree_) tree_ = std::make_unique<NodeTree>();

  std::vector<Move> moves;
  for (const auto& move : moves_str) moves.emplace_back(move);
  tree_->ResetToPosition(fen, moves);
  UpdateTBAndNetwork();
}

void EngineController::Go(const GoParams& params) {
  go_params_ = params;

  ThinkingInfo::Callback info_callback(info_callback_);

  if (current_position_) {
    if (params.ponder && !current_position_->moves.empty()) {
      std::vector<std::string> moves(current_position_->moves);
      std::string ponder_move = moves.back();
      moves.pop_back();
      SetupPosition(current_position_->fen, moves);

      info_callback = [this, ponder_move](const ThinkingInfo& info) {
        ThinkingInfo ponder_info(info);
        if (!ponder_info.pv.empty() &&
            ponder_info.pv[0].as_string() == ponder_move) {
          ponder_info.pv.erase(ponder_info.pv.begin());
        } else {
          ponder_info.pv.clear();
        }
        if (ponder_info.score) {
          ponder_info.score = -*ponder_info.score;
        }
        if (ponder_info.depth > 1) {
          ponder_info.depth--;
        }
        if (ponder_info.seldepth > 1) {
          ponder_info.seldepth--;
        }
        info_callback_(ponder_info);
      };
    } else {
      SetupPosition(current_position_->fen, current_position_->moves);
    }
  } else if (!tree_) {
    SetupPosition(ChessBoard::kStartingFen, {});
  }

  auto limits = PopulateSearchLimits(tree_->GetPlyCount(),
                                     tree_->IsBlackToMove(), params);

  search_ = std::make_unique<Search>(*tree_, network_.get(),
                                     best_move_callback_, info_callback, limits,
                                     options_, &cache_, syzygy_tb_.get());

  search_->StartThreads(options_.Get<int>(kThreadsOption));
}

void EngineController::PonderHit() {
  go_params_.ponder = false;
  Go(go_params_);
}

void EngineController::Stop() {
  if (search_) {
    search_->Stop();
    search_->Wait();
  }
}

EngineLoop::EngineLoop()
    : engine_(std::bind(&UciLoop::SendBestMove, this, std::placeholders::_1),
              std::bind(&UciLoop::SendInfo, this, std::placeholders::_1),
              options_.GetOptionsDict()) {
  engine_.PopulateOptions(&options_);
  options_.Add<StringOption>(
      kDebugLogStr, "debuglog", 'l',
      [this](const std::string& filename) { SetLogFilename(filename); }) = "";
}

void EngineLoop::RunLoop() {
  if (!ConfigFile::Init(&options_) || !options_.ProcessAllFlags()) return;
  UciLoop::RunLoop();
}

void EngineLoop::CmdUci() {
  SendId();
  for (const auto& option : options_.ListOptionsUci()) {
    SendResponse(option);
  }
  SendResponse("uciok");
}

void EngineLoop::CmdIsReady() {
  engine_.EnsureReady();
  SendResponse("readyok");
}

void EngineLoop::CmdSetOption(const std::string& name, const std::string& value,
                              const std::string& context) {
  options_.SetOption(name, value, context);
  if (options_sent_) {
    options_.SendOption(name);
  }
}

void EngineLoop::EnsureOptionsSent() {
  if (!options_sent_) {
    options_.SendAllOptions();
    options_sent_ = true;
  }
}

void EngineLoop::CmdUciNewGame() {
  EnsureOptionsSent();
  engine_.NewGame();
}

void EngineLoop::CmdPosition(const std::string& position,
                             const std::vector<std::string>& moves) {
  EnsureOptionsSent();
  std::string fen = position;
  if (fen.empty()) fen = ChessBoard::kStartingFen;
  engine_.SetPosition(fen, moves);
}

void EngineLoop::CmdGo(const GoParams& params) {
  EnsureOptionsSent();
  engine_.Go(params);
}

void EngineLoop::CmdPonderHit() { engine_.PonderHit(); }

void EngineLoop::CmdStop() { engine_.Stop(); }

}  // namespace lczero
