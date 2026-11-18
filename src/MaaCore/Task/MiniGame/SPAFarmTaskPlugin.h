#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <meojson/json.hpp>

#include "Common/AsstMsg.h"
#include "Common/AsstTypes.h"
#include "MaaUtils/NoWarningCVMat.hpp"
#include "Task/AbstractTaskPlugin.h"

namespace asst
{

class SPAFarmTaskPlugin final : public AbstractTaskPlugin
{
public:
    using AbstractTaskPlugin::AbstractTaskPlugin;
    ~SPAFarmTaskPlugin() override = default;

    enum class ScreenState
    {
        Lobby,
        Mode,
        Strategy,
        Squad,
        Info,
        Shop,
        Facing,
        Battle,
        Result,
        Reconnect,
        Confirm,
        Abandon,
        EquipFull,
        ThreeChoice,
        Decision,
        Reward,
        Detail,
        Unknown
    };

    struct OcrItem
    {
        double cx = 0.0;
        double cy = 0.0;
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        std::string text;
        double score = 0.0;
    };

    struct DictEntry
    {
        std::string name;
        std::string norm;
        int len = 0;
        std::unordered_map<std::string, int> bag;
    };

    struct DictMatch
    {
        std::string name;
        int len = 0;
        int start = 0;
        int end = 0;
        double score = 0.0;
        bool exact = false;
    };

    struct StackGeomRule
    {
        std::string geom;      // "front1", "back1", "sameRow", "around4", "around8", "around12", "inRange"
        std::string need_bond; // required bond name on neighbor, or empty
        bool other_only = false;
        bool unconditional = false;
    };

    struct StackTraitInfo
    {
        std::vector<StackGeomRule> rules;
        bool on_skill = false;
        bool on_deploy = false;
        bool end_of_prep = false;
        bool infects_tile = false;
        bool uses_infected_tile = false;
        int stack_cap = 0;
    };

    struct OperatorInfo
    {
        std::string chess_id;
        std::string char_id;
        std::string name;
        std::string norm_name;
        int tier = 1;
        bool elite = false;
        bool golden = false;
        std::string profession;
        std::string sub_profession_id;
        std::string position; // "MELEE" or "RANGED"
        int atk = 0;
        int def = 0;
        int max_hp = 0;
        int block_cnt = 1;
        int purchase_price = 2;
        int sell_price = 1;
        std::vector<std::string> bond_ids;
        std::vector<std::string> bond_names;
        std::string trait_text;
        StackTraitInfo stack_info;
        bool is_summoner = false;
        bool is_healer = false;
        bool can_block_air = false;
        int taunt_level = 1;
        double dps_weight = 1.0;
        bool facing_independent = false;
        std::vector<std::pair<int, int>> base_range; // [dr, dc] under RIGHT facing
    };

    struct BondInfo
    {
        std::string bond_id;
        std::string name;
        std::string norm_name;
        bool is_core = false;
        std::string count_mode; // "FIELD_ONLY" or "FIELD_AND_BENCH"
        std::vector<int> thresholds;
        int max_Active = 6;
        std::string desc;
        std::vector<std::string> chess_ids;
        std::vector<std::string> operator_names;
    };

    struct ItemInfo
    {
        std::string id;
        std::string name;
        std::string norm_name;
        int tier = 1;
        bool golden = false;
        int price = 2;
        std::string effect;
        std::string category = "output"; // "debuff", "utility", "stack", "defense", "output" (`An` in L8396-8399)
        std::string granted_bond_id;
        std::string granted_bond_name;
        bool is_debuff = false;
    };

    struct StrategyRule
    {
        std::string id;
        std::string name;
        std::string norm_name;
        std::string owner;
        bool available = true;
        int passed = 0;
    };

    struct StageInfo
    {
        std::string stage_id;
        std::string name;
        int width = 13;
        int height = 7;
        std::vector<std::string> rows;
    };

    struct BoardRefEntry
    {
        std::string stage_id;
        std::string camera;
        std::unordered_map<std::string, std::vector<double>> cells;
    };

    struct BossInfo
    {
        std::string enemy_id;
        std::string name;
        std::string norm_name;
        std::string description;
    };

