#include <matchmaking/match/match.hpp>

#include <algorithm>

#include <types/tournament_options.hpp>
#include <util/date.hpp>
#include <util/helper.hpp>
#include <util/logger/logger.hpp>

namespace fast_chess {

namespace atomic {
extern std::atomic_bool stop;
}  // namespace atomic

namespace {
bool isFen(const std::string& line) { return line.find(';') == std::string::npos; }
}  // namespace

namespace chrono = std::chrono;

using clock = chrono::high_resolution_clock;
using namespace std::literals;
using namespace chess;

void Match::addMoveData(const Player& player, int64_t measured_time_ms, bool legal) {
    const auto move    = player.engine.bestmove() ? *player.engine.bestmove() : "<none>";
    MoveData move_data = MoveData(move, "0.00", measured_time_ms, 0, 0, 0, 0, legal);

    if (player.engine.output().size() <= 1) {
        data_.moves.push_back(move_data);
        uci_moves_.push_back(move);
        return;
    }

    // extract last info line
    const auto score_type = player.engine.lastScoreType();
    const auto info       = player.engine.lastInfo();

    move_data.nps      = str_utils::findElement<int>(info, "nps").value_or(0);
    move_data.hashfull = str_utils::findElement<int>(info, "hashfull").value_or(0);
    move_data.tbhits   = str_utils::findElement<uint64_t>(info, "tbhits").value_or(0);
    move_data.depth    = str_utils::findElement<int>(info, "depth").value_or(0);
    move_data.seldepth = str_utils::findElement<int>(info, "seldepth").value_or(0);
    move_data.nodes    = str_utils::findElement<uint64_t>(info, "nodes").value_or(0);
    move_data.score    = player.engine.lastScore();

    // Missing elements default to 0
    std::stringstream ss;

    if (score_type == engine::ScoreType::CP) {
        ss << (move_data.score >= 0 ? '+' : '-');
        ss << std::fixed << std::setprecision(2) << (float(std::abs(move_data.score)) / 100);
    } else if (score_type == engine::ScoreType::MATE) {
        ss << (move_data.score > 0 ? "+M" : "-M") << std::to_string(std::abs(move_data.score));
    } else {
        ss << "ERR";
    }

    move_data.score_string = ss.str();

    verifyPvLines(player);

    data_.moves.push_back(move_data);
    uci_moves_.push_back(move);
}

void Match::prepare() {
    board_.set960(tournament_options_.variant == VariantType::FRC);
    if (isFen(opening_.fen)) {
        board_.setFen(opening_.fen);
    } else {
        board_.setEpd(opening_.fen);
    }

    start_position_ = board_.getFen() == chess::constants::STARTPOS ? "startpos" : board_.getFen();

    const auto insert_move = [&](const auto& opening_move) {
        const auto move = uci::moveToUci(opening_move, board_.chess960());
        board_.makeMove(opening_move);

        return MoveData(move, "0.00", 0, 0, 0, 0, 0, true, true);
    };

    data_ = MatchData(board_.getFen());

    std::transform(opening_.moves.begin(), opening_.moves.end(), std::back_inserter(data_.moves), insert_move);

    draw_tracker_     = DrawTracker(tournament_options_);
    resign_tracker_   = ResignTracker(tournament_options_);
    maxmoves_tracker_ = MaxMovesTracker(tournament_options_);
}

void Match::start(engine::UciEngine& engine1, engine::UciEngine& engine2, const std::vector<int>& cpus) {
    prepare();

    std::transform(data_.moves.begin(), data_.moves.end(), std::back_inserter(uci_moves_),
                   [](const MoveData& data) { return data.move; });

    Player player_1 = Player(engine1);
    Player player_2 = Player(engine2);

    player_1.color = board_.sideToMove();
    player_2.color = ~board_.sideToMove();

    player_1.engine.refreshUci();
    player_2.engine.refreshUci();

    player_1.engine.setCpus(cpus);
    player_2.engine.setCpus(cpus);

    const auto start = clock::now();

    try {
        while (true) {
            if (atomic::stop.load()) {
                data_.termination = MatchTermination::INTERRUPT;
                break;
            }

            if (!playMove(player_1, player_2)) break;

            if (atomic::stop.load()) {
                data_.termination = MatchTermination::INTERRUPT;
                break;
            }

            if (!playMove(player_2, player_1)) break;
        }
    } catch (const std::exception& e) {
    }

    const auto end = clock::now();

    data_.end_time = util::time::datetime("%Y-%m-%dT%H:%M:%S %z");
    data_.duration = util::time::duration(chrono::duration_cast<chrono::seconds>(end - start));

    data_.players = std::make_pair(MatchData::PlayerInfo{engine1.getConfig(), player_1.getResult(), player_1.color},
                                   MatchData::PlayerInfo{engine2.getConfig(), player_2.getResult(), player_2.color});
}

bool Match::playMove(Player& us, Player& them) {
    const auto gameover = board_.isGameOver();
    const auto name     = us.engine.getConfig().name;

    if (gameover.second == GameResult::DRAW) {
        us.setDraw();
        them.setDraw();
    }

    if (gameover.second == GameResult::LOSE) {
        us.setLost();
        them.setWon();
    }

    if (gameover.first != GameResultReason::NONE) {
        data_.reason = convertChessReason(name, gameover.first);
        return false;
    }

    // disconnect
    if (!us.engine.isready()) {
        setEngineCrashStatus(us, them);
        return false;
    }

    // write new uci position
    auto success = us.engine.position(uci_moves_, start_position_);
    if (!success) {
        setEngineCrashStatus(us, them);
        return false;
    }

    // wait for readyok
    if (!us.engine.isready()) {
        setEngineCrashStatus(us, them);
        return false;
    }

    // write go command
    success = us.engine.go(us.getTimeControl(), them.getTimeControl(), board_.sideToMove());
    if (!success) {
        setEngineCrashStatus(us, them);
        return false;
    }

    // wait for bestmove
    auto t0     = clock::now();
    auto status = us.engine.readEngine("bestmove", us.getTimeoutThreshold());
    auto t1     = clock::now();

    us.engine.writeLog();

    if (status == engine::process::Status::ERR || !us.engine.isready()) {
        setEngineCrashStatus(us, them);
        return false;
    }

    if (atomic::stop) {
        data_.termination = MatchTermination::INTERRUPT;

        return false;
    }

    const auto elapsed_millis = chrono::duration_cast<chrono::milliseconds>(t1 - t0).count();

    const auto best_move = us.engine.bestmove();
    const auto move      = best_move ? uci::uciToMove(board_, *best_move) : Move::NO_MOVE;
    const auto legal     = isLegal(move);

    const auto timeout = !us.updateTime(elapsed_millis);

    addMoveData(us, elapsed_millis, legal);

    // there are two reasons why best_move could be empty
    // 1. the engine crashed
    // 2. the engine did not respond in time
    // we report a loss on time when the engine didnt respond in time
    // and otherwise an illegal move
    if (best_move == std::nullopt) {
        // Time forfeit
        if (timeout) {
            setEngineTimeoutStatus(us, them);
        } else {
            setEngineIllegalMoveStatus(us, them, best_move);
        }

        return false;
    }

    // illegal move
    if (!legal) {
        setEngineIllegalMoveStatus(us, them, best_move);
        return false;
    }

    if (timeout) {
        setEngineTimeoutStatus(us, them);
        return false;
    }

    board_.makeMove(move);

    // CuteChess uses plycount/2 for its movenumber, which is wrong for epd books as it doesnt take
    // into account the fullmove counter of the starting FEN, leading to different behavior between
    // pgn and epd adjudication. fast-chess fixes this by using the fullmove counter from the board
    // object directly
    draw_tracker_.update(us.engine.lastScore(), board_.fullMoveNumber() - 1, us.engine.lastScoreType(),
                         board_.halfMoveClock());
    resign_tracker_.update(us.engine.lastScore(), us.engine.lastScoreType(), ~board_.sideToMove());
    maxmoves_tracker_.update(us.engine.lastScore(), us.engine.lastScoreType());

    return !adjudicate(us, them);
}

bool Match::isLegal(Move move) const noexcept {
    Movelist moves;
    movegen::legalmoves(moves, board_);

    return std::find(moves.begin(), moves.end(), move) != moves.end();
}

void Match::setEngineCrashStatus(Player& loser, Player& winner) {
    loser.setLost();
    winner.setWon();

    crash_or_disconnect_ = true;

    const auto name = loser.engine.getConfig().name;
    data_.termination = MatchTermination::DISCONNECT;
    data_.reason      = name + Match::DISCONNECT_MSG;

    Logger::warn<true>("Warning; Engine {} disconnects", name);
}

void Match::setEngineTimeoutStatus(Player& loser, Player& winner) {
    loser.setLost();
    winner.setWon();

    const auto name = loser.engine.getConfig().name;

    data_.termination = MatchTermination::TIMEOUT;
    data_.reason      = name + Match::TIMEOUT_MSG;

    Logger::warn<true>("Warning; Engine {} loses on time", name);

    // we send a stop command to the engine to prevent it from thinking
    // and wait for a bestmove to appear

    loser.engine.writeEngine("stop");

    if (!loser.engine.outputIncludesBestmove()) {
        // wait 10 seconds for the bestmove to appear
        loser.engine.readEngine("bestmove", 1000ms * 10);
    }
}

void Match::setEngineIllegalMoveStatus(Player& loser, Player& winner, const std::optional<std::string>& best_move) {
    loser.setLost();
    winner.setWon();

    const auto name = loser.engine.getConfig().name;

    data_.termination = MatchTermination::ILLEGAL_MOVE;
    data_.reason      = name + Match::ILLEGAL_MSG;

    Logger::warn<true>("Warning; Illegal move {} played by {}", best_move ? *best_move : "<none>", name);
}

bool Match::isUciMove(const std::string& move) noexcept {
    bool is_uci = false;

    constexpr auto is_digit     = [](char c) { return c >= '0' && c <= '9'; };
    constexpr auto is_file      = [](char c) { return c >= 'a' && c <= 'h'; };
    constexpr auto is_promotion = [](char c) { return c == 'n' || c == 'b' || c == 'r' || c == 'q'; };

    // assert that the move is in uci format, [abcdefgh][0-9][abcdefgh][0-9][nbrq]
    if (move.size() >= 4) {
        is_uci = is_file(move[0]) && is_digit(move[1]) && is_file(move[2]) && is_digit(move[3]);
    }

    if (move.size() == 5) {
        is_uci = is_uci && is_promotion(move[4]);
    }

    return is_uci;
}

void Match::verifyPvLines(const Player& us) {
    const static auto verifyPv = [](Board board, const std::string& startpos, const std::vector<std::string>& uci_moves,
                                    const std::string& info) {
        // skip lines without pv
        const auto tokens = str_utils::splitString(info, ' ');
        if (!str_utils::contains(tokens, "pv")) return;

        const auto fen = board.getFen();
        auto it_start  = std::find(tokens.begin(), tokens.end(), "pv") + 1;
        auto it_end    = std::find_if(it_start, tokens.end(), [](const auto& token) { return !isUciMove(token); });

        Movelist moves;

        while (it_start != it_end) {
            movegen::legalmoves(moves, board);

            if (std::find(moves.begin(), moves.end(), uci::uciToMove(board, *it_start)) == moves.end()) {
                auto fmt      = fmt::format("Warning; Illegal pv move {} pv: {}", *it_start, info);
                auto position = fmt::format("position {}", startpos == "startpos" ? "startpos" : ("fen " + startpos));
                auto fmt2     = fmt::format("From; {} moves {}", position, str_utils::join(uci_moves, " "));

                Logger::warn<true>(fmt + "\n" + fmt2);

                break;
            }

            board.makeMove(uci::uciToMove(board, *it_start));

            it_start++;
        }
    };

    for (const auto& info : us.engine.output()) {
        verifyPv(board_, start_position_, uci_moves_, info.line);
    }
}

bool Match::adjudicate(Player& us, Player& them) noexcept {
    if (tournament_options_.resign.enabled && resign_tracker_.resignable() && us.engine.lastScore() < 0) {
        us.setLost();
        them.setWon();

        data_.termination = MatchTermination::ADJUDICATION;
        data_.reason      = them.engine.getConfig().name + Match::ADJUDICATION_WIN_MSG;

        return true;
    }

    if (tournament_options_.draw.enabled && draw_tracker_.adjudicatable()) {
        us.setDraw();
        them.setDraw();

        data_.termination = MatchTermination::ADJUDICATION;
        data_.reason      = Match::ADJUDICATION_MSG;

        return true;
    }

    if (tournament_options_.maxmoves.enabled && maxmoves_tracker_.maxmovesreached()) {
        us.setDraw();
        them.setDraw();

        data_.termination = MatchTermination::ADJUDICATION;
        data_.reason      = Match::ADJUDICATION_MSG;

        return true;
    }

    return false;
}

std::string Match::convertChessReason(const std::string& engine_name, GameResultReason reason) noexcept {
    if (reason == GameResultReason::CHECKMATE) {
        return engine_name + Match::CHECKMATE_MSG;
    }

    if (reason == GameResultReason::STALEMATE) {
        return Match::STALEMATE_MSG;
    }

    if (reason == GameResultReason::INSUFFICIENT_MATERIAL) {
        return Match::INSUFFICIENT_MSG;
    }

    if (reason == GameResultReason::THREEFOLD_REPETITION) {
        return Match::REPETITION_MSG;
    }

    if (reason == GameResultReason::FIFTY_MOVE_RULE) {
        return Match::FIFTY_MSG;
    }

    return "";
}

}  // namespace fast_chess
