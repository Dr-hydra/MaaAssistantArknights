#pragma once

#include "Task/AbstractTaskPlugin.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace cv
{
class Mat;
}

namespace asst
{
// 卫戍协议 · 极速模拟 · 自动刷策略角标
//
// 移植自 GarrisonFarm（garrison_farm.py / garrison_farm.cpp）：策略网格识别、角标判定、干员优先级、
// 商店三阶段运营、固定布局部署与换位售卖等判定阈值、坐标公式和分支顺序保持原实现不变，
// 只把输入输出替换为 MAA 的截图 / 触控 / OCR / 日志 / 回调接口。
//
// 坐标：原实现按 1920x1080 设计，MAA 逻辑画布为 1280x720（Controller 内部再按设备分辨率缩放）。
// 插件把每帧截图统一缩放到 1920x1080 后按原坐标计算，点击/滑动时再等比换算回 MAA 逻辑坐标，
// 因此原实现的全部阈值、卷积核与拟合公式都可以直接沿用。
class SPAFarmTaskPlugin final : public AbstractTaskPlugin
{
public:
    using AbstractTaskPlugin::AbstractTaskPlugin;
    ~SPAFarmTaskPlugin() override = default;

    bool verify(AsstMsg msg, const json::value& details) const override;

    // 低配模式：不需要自选编队，使用低配干员优先级表
    void set_profile(bool low_profile) { m_low_profile = low_profile; }

    // 单个策略需要通关的次数：3 = 金色角标，6 = 彩色角标
    void set_target_clears(int clears) { m_target_clears = clears; }

    // 最多通关局数，<= 0 表示不限
    void set_max_runs(int runs) { m_max_runs = runs; }

protected:
    bool _run() override;

private:
    struct OcrItem
    {
        double x = 0;
        double y = 0;
        std::string text;
    };

    struct Box
    {
        int x1 = 0;
        int y1 = 0;
        int x2 = 0;
        int y2 = 0;
    };

    struct OpRecord
    {
        int x = 0;
        int y = 0;
        int tier = 1;
        std::optional<std::pair<int, int>> tile;
    };

    struct OccupantInfo
    {
        std::string name;
        int tier = 1;
        bool ranged = false;
    };

    struct ShopCard
    {
        int index = 0;
        int x = 0;
        bool empty = false;
        std::optional<int> cost;
        bool high_tier = false;
        bool is_operator = false;
        std::string name;
        bool dps = false;
    };

    struct Board
    {
        std::vector<OpRecord> ops;
        int round_done = -1;
        std::vector<std::pair<int, int>> used;
        bool pending = false;
        std::vector<std::pair<int, int>> hi_tiles;
        bool prep_done = false;
        double last_ready = 0.0;
        std::map<std::string, int> owned;
        int healers = 0;
        std::map<std::string, OccupantInfo> occ;
    };

    // ---- 画布与输入输出（MAA 接口）----
    cv::Mat capture() const;
    void click_at(int x, int y, double wait_sec) const;
    void swipe_to(int x1, int y1, int x2, int y2, int duration_ms, double wait_sec) const;
    void touch_event(int type, int x, int y) const;
    void drag_path(int x1, int y1, int x2, int y2, int steps = 12) const;
    void scroll_grid() const;
    void save_snapshot(const cv::Mat& image, const std::string& tag) const;

    // ---- OCR ----
    std::vector<OcrItem> ocr(const cv::Mat& image, std::optional<Box> box = std::nullopt) const;
    std::optional<OcrItem> find(const std::vector<OcrItem>& items, const std::vector<std::string>& keywords) const;
    std::optional<int> read_number(const cv::Mat& image, Box box) const;
    std::optional<int> read_number_rec(const cv::Mat& image, Box box) const;
    int passed_count(const std::vector<OcrItem>& panel) const;
    std::string strategy_name(const std::vector<OcrItem>& panel) const;
    static std::string join_text(const std::vector<OcrItem>& items);

    // ---- 策略网格 / 角标 ----
    std::optional<int> grid_shift(const cv::Mat& before, const cv::Mat& after) const;
    std::vector<int> detect_rows(const cv::Mat& image) const;
    std::vector<std::pair<int, int>> green_tiles(const cv::Mat& image) const;
    std::vector<std::pair<int, int>>
        right3(const std::vector<std::pair<int, int>>& tiles, const cv::Mat& image) const;
    std::optional<std::string> pick_strategy();

    // ---- 干员与编队 ----
    void apply_profile();
    std::optional<int> priority_index(const std::string& name) const;
    bool is_safe_operator(const std::string& name) const;
    static bool is_healer(const std::string& name);
    static bool is_dps(const std::string& name);
    int rank_operator(const std::string& name, int tier) const;
    void pick_squad();

    // ---- 商店 / 棋盘 ----
    std::tuple<std::optional<int>, std::optional<int>, bool, std::optional<int>> read_shop(const cv::Mat& image);
    std::vector<ShopCard> shop_cards(const cv::Mat& image) const;
    bool buy(int index, std::optional<int> cost = std::nullopt);
    std::optional<int> read_gold(const cv::Mat* image = nullptr, int tries = 3) const;
    std::optional<int> gold_now();

    // ---- 部署 ----
    static bool is_green(const cv::Mat& image, std::pair<int, int> point, int radius = 22);
    std::string drag_name(const cv::Mat& image) const;
    std::optional<int> slots_left(const cv::Mat& image) const;
    static std::optional<std::pair<int, int>> new_bench_slot(const cv::Mat& before, const cv::Mat& after);
    bool sell_at(int x, int y) const;
    bool in_facing_mode(const cv::Mat* image = nullptr) const;
    bool set_facing(int tx, int ty, int tries = 3);
    std::vector<std::string> tile_order(const std::string& name) const;
    static std::pair<int, int> tile_xy(const std::string& tile_name);
    std::optional<std::pair<int, int>> deploy_layout(
        std::pair<int, int> bench_xy,
        Board& board,
        const std::string& name_hint = "",
        int tier_hint = 0,
        int depth = 0);
    std::optional<std::pair<int, int>> deploy_from_bench_old(
        std::pair<int, int> bench_xy,
        bool quiet = false,
        Board* board = nullptr,
        bool hi = false);
    std::optional<std::pair<int, int>> deploy_from_bench(
        std::pair<int, int> bench_xy,
        bool quiet = false,
        Board* board = nullptr,
        bool hi = false);
    bool sell_lowest(Board& board);
    bool handle_popups() const;
    bool buy_and_deploy(Board& board, int index, int tier, std::optional<int> cost = std::nullopt);
    void deploy_bench(Board& board);

    // ---- 休整期状态机 ----
    void prep_phase(Board& board, const cv::Mat& image);

    // ---- 进度持久化 ----
    std::set<std::string> load_done() const;
    void save_done(const std::set<std::string>& done) const;

    // ---- 上报 ----
    void report_log(const std::string& message, const std::string& level = "info");
    void report_progress();

    bool m_low_profile = false;
    int m_target_clears = 3;
    int m_max_runs = 20;

    int m_successful_runs = 0;
    int m_attempts = 0;
    int m_gold_fail = 0;
    std::string m_current_strategy;

    // 原实现中的全局可变状态，收敛为成员
    int m_up_x = 100;
    std::vector<int> m_cards;
    int m_last_tier = 1;
    std::optional<std::pair<int, int>> m_last_tile;

    std::vector<std::pair<std::string, std::string>> m_priority;
    std::vector<std::string> m_safe_keys;
};
} // namespace asst