    struct EnemyTypeInfo
    {
        std::string code;
        std::string name;
    };

    struct RoundPacing
    {
        int round = 1;
        int target_shop_level = 3;
        int min_deploy = 2;
        bool is_boss_round = false;
    };

    struct CameraCalibData
    {
        std::string camera;
        std::vector<std::array<double, 4>> anchors; // [gx, gy, px, py]
        std::array<double, 2> marker_E = { 0.0, 0.0 };
        std::array<double, 2> marker_S1 = { 0.0, 0.0 };
        std::array<double, 2> marker_S2 = { 0.0, 0.0 };
        int ref_n_E = 900;
        int ref_n_S1 = 950;
        int ref_n_S2 = 950;
        std::array<double, 9> base_H = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    };

    struct SimilarityFit
    {
        bool valid = false;
        int matched_markers = 0;
        double scale = 1.0;
        double cos_a = 1.0;
        double sin_a = 0.0;
        double tx = 0.0;
        double ty = 0.0;
        double err = 999.0;
        double angle_deg = 0.0;
    };

    struct CameraDetectResult
    {
        std::string camera; // "expanded", "collapsed", or ""
        std::array<double, 9> H = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        SimilarityFit fit;
    };

    struct StableBoardState
    {
        std::array<double, 9> H = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        std::array<double, 9> raw_H = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        std::string camera;
        SimilarityFit fit;
        std::array<double, 4> pts = { 0.0, 0.0, 0.0, 0.0 };
        cv::Mat img;
    };

    struct ShopCard
    {
        int index = 0;
        int x = 0;
        std::string name;
        std::string kind; // "operator", "item", or ""
        std::string source;
        std::vector<std::string> bonds;
        bool free_card = false;
        int cost = 2;
        int tier = 1;
    };

    struct ShopHeaderState
    {
        std::optional<int> level;
        std::optional<int> gold;
        std::optional<int> upgrade_cost;
        std::optional<std::pair<int, int>> upgrade_btn;
        std::optional<std::pair<int, int>> refresh_btn;
    };

    struct UnitSlot
    {
        std::string slot;   // "board:r,c" or "bench:k"
        std::string area;   // "board" or "bench"
        int r = 0;
        int c = 0;
        std::string name;
        std::string source; // "panel", "shop", "carried", "guess"
        std::string facing; // "RIGHT", "UP", "LEFT", "DOWN", or ""
        bool elite = false;
        bool verified = false;
        bool confirmed = false;
        bool ghost = false;
        bool auxiliary = false;
        bool item = false;
        std::vector<double> hist;
    };

    struct EquipSlotInfo
    {
        int c = 4;
        std::string state; // "empty", "item", "unknown"
        std::string name;
        int inspected_round = -1;
    };

    struct GrazeStats
    {
        double ground = 0.0;
        double air = 0.0;
        double early_ground = 0.0;
        double first_step = 0.0;
    };

    struct SpawnPathInfo
    {
        std::pair<int, int> spawn = { 0, 0 };
        std::unordered_map<std::string, double> ground_flow;
        std::unordered_map<std::string, int> ground_progress;
        std::string first_tile;
        bool is_single_tile = false;
    };

    struct StageTrafficMap
    {
        std::string stage_id;
        int rows = 7;
        int cols = 13;
        std::vector<std::string> grid;
        std::vector<std::pair<int, int>> spawns;
        std::vector<std::pair<int, int>> goals;
        std::vector<SpawnPathInfo> paths;
        std::unordered_map<std::string, double> road_importance;
        std::vector<std::vector<double>> ground_traffic;
        std::vector<std::vector<double>> air_traffic;
        std::vector<std::vector<double>> early_ground_traffic;
        std::vector<std::vector<double>> ground_first_step;
        std::vector<std::vector<int>> ground_dist;
        std::vector<std::vector<int>> air_dist;
        std::set<std::string> chokepoints;
        double max_ground_traffic = 1.0;
        double max_air_traffic = 1.0;
    };

    struct PlacementEvalOptions
    {
        std::unordered_set<std::string> focus_bonds;
        double focus_bond_mul = 2.0;
        double core_bond_mul = 1.5;
        double support_defense_weight = 1.0;
        double support_heal_weight = 1.0;
        bool advance = false;
        bool avoid_right_col = false;
        std::string boss_side;
        int round = 0;
    };

