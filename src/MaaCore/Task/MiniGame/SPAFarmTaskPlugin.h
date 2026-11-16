#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/AsstTypes.h"
#include "Task/AbstractTaskPlugin.h"
#include "Vision/OCRer.h"
#include "Vision/RegionOCRer.h"

namespace asst
{

class SPAFarmTaskPlugin : public AbstractTaskPlugin
{
public:
    using AbstractTaskPlugin::AbstractTaskPlugin;
    virtual ~SPAFarmTaskPlugin() override = default;

    // 基类 verify 是 const；set_params 基类无此方法，故不加 override
    virtual bool verify(AsstMsg msg, const json::value& details) const override;
    bool set_params(const json::value& params);
    virtual bool _run() override;

private:
    struct OcrItem
    {
        double cx = 0.0;
        double cy = 0.0;
        std::string text;
        double score = 0.0;
    };

    struct ShopCard
    {
        int i = 0;
        int x = 0;
        bool empty = false;
        std::optional<int> cost;
        bool hi = false;
        bool op = false;
        std::string name;
        bool dps = false;
    };

    struct OccupantInfo
    {
        std::string name;
        int tier = 1;
        bool ranged = false;
    };

    struct BoardOp
    {
        int x = 0;
        int y = 0;
        int tier = 1;
        std::pair<int, int> tile = { 0, 0 };
    };

    struct Board
    {
        std::vector<BoardOp> ops;
        int round_done = -1;
        std::vector<std::pair<int, int>> used;
        bool pending = false;
        std::vector<std::pair<int, int>> hi_tiles;
        bool prep_done = false;
        double last_ready = 0.0;
        std::unordered_map<std::string, int> owned;
        int healers = 0;
        std::unordered_map<std::string, OccupantInfo> occ;
    };

    struct LayoutConfig
    {
        bool loaded = false;
        std::vector<std::pair<int, int>> safe;
        std::vector<std::pair<int, int>> path;
        std::vector<std::pair<int, int>> rest;
        std::pair<int, int> lift = { 760, 650 };
    };

    // --- 控制器与坐标映射 (1920x1080 <-> 1280x720) ---
    static Point to_720p(int x_1080, int y_1080);
    cv::Mat shot() const;
    bool sleep_sec(double sec);
    void tap(double x, double y, double wait_sec = 0.6);
    void swipe(double x1, double y1, double x2, double y2, int ms = 500, double wait_sec = 0.8);
    void drag_move(int x1, int y1, int x2, int y2, int duration_ms = 500, double post_wait_sec = 0.8);
    void log_info(const std::string& msg);

    // --- 核心 OCR 接口 (修复四大准确率问题) ---
    std::vector<OcrItem> ocr(
        const cv::Mat& img,
        const std::optional<std::array<int, 4>>& box = std::nullopt) const;
    static std::optional<OcrItem> find(
        const std::vector<OcrItem>& items,
        std::initializer_list<std::string_view> kw);
    std::optional<int> numrec(const cv::Mat& img, const std::array<int, 4>& box) const;
    std::optional<int> num(const cv::Mat& img, const std::array<int, 4>& box) const;

    // --- 颜色与图像分析 ---
    static double yellow_ratio(const cv::Mat& img, const std::array<int, 4>& box);
    static bool has_color_badge(const cv::Mat& img, int gx, int gy);
    static bool has_badge(const cv::Mat& img, int gx, int gy);
    static int passed_count(const std::vector<OcrItem>& panel);
    static std::optional<int> grid_shift(const cv::Mat& before, const cv::Mat& after);
    static std::vector<int> detect_rows(const cv::Mat& img);
    void scroll_grid();
    static std::string strategy_name(const std::vector<OcrItem>& panel);
    std::optional<std::string> pick_strategy(const std::set<std::string>& done);

    // --- 干员权重与编队选择 ---
    void apply_profile(const std::string& name);
    static bool match_key(std::string_view name, std::string_view key);
    std::optional<int> prio_index(std::string_view name) const;
    bool is_safe_op(std::string_view name) const;
    static bool is_healer(std::string_view name);
    static bool is_dps(std::string_view name);
    static double seq_ratio(std::string_view a, std::string_view b);
    int rank(std::string_view name, int tier) const;
    void pick_squad();

    // --- 商店与棋盘部署 ---
    std::tuple<std::optional<int>, std::optional<int>, bool, std::optional<int>> read_shop(const cv::Mat& img);
    std::vector<ShopCard> shop_cards(const cv::Mat& img) const;
    bool buy(int i, std::optional<int> cost = std::nullopt);
    static std::vector<std::pair<int, int>> green_tiles(const cv::Mat& img);
    static std::vector<std::pair<int, int>> right3(
        const std::vector<std::pair<int, int>>& tiles,
        const cv::Mat& img);
    void load_layout();
    static bool is_green(const cv::Mat& img, std::pair<int, int> pt, int r = 22);
    std::string drag_name(const cv::Mat& img) const;
    static bool near_pt(std::pair<int, int> t, const std::vector<std::pair<int, int>>& lst, int d = 95);
    std::vector<std::string> tile_order(std::string_view name) const;
    std::pair<int, int> tile_xy(std::string_view tn) const;

    std::optional<std::pair<int, int>> deploy_from_bench(
        std::pair<int, int> bench_xy,
        bool quiet = false,
        Board* board = nullptr,
        bool hi = false);
    std::optional<std::pair<int, int>> deploy_from_bench_old(
        std::pair<int, int> bench_xy,
        bool quiet = false,
        Board* board = nullptr,
        bool hi = false);
    std::optional<std::pair<int, int>> deploy_layout(
        std::pair<int, int> bench_xy,
        Board& board,
        std::string name_hint = "",
        int tier_hint = 0,
        int depth = 0);

    bool sell_at(int x, int y);
    bool in_facing_mode(const cv::Mat* img = nullptr) const;
    bool set_facing(int tx, int ty, int tries = 3);
    std::optional<int> slots_left(const cv::Mat& img) const;
    static std::optional<std::pair<int, int>> new_bench_slot(const cv::Mat& before, const cv::Mat& after);
    bool buy_and_deploy(Board& board, int i, int tier, std::optional<int> cost = std::nullopt);
    void deploy_bench(Board& board);
    bool handle_popups();
    std::optional<int> read_gold(const cv::Mat* img = nullptr, int tries = 3);
    std::optional<int> gold_now();
    void prep_phase(Board& board, const cv::Mat& img);

    // --- 状态与配置成员 ---
    std::string m_profile = "full";
    int m_target = 3;
    int m_runs = 20;
    int m_attempts = 0;

    std::vector<std::pair<std::string, std::string>> m_priority;
    std::vector<std::string> m_safe_keys;
    LayoutConfig m_layout;

    int m_upx = 100;
    std::vector<int> m_cards = { 304, 550, 790, 1034, 1274, 1520 };
    int m_last_tier = 1;
    std::optional<std::pair<int, int>> m_last_tile;
    std::string m_last_name;
    int m_gold_fail = 0;
};

} // namespace asst