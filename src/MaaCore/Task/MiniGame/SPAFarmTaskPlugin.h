#pragma once

#include "Task/AbstractTaskPlugin.h"
#include "Vision/OCRer.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asst
{
class SPAFarmTaskPlugin final : public AbstractTaskPlugin
{
public:
    using AbstractTaskPlugin::AbstractTaskPlugin;
    virtual ~SPAFarmTaskPlugin() override = default;

    bool verify(AsstMsg msg, const json::value& details) const override;
    void set_mode(bool low_profile, int target_clears) noexcept;

protected:
    bool _run() override;

private:
    using OcrResults = OCRer::ResultsVec;
    struct ShopCard
    {
        int index = 0;
        int x = 0;
        int tier = 0;
        int cost = -1;
        std::string name;
        bool is_operator = false;
        bool is_dps = false;
        bool is_healer = false;
    };

    OcrResults recognize(const Rect& roi = {}) const;
    double yellow_ratio(const cv::Mat& image, const Rect& roi) const;
    bool has_color_badge(const cv::Mat& image, const Point& grid_point) const;
    bool has_badge(const cv::Mat& image, const Point& grid_point) const;
    std::optional<int> grid_shift(const cv::Mat& before, const cv::Mat& after) const;
    std::vector<int> detect_strategy_rows(const cv::Mat& image) const;
    bool click_canvas(const Point& point);
    bool click_canvas(const Rect& rect);
    bool swipe_canvas(const Point& from, const Point& to, int duration);
    std::vector<ShopCard> scan_shop(const cv::Mat& image) const;
    bool buy_shop_card(const ShopCard& card, cv::Mat& before, cv::Mat& after);
    std::optional<Point> locate_new_bench_slot(const cv::Mat& before, const cv::Mat& after) const;
    bool deploy_from_bench(const Point& bench, const std::string& name, int tier);
    std::optional<int> read_number(const OcrResults& results) const;
    bool contains(const OcrResults& results, std::string_view text) const;
    std::optional<OCRer::Result> find_text(const OcrResults& results, std::string_view text) const;
    std::optional<std::string> choose_strategy();
    std::optional<int> read_passed_count(const OcrResults& results) const;
    std::optional<std::string> read_strategy_name(const OcrResults& results) const;
    bool choose_squad();
    bool handle_screen(const OcrResults& results, std::string& current_strategy, int& clears);
    bool handle_preparation(const OcrResults& results, int round);
    void report(std::string what, json::value details = json::object());

    int m_target_clears = 3;
    bool m_low_profile = false;
    bool m_completed = false;
    int m_initial_clears = 0;
    std::string m_abort_reason;
    std::vector<Point> m_used_tiles;
    std::vector<std::string> m_done_strategies;
};
} // namespace asst