    struct PlacementEvalResult
    {
        double score = -1e9;
        double defense = 0.0;
        double stacking = 0.0;
        double exposed = 0.0;
        double advance_score = 0.0;
        double gate = 0.0;
        int unguarded_routes = 0;
    };

    struct PlacementAssign
    {
        std::string slot;
        std::string from_area;
        int from_r = 0;
        int from_c = 0;
        std::string name;
        int r = 0;
        int c = 0;
        std::string facing = "LEFT";
        bool elite = false;
    };

    struct PlacementMove
    {
        std::string type; // "deploy", "reposition", "bench", "orient"
        std::string unit;
        std::string from_area;
        int from_r = 0;
        int from_c = 0;
        std::string to_area;
        int to_r = 0;
        int to_c = 0;
        std::string facing = "LEFT";
    };

    struct ShopPlanStep
    {
        std::string action; // "upgrade", "buy", "swap", "refresh", "freeze", "done"
        std::string target;
        std::string kind;
        int x = 0;
        int cost = 0;
        std::string sell_slot;
        std::string sell_name;
        std::string reason;
    };

    struct InfoScreenData
    {
        struct BondStatus
        {
            std::string name;
            bool active = false;
            double green = 0.0;
            bool core = false;
        };
        std::vector<BondStatus> bonds;
        std::string boss;
        std::vector<std::string> enemy_types;
    };

    bool verify(AsstMsg msg, const json::value& details) const override;
    bool set_params(const json::value& params);
    void set_profile(bool low_profile);
    void set_target_clears(int target_clears);
    void set_max_runs(int max_runs);
    bool _run() override;

private:
    // --- Low-level Controller & Timing ---
    static Point to_720p(double x_1080, double y_1080);
    cv::Mat shot() const;
    bool sleep_ms(int ms);
    bool sleep_sec(double sec);
    void tap(double x, double y, int wait_ms = 600);
    void swipe(double x1, double y1, double x2, double y2, int duration_ms = 650, int wait_ms = 600);
    void touch_event(int type, double x, double y) const;
    void drag_segments(
        double x1,
        double y1,
        double x2,
        double y2,
        int steps = 6,
        int step_ms = 25,
        int hold_ms = 80);
    cv::Mat drag_board_unit(
        double x1,
        double y1,
        double x2,
        double y2,
        bool capture_before_up = false);

    // --- Logging & MAA GUI ExtraInfo Reporting ---
    void log_info(const std::string& msg);
    void report_gui_progress(const std::string& event_type, const std::string& message);
    void report_match_finished(bool won, int round_reached);
    void report_task_summary(const std::string& stop_reason, bool completed_ok);
    int total_available_strategies() const;

    // --- OCR & Text Matching (`rt`, `Jr`, `xr`, `yi`) ---
    std::vector<OcrItem> ocr(
        const cv::Mat& img,
        const std::optional<std::array<int, 4>>& roi_xywh = std::nullopt,
        bool pad_replicate = true) const;
    std::vector<OcrItem> ocr_rec_only(
        const cv::Mat& img,
        const std::array<int, 4>& roi_xywh,
        int upscale = 1,
        int black_border = 0) const;
    static std::optional<OcrItem> find_ocr(
        const std::vector<OcrItem>& items,
        std::initializer_list<std::string_view> keywords);
    static std::optional<OcrItem> find_ocr_exact(
        const std::vector<OcrItem>& items,
        std::initializer_list<std::string_view> exact_keywords);
    static std::optional<OcrItem> find_ocr_in_roi(
        const std::vector<OcrItem>& items,
        std::initializer_list<std::string_view> keywords,
        int x0,
        int y0,
        int x1,
        int y1);
    static bool has_ocr(
        const std::vector<OcrItem>& items,
        std::initializer_list<std::string_view> keywords);
    static bool has_ocr_exact(
        const std::vector<OcrItem>& items,
        std::initializer_list<std::string_view> exact_keywords);
    static std::string norm_text(std::string_view sv);
    static std::unordered_map<std::string, int> char_bag(std::string_view norm_sv);
    static double bag_similarity(
        const std::unordered_map<std::string, int>& a,
        int len_a,
        const std::unordered_map<std::string, int>& b,
        int len_b);
    static std::vector<DictEntry> build_dict(const std::vector<std::string>& names);
    static std::vector<DictMatch> match_dict(std::string_view text, const std::vector<DictEntry>& dict);
    static double match_coverage(std::string_view text, const std::vector<DictMatch>& matches);

    // --- Data Loading (`oa`) ---
    bool load_all_data();
    void init_builtin_fallbacks();
    static StackTraitInfo parse_stack_trait(std::string_view trait_text);
    static std::vector<std::pair<int, int>> default_range_for_subprof(
        std::string_view sub_prof,
        std::string_view pos);
    static std::pair<int, int> rotate_offset(std::pair<int, int> dr_dc, std::string_view facing);
    const OperatorInfo* find_operator(std::string_view name) const;
    const ItemInfo* find_item(std::string_view name) const;
    const BondInfo* find_bond(std::string_view name) const;
    const StageInfo* find_stage(std::string_view stage_id) const;

    // --- Projective Homography & Camera Calibration (`kr`, `ft`, `os`, `Un`, `pn`, `wn`, `Hn`, `Sr`) ---
    static std::array<double, 9> solve_homography(const std::vector<std::array<double, 4>>& anchors);
    static std::pair<double, double> project_point(const std::array<double, 9>& H, double gx, double gy);
    static std::array<double, 9> invert_homography(const std::array<double, 9>& H);
    static std::array<double, 9> adjust_homography(const std::array<double, 9>& H, const SimilarityFit& fit);
    static std::optional<std::tuple<double, double, int>> detect_marker_centroid(
        const cv::Mat& img,
        const std::array<int, 4>& roi_xyxy,
        std::string_view color_kind);
    static SimilarityFit fit_similarity(
        const std::vector<std::pair<double, double>>& src_pts,
        const std::vector<std::pair<double, double>>& dst_pts);
    CameraDetectResult detect_camera(const cv::Mat& img, bool allow_goal_only = false) const;
    std::array<double, 9> active_board_homography(const CameraDetectResult& cam) const;
    std::optional<StableBoardState> wait_stable_board(
        int timeout_ms = 5000,
        const std::optional<std::array<double, 4>>& prev_pts = std::nullopt);

    // --- Stage Identification & Info Screen (`Ri`, `ji`, `Ni`, `ki`, `lr`, `F`) ---
    std::unordered_map<std::string, std::vector<double>> sample_board_cells(
        const cv::Mat& img,
        const std::array<double, 9>& H) const;
    void update_stage_from_board(
        const cv::Mat& img,
        const std::array<double, 9>& H,
        std::string_view camera,
        const std::set<std::string>& occupied_cells);
    std::optional<InfoScreenData> read_info_screen(const cv::Mat& img) const;
    std::pair<std::optional<int>, bool> read_round_and_prep(
        const cv::Mat& img,
        const std::vector<OcrItem>& full_items) const;
    std::optional<int> read_remaining_deploy(const cv::Mat& img) const;

    // --- Shop Reader (`Kr`, `Kn`, `Rt`, `Bi`, `Ci`, `Di`, `Mt`) ---
    std::optional<int> read_shop_digits_4x(const cv::Mat& img, const std::array<int, 4>& roi_xywh) const;
    static std::optional<int> recognize_shop_level_roman_cc(const cv::Mat& img, int up_x);
    ShopHeaderState read_shop_header(const cv::Mat& img, const std::vector<OcrItem>& full_items) const;
    static std::vector<int> detect_card_gaps(const cv::Mat& img);
    std::vector<ShopCard> read_shop_cards(const cv::Mat& img);
    void detect_free_cards(const cv::Mat& img, std::vector<ShopCard>& cards) const;

    // --- Board Unit Detection, Sprite Histogram Tracking & Detail Panel (`he`, `yn`, `ta`, `Oi`, `Gt`, `Ce`, `Pt`, `zr`) ---
    static cv::Rect unit_sprite_box(const std::array<double, 9>& H, int r, int c, std::string_view camera);
    static double green_hp_bar_ratio(const cv::Mat& img, double cx, double top_y, double box_h);
    static std::vector<double> compute_hsv_hist(const cv::Mat& img, const cv::Rect& roi, const cv::Mat* bg_ref = nullptr);
    static double hist_intersection(const std::vector<double>& a, const std::vector<double>& b);
    std::vector<std::pair<double, double>> detect_unit_badges(
        const cv::Mat& img,
        const std::array<double, 9>& H,
        std::string_view camera,
        const std::vector<std::string>& rows) const;
    std::vector<UnitSlot> map_badges_to_units(
        const std::vector<std::pair<double, double>>& badges,
        const std::array<double, 9>& H,
        std::string_view camera,
        const std::vector<std::string>& rows) const;
    bool is_detail_panel_open(const cv::Mat& img) const;
    bool close_detail_panel();
    std::pair<std::string, std::string> read_unit_detail_panel(const cv::Mat& img) const;
    void refresh_board_snapshot(const cv::Mat& img, const CameraDetectResult& cam, bool force_shop = true);
    void inspect_unknown_units(const CameraDetectResult& cam, bool skip_bench_unknown = false);
    void check_triple_merge(const std::string& op_name);

    // --- Traffic Analysis & Simulated Annealing Placement Solver (`Rr`, `Nt`, `Zr`, `is`, `ss`, `cr`, `ws`) ---
    StageTrafficMap build_stage_traffic(std::string_view stage_id) const;
    GrazeStats compute_cell_graze(
        const StageTrafficMap& traffic,
        const OperatorInfo& op,
        int r,
        int c,
        std::string_view facing) const;
    PlacementEvalResult evaluate_placement_full(
        const std::vector<PlacementAssign>& placement,
        const std::vector<UnitSlot>& bench_units,
        const StageTrafficMap& traffic,
        const PlacementEvalOptions& opts) const;
    double evaluate_placement(
        const std::vector<PlacementAssign>& placement,
        const std::vector<UnitSlot>& bench_units,
        const StageTrafficMap& traffic,
        bool advance_mode = false) const;
    std::pair<std::vector<PlacementAssign>, std::vector<PlacementMove>> solve_placement(
        const StageTrafficMap& traffic,
        int max_deploy,
        bool advance_mode = false) const;
    static std::optional<std::pair<double, double>> detect_facing_white_box(const cv::Mat& img);
    bool execute_placement_move(const PlacementMove& mv, const CameraDetectResult& cam);
    int run_auto_place(const CameraDetectResult& cam, bool advance_mode = false);

    // --- Shop Multi-Step Action Planner & Execution (`_r`, `gs`, `ps`, `Lr`, `ha`, `ur`, `ct`, `oe`) ---
    static RoundPacing get_round_pacing(int round);
    double score_operator_for_plan(
        const OperatorInfo& op,
        const std::unordered_map<std::string, int>& owned_counts,
        const std::unordered_set<std::string>& active_bonds) const;
    std::vector<ShopPlanStep> plan_shop_Steps(
        const ShopHeaderState& header,
        const std::vector<ShopCard>& shop);
    bool buy_shop_card(int card_x, const std::string& expected_name, int upgrade_x_hint);
    bool upgrade_shop(const ShopHeaderState& header);
    bool sell_unit_slot(const std::string& slot, const CameraDetectResult& cam);

    // --- Equipment Row `Q`, Classification (`Es`/`Fs`/`ma`/`vs`/`An`/`ga`), Jessica Pager, Mob Elite & Summons ---
    static std::string classify_item_category(std::string_view name, std::string_view effect);
    const UnitSlot* pick_equipment_target(std::string_view category, const ItemInfo* item_info) const;
    void handle_overflow_units(const CameraDetectResult& cam);
    void scan_and_distribute_equipment(const cv::Mat& img, const CameraDetectResult& cam);
    bool try_use_jessica_pager(const CameraDetectResult& cam);
    bool try_use_mob_skill(const CameraDetectResult& cam, int gold);
    int deploy_summons(const CameraDetectResult& cam);
    void run_prep_phase(const cv::Mat& img, const CameraDetectResult& cam);

    // --- Screen State Machine & Strategy/Squad/Popup Handlers (`bi`, `Ot`, `K`, `ce`, `ln`) ---
    ScreenState classify_screen(const cv::Mat& img, const std::vector<OcrItem>& items) const;
    static cv::Mat extract_strategy_icon(const cv::Mat& img, int gx, int gy);
    static double icon_diff(const cv::Mat& a, const cv::Mat& b);
    static bool has_strategy_selected_border(const cv::Mat& img, int gx, int gy);
    static int detect_strategy_badge_tier(const cv::Mat& img, int gx, int gy);
    bool handle_strategy_screen(const cv::Mat& img);
    void handle_squad_screen(const cv::Mat& img, const std::vector<OcrItem>& items);
    void handle_info_screen(const cv::Mat& img, const std::vector<OcrItem>& items);
    void handle_three_choice_screen(const cv::Mat& img, const std::vector<OcrItem>& items);
    void handle_decision_screen(const cv::Mat& img, const std::vector<OcrItem>& items);

    // --- Configuration & Runtime State ---
    std::string m_profile = "full";
    std::string m_goal = "medal";
    int m_target = 3;
    int m_runs = 20;
    int m_attempts = 0;
    double m_step_scale = 1.0;

    bool m_data_loaded = false;
    std::vector<OperatorInfo> m_operators;
    std::unordered_map<std::string, size_t> m_op_by_norm;
    std::vector<BondInfo> m_bonds;
    std::unordered_map<std::string, size_t> m_bond_by_norm;
    std::vector<ItemInfo> m_items;
    std::unordered_map<std::string, size_t> m_item_by_norm;
    std::vector<StrategyRule> m_strategies;
    std::vector<StageInfo> m_stages;
    std::vector<BoardRefEntry> m_board_refs;
    std::vector<BossInfo> m_bosses;
    std::vector<EnemyTypeInfo> m_enemy_types;
    CameraCalibData m_calib_expanded;
    CameraCalibData m_calib_collapsed;
    std::array<double, 9> m_collapsed_board_H = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

    std::vector<DictEntry> m_chess_dict;
    std::vector<DictEntry> m_item_dict;
    std::vector<DictEntry> m_bond_dict;
    std::vector<DictEntry> m_boss_dict;
    std::vector<DictEntry> m_strategy_dict;

    // Active goal bonds & operator sets
    std::vector<std::string> m_active_core_bonds = { "精准", "奥术" };

    // Match & Board Tracking State
    bool m_match_info_read = false;
    std::string m_stage_id = "act3autochess_m01";
    std::string m_stage_source = "default";
    std::pair<std::string, int> m_stage_vote = { "", 0 };
    int m_current_round = 0;
    int m_last_prep_round = -1;
    int m_current_shop_level = 1;
    int m_current_gold = 0;
    int m_deploy_cap = 8;
    std::optional<int> m_upgrade_x_Ref;

    std::vector<UnitSlot> m_units;
    std::vector<ShopCard> m_shop;
    std::unordered_map<std::string, UnitSlot> m_remembered_units;
    std::unordered_map<std::string, std::string> m_panel_verified_slots;
    std::unordered_map<std::string, std::vector<std::vector<double>>> m_op_hist_bank;
    std::unordered_map<std::string, cv::Mat> m_empty_cell_refs;
    std::vector<std::string> m_recent_bought_names;
    std::unordered_set<std::string> m_bought_items;
    std::string m_current_boss;
    std::unordered_map<int, EquipSlotInfo> m_equip_slots;
    std::unordered_map<std::string, std::string> m_unit_equipped_bond;
    std::unordered_map<std::string, int> m_unit_equipped_count;
    bool m_mob_used_this_round = false;
    int m_pager_used_round = -1;

    // Strategy & Task Completion Progress
    std::string m_current_strategy;
    std::string m_current_strategy_id;
    std::set<std::string> m_done_strategies;
    std::map<std::string, int> m_strategy_clears;
    struct SeenStrategyIcon
    {
        std::string name;
        cv::Mat icon;
    };
    std::vector<SeenStrategyIcon> m_seen_strategy_icons;
    int m_runs_completed = 0;
    int m_attempts_done = 0;
    int m_medals_earned = 0;

    void reset_match_state();
};

} // namespace asst