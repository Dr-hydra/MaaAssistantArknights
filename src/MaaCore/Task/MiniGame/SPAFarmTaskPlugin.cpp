#include "SPAFarmTaskPlugin.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <regex>
#include <span>
#include <unordered_set>
#include <utility>

#include "Controller/Controller.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"

namespace
{
constexpr std::string_view SpaEntry = "MiniGame@SPA@Begin";
constexpr int MaxStrategyPages = 8;
constexpr int MaxActions = 1200;
constexpr auto NoProgressLimit = std::chrono::minutes(10);
constexpr auto LeavePageLimit = std::chrono::seconds(30);

const std::array<int, 4> StrategyColumns { 680, 890, 1100, 1310 };
const std::array<int, 4> StrategyRows { 370, 580, 790, 1000 };
const std::array<int, 6> ShopCardColumns { 304, 550, 790, 1034, 1274, 1520 };
const std::array<std::string_view, 14> FullPriority {
    "塔露拉", "维什戴尔", "斩业星熊", "予愿安洁莉娜", "新约能天使", "凯尔希·思衡托", "望",
    "圣聆初雪", "逻各斯", "蕾缪安", "贝洛内", "冻河", "赤刃明霄陈", "结城理"
};
const std::array<std::string_view, 24> LowPriority {
    "塔露拉", "史尔特尔", "耀骑士临光", "逻各斯", "克莱门莎", "锏", "帕拉斯", "真言", "蕾缪安", "烛煌",
    "艾丽妮", "薇薇安娜", "维娜·维多利亚", "乌尔比安", "棘刺", "怒潮凛冬", "圣聆初雪", "迷迭香",
    "鸿雪", "贝洛内", "佩佩", "隐德来希", "冻河", "酒神"
};
const std::array<std::string_view, 12> HealerNames {
    "褐果", "莎草", "哈洛德", "瑰盐", "乌啾", "风絮", "流明", "夜莺", "华法琳", "纯烬艾雅法拉", "Mon3tr", "Touch"
};
const std::array<std::string_view, 6> RomanTiers { "I", "II", "III", "IV", "V", "VI" };
const std::array<std::string_view, 7> FullSafeNames {
    "维什戴尔", "望", "新约能天使", "圣聆初雪", "蕾缪安", "逻各斯", "冻河"
};
const std::array<std::string_view, 9> LowSafeNames {
    "逻各斯", "真言", "蕾缪安", "烛煌", "圣聆初雪", "迷迭香", "鸿雪", "冻河", "酒神"
};
const std::array<std::pair<std::string_view, std::string_view>, 14> FullPriorityKeys {{
    { "塔露拉", "塔露拉" }, { "维什戴尔", "维什戴尔" }, { "星熊", "斩业星熊" }, { "安洁莉娜", "予愿安洁莉娜" },
    { "能天使", "新约能天使" }, { "凯尔希", "凯尔希·思衡托" }, { "望", "望" }, { "初雪", "圣聆初雪" },
    { "逻各斯", "逻各斯" }, { "蕾缪安", "蕾缪安" }, { "贝洛内", "贝洛内" }, { "冻河", "冻河" },
    { "明霄陈", "赤刃明霄陈" }, { "结城理", "结城理" }
}};
const std::array<std::pair<std::string_view, std::string_view>, 24> LowPriorityKeys {{
    { "塔露拉", "塔露拉" }, { "史尔特尔", "史尔特尔" }, { "耀骑士", "耀骑士临光" }, { "逻各斯", "逻各斯" },
    { "克莱门莎", "克莱门莎" }, { "锏", "锏" }, { "帕拉斯", "帕拉斯" }, { "真言", "真言" },
    { "蕾缪安", "蕾缪安" }, { "烛煌", "烛煌" }, { "艾丽妮", "艾丽妮" }, { "薇薇安娜", "薇薇安娜" },
    { "维娜", "维娜·维多利亚" }, { "乌尔比安", "乌尔比安" }, { "棘刺", "棘刺" }, { "凛冬", "怒潮凛冬" },
    { "初雪", "圣聆初雪" }, { "迷迭香", "迷迭香" }, { "鸿雪", "鸿雪" }, { "贝洛内", "贝洛内" },
    { "佩佩", "佩佩" }, { "隐德来希", "隐德来希" }, { "冻河", "冻河" }, { "酒神", "酒神" }
}};
constexpr std::string_view DpsNames =
    "怒潮凛冬 艾丽妮 棘刺 佩佩 圣聆初雪 乌尔比安 薇薇安娜 贝洛内 维娜·维多利亚 维娜 迷迭香 鸿雪 阿米娅 "
    "赤刃明霄陈 司霆惊蛰 维什戴尔 斩业星熊 丰川祥子 结城理 凛御银灰 歌蕾蒂娅 Sharp 领主·Sharp Pith Stormeye "
    "预备干员-近卫 预备干员-术师 引星棘刺 克莱门莎 耀骑士临光 荒芜拉普兰德 逻各斯 真言 史尔特尔 锏 塔露拉 夕 蕾缪安 帕拉斯 望 塑心";
bool contains_text(std::string_view source, std::string_view needle)
{
    return source.find(needle) != std::string_view::npos;
}

bool contains_cjk(std::string_view text)
{
    for (size_t index = 0; index + 2 < text.size(); ++index) {
        const auto lead = static_cast<unsigned char>(text[index]);
        const auto middle = static_cast<unsigned char>(text[index + 1]);
        const auto tail = static_cast<unsigned char>(text[index + 2]);
        if (lead >= 0xE4 && lead <= 0xE9 && (middle & 0xC0) == 0x80 && (tail & 0xC0) == 0x80) {
            return true;
        }
    }
    return false;
}

std::string compact(std::string text)
{
    std::erase_if(text, [](unsigned char ch) { return std::isspace(ch) != 0; });
    return text;
}

size_t utf8_codepoint_count(std::string_view text)
{
    return static_cast<size_t>(std::ranges::count_if(text, [](unsigned char ch) { return (ch & 0xC0) != 0x80; }));
}

bool match_priority_key(std::string_view name, std::string_view key)
{
    const auto first = std::ranges::find_if_not(name, [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto last = std::find_if_not(name.rbegin(), name.rend(), [](unsigned char ch) { return std::isspace(ch) != 0; });
    const std::string_view trimmed = first >= last.base() ? std::string_view {} :
                                                           std::string_view(&*first, static_cast<size_t>(last.base() - first));
    if (utf8_codepoint_count(key) == 1) {
        return trimmed == key || (utf8_codepoint_count(trimmed) <= 2 && contains_text(trimmed, key));
    }
    return contains_text(name, key);
}

std::vector<uint32_t> utf8_codepoints(std::string_view text)
{
    std::vector<uint32_t> result;
    for (size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        uint32_t codepoint = lead;
        size_t length = 1;
        if ((lead & 0xE0) == 0xC0 && index + 1 < text.size()) {
            codepoint = lead & 0x1F;
            length = 2;
        }
        else if ((lead & 0xF0) == 0xE0 && index + 2 < text.size()) {
            codepoint = lead & 0x0F;
            length = 3;
        }
        else if ((lead & 0xF8) == 0xF0 && index + 3 < text.size()) {
            codepoint = lead & 0x07;
            length = 4;
        }
        for (size_t offset = 1; offset < length; ++offset) {
            codepoint = (codepoint << 6) | (static_cast<unsigned char>(text[index + offset]) & 0x3F);
        }
        result.emplace_back(codepoint);
        index += length;
    }
    return result;
}

double sequence_match_ratio(std::string_view left, std::string_view right)
{
    const auto a = utf8_codepoints(left);
    const auto b = utf8_codepoints(right);
    if (a.empty() || b.empty()) {
        return 0.0;
    }

    struct Span
    {
        size_t a_begin;
        size_t a_end;
        size_t b_begin;
        size_t b_end;
    };
    std::vector<Span> pending { { 0, a.size(), 0, b.size() } };
    size_t matches = 0;
    while (!pending.empty()) {
        const Span span = pending.back();
        pending.pop_back();
        size_t best_a = span.a_begin;
        size_t best_b = span.b_begin;
        size_t best_size = 0;
        std::vector<size_t> previous(b.size() + 1, 0);
        std::vector<size_t> current(b.size() + 1, 0);
        for (size_t i = span.a_begin; i < span.a_end; ++i) {
            for (size_t j = span.b_begin; j < span.b_end; ++j) {
                if (a[i] == b[j]) {
                    current[j + 1] = previous[j] + 1;
                    if (current[j + 1] > best_size) {
                        best_size = current[j + 1];
                        best_a = i + 1 - best_size;
                        best_b = j + 1 - best_size;
                    }
                }
                else {
                    current[j + 1] = 0;
                }
            }
            previous.swap(current);
            std::ranges::fill(current, 0);
        }
        if (best_size == 0) {
            continue;
        }
        matches += best_size;
        if (span.a_begin < best_a && span.b_begin < best_b) {
            pending.push_back({ span.a_begin, best_a, span.b_begin, best_b });
        }
        if (best_a + best_size < span.a_end && best_b + best_size < span.b_end) {
            pending.push_back({ best_a + best_size, span.a_end, best_b + best_size, span.b_end });
        }
    }
    return 2.0 * static_cast<double>(matches) / static_cast<double>(a.size() + b.size());
}

bool is_dps_name(std::string_view name)
{
    size_t start = 0;
    while (start < DpsNames.size()) {
        const size_t end = DpsNames.find(' ', start);
        const std::string_view key = DpsNames.substr(start, end == std::string_view::npos ? end : end - start);
        if (contains_text(name, key) || contains_text(key, name) || sequence_match_ratio(key, name) >= 0.6) {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

std::string normalize_digits(std::string text)
{
    static constexpr std::array<std::string_view, 10> FullWidthDigits {
        "０", "１", "２", "３", "４", "５", "６", "７", "８", "９"
    };
    for (size_t digit = 0; digit < FullWidthDigits.size(); ++digit) {
        size_t pos = 0;
        while ((pos = text.find(FullWidthDigits[digit], pos)) != std::string::npos) {
            text.replace(pos, FullWidthDigits[digit].size(), 1, static_cast<char>('0' + digit));
            ++pos;
        }
    }
    return text;
}
} // namespace

bool asst::SPAFarmTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    return msg == AsstMsg::SubTaskStart && details.get("subtask", std::string()) == "ProcessTask" &&
           details.get("details", "task", std::string()) == SpaEntry;
}

void asst::SPAFarmTaskPlugin::set_mode(bool low_profile, int target_clears) noexcept
{
    m_low_profile = low_profile;
    m_target_clears = target_clears == 6 ? 6 : 3;
}

asst::SPAFarmTaskPlugin::OcrResults asst::SPAFarmTaskPlugin::recognize(const Rect& roi) const
{
    cv::Mat image = ctrler()->get_image();
    if (image.empty()) {
        return {};
    }
    if (image.cols * 9 != image.rows * 16) {
        LogError << "SPA farm requires a 16:9 game image, got " << image.cols << "x" << image.rows;
        return {};
    }
    cv::resize(image, image, cv::Size(1920, 1080), 0.0, 0.0, cv::INTER_LINEAR);

    OCRer analyzer(image, roi, inst());
    analyzer.set_use_raw(true);
    if (auto result = analyzer.analyze()) {
        return std::move(*result);
    }
    return {};
}

double asst::SPAFarmTaskPlugin::yellow_ratio(const cv::Mat& image, const Rect& roi) const
{
    if (image.empty()) {
        return 0.0;
    }
    const int left = std::clamp(roi.x, 0, image.cols);
    const int top = std::clamp(roi.y, 0, image.rows);
    const int right = std::clamp(roi.x + roi.width, left, image.cols);
    const int bottom = std::clamp(roi.y + roi.height, top, image.rows);
    if (right <= left || bottom <= top) {
        return 0.0;
    }

    cv::Mat hsv;
    cv::Mat mask;
    cv::cvtColor(image(cv::Rect(left, top, right - left, bottom - top)), hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, cv::Scalar(8, 120, 150), cv::Scalar(35, 255, 255), mask);
    return cv::mean(mask)[0] / 255.0;
}

bool asst::SPAFarmTaskPlugin::has_color_badge(const cv::Mat& image, const Point& grid_point) const
{
    const Rect badge {
        grid_point.x + 40,
        std::max(0, grid_point.y - 110),
        55,
        65
    };
    if (image.empty() || badge.x + badge.width > image.cols || badge.y + badge.height > image.rows) {
        return false;
    }

    cv::Mat hsv;
    cv::Mat cyan;
    cv::cvtColor(image(cv::Rect(badge.x, badge.y, badge.width, badge.height)), hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, cv::Scalar(70, 40, 150), cv::Scalar(140, 255, 255), cyan);
    return cv::mean(cyan)[0] / 255.0 > 0.15 && yellow_ratio(image, badge) < 0.05;
}

bool asst::SPAFarmTaskPlugin::has_badge(const cv::Mat& image, const Point& grid_point) const
{
    const Rect first { grid_point.x + 40, std::max(0, grid_point.y - 100), 50, 50 };
    const Rect second { grid_point.x + 30, std::max(0, grid_point.y - 130), 70, 100 };
    return yellow_ratio(image, first) > 0.12 || yellow_ratio(image, second) > 0.10;
}

std::optional<int> asst::SPAFarmTaskPlugin::grid_shift(const cv::Mat& before, const cv::Mat& after) const
{
    constexpr int x = 600;
    constexpr int width = 800;
    constexpr int template_y = 560;
    constexpr int template_height = 140;
    constexpr int grid_top = 250;
    constexpr int grid_bottom = 1050;
    if (before.empty() || after.empty() || before.cols < x + width || after.cols < x + width ||
        before.rows < template_y + template_height || after.rows < grid_bottom) {
        return std::nullopt;
    }

    cv::Mat scores;
    cv::matchTemplate(
        after(cv::Rect(x, grid_top, width, grid_bottom - grid_top)),
        before(cv::Rect(x, template_y, width, template_height)),
        scores,
        cv::TM_CCOEFF_NORMED);
    double maximum = 0.0;
    cv::Point location;
    cv::minMaxLoc(scores, nullptr, &maximum, nullptr, &location);
    if (maximum < 0.6) {
        return std::nullopt;
    }
    return template_y - grid_top + location.y;
}

std::vector<int> asst::SPAFarmTaskPlugin::detect_strategy_rows(const cv::Mat& image) const
{
    constexpr int grid_top = 250;
    constexpr int grid_bottom = 1050;
    if (image.empty() || image.cols < 740 || image.rows <= grid_bottom + 19) {
        return {};
    }

    cv::Mat gray;
    cv::cvtColor(image(cv::Rect(620, 0, 120, image.rows)), gray, cv::COLOR_BGR2GRAY);
    std::vector<unsigned char> active(static_cast<size_t>(gray.rows));
    for (int y = grid_top - 60; y < grid_bottom + 20; ++y) {
        cv::Scalar mean;
        cv::Scalar deviation;
        cv::meanStdDev(gray.row(y), mean, deviation);
        active[static_cast<size_t>(y)] = deviation[0] > 18.0 ? 1 : 0;
    }

    std::vector<int> rows;
    int start = -1;
    for (int y = grid_top - 60; y < grid_bottom + 20; ++y) {
        if (active[static_cast<size_t>(y)] && start < 0) {
            start = y;
        }
        if ((!active[static_cast<size_t>(y)] || y == grid_bottom + 19) && start >= 0) {
            if (y - start >= 60) {
                const int center = (start + y) / 2;
                if (center >= grid_top && center <= grid_bottom - 20) {
                    rows.emplace_back(center);
                }
            }
            start = -1;
        }
    }
    return rows;
}

bool asst::SPAFarmTaskPlugin::click_canvas(const Point& point)
{
    return ctrler()->click(Point { point.x * 2 / 3, point.y * 2 / 3 });
}

bool asst::SPAFarmTaskPlugin::click_canvas(const Rect& rect)
{
    return click_canvas(Point { rect.x + rect.width / 2, rect.y + rect.height / 2 });
}

bool asst::SPAFarmTaskPlugin::swipe_canvas(const Point& from, const Point& to, int duration)
{
    return ctrler()->swipe(
        Point { from.x * 2 / 3, from.y * 2 / 3 },
        Point { to.x * 2 / 3, to.y * 2 / 3 },
        duration);
}

std::optional<int> asst::SPAFarmTaskPlugin::read_number(const OcrResults& results) const
{
    static const std::regex digits(R"(([0-9０-９]{1,3}))");
    for (const auto& result : results) {
        std::smatch match;
        const std::string normalized = normalize_digits(result.text);
        if (!std::regex_search(normalized, match, digits)) {
            continue;
        }
        int value = 0;
        const auto text = match[1].str();
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error == std::errc {} && end == text.data() + text.size()) {
            return value;
        }
    }
    return std::nullopt;
}

std::vector<asst::SPAFarmTaskPlugin::ShopCard> asst::SPAFarmTaskPlugin::scan_shop(const cv::Mat& image) const
{
    std::vector<ShopCard> cards;
    if (image.empty()) {
        return cards;
    }

    OCRer bottom_analyzer(image, Rect { 0, 820, 760, 190 }, inst());
    bottom_analyzer.set_use_raw(true);
    const auto bottom = bottom_analyzer.analyze().value_or(OcrResults {});
    int upgrade_x = 100;
    for (const auto& result : bottom) {
        const std::string text = compact(result.text);
        if (contains_text(text, "满级") || contains_text(text, "升级")) {
            upgrade_x = result.rect.x + result.rect.width / 2;
            break;
        }
    }
    std::vector<int> available_cards;
    std::ranges::copy_if(ShopCardColumns, std::back_inserter(available_cards), [upgrade_x](int x) {
        return x > upgrade_x + 150;
    });

    OCRer analyzer(image, Rect { 180, 770, 1520, 290 }, inst());
    analyzer.set_use_raw(true);
    const auto recognized = analyzer.analyze().value_or(OcrResults {});

    for (size_t index = 0; index < available_cards.size(); ++index) {
        const int x = available_cards[index];
        std::vector<const OCRer::Result*> local;
        for (const auto& result : recognized) {
            const int center_x = result.rect.x + result.rect.width / 2;
            if (std::abs(center_x - x) < 115) {
                local.emplace_back(&result);
            }
        }

        ShopCard card;
        card.index = static_cast<int>(index);
        card.x = x;
        bool empty = false;
        bool has_operator_text = false;
        int detected_tier = 0;
        std::vector<std::string> names;
        for (const auto* result : local) {
            const std::string text = compact(result->text);
            const int center_x = result->rect.x + result->rect.width / 2;
            const int center_y = result->rect.y + result->rect.height / 2;
            empty = empty || contains_text(text, "INFO");
            has_operator_text = has_operator_text || contains_cjk(text);

            if (center_y < 830 && std::abs(center_x - x) < 45 && !text.empty() &&
                std::ranges::all_of(text, [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
                card.cost = std::stoi(text);
            }

            if (center_y < 840 && center_x < x - 50) {
                std::string tier_text = text;
                std::ranges::transform(tier_text, tier_text.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::toupper(ch));
                });
                std::ranges::replace(tier_text, '1', 'I');
                std::ranges::replace(tier_text, 'L', 'I');
                for (size_t tier = 0; tier < RomanTiers.size(); ++tier) {
                    if (tier_text == RomanTiers[tier]) {
                        detected_tier = static_cast<int>(tier + 1);
                    }
                }
            }

            if (center_y >= 1000 &&
                (contains_cjk(text) ||
                 std::ranges::any_of(text, [](unsigned char ch) { return std::isalpha(ch) != 0; }))) {
                names.emplace_back(result->text);
            }
        }

        if (card.cost < 0 && !empty) {
            OCRer cost_analyzer(image, Rect { x - 22, 780, 44, 42 }, inst());
            cost_analyzer.set_use_raw(true);
            card.cost = read_number(cost_analyzer.analyze().value_or(OcrResults {})).value_or(-1);
        }

        if (detected_tier != 0) {
            card.tier = detected_tier >= 5 ? 5 : 1;
        }
        else {
            card.tier = yellow_ratio(image, Rect { x - 104, 800, 26, 26 }) > 0.1 ? 5 : 1;
        }
        if (!names.empty()) {
            card.name = compact(names.back());
        }
        card.is_operator = has_operator_text && !empty;
        card.is_dps = is_dps_name(card.name);
        card.is_healer = std::ranges::any_of(HealerNames, [&card](std::string_view name) {
            return !card.name.empty() &&
                   (contains_text(name, card.name) ||
                    (utf8_codepoint_count(card.name) >= 2 && contains_text(card.name, name)));
        });
        cards.emplace_back(std::move(card));
    }
    return cards;
}

bool asst::SPAFarmTaskPlugin::buy_shop_card(const ShopCard& card, cv::Mat& before, cv::Mat& after)
{
    if (card.index < 0 || card.x <= 0 || !card.is_operator) {
        return false;
    }
    const int x = card.x;
    before = ctrler()->get_image();
    if (before.empty() || before.cols * 9 != before.rows * 16) {
        return false;
    }
    cv::resize(before, before, cv::Size(1920, 1080), 0.0, 0.0, cv::INTER_LINEAR);
    OCRer gold_before_ocr(before, Rect { 1785, 768, 65, 48 }, inst());
    gold_before_ocr.set_use_raw(true);
    const auto gold_before = read_number(gold_before_ocr.analyze().value_or(OcrResults {}));

    if (!click_canvas(Point { x, 920 })) {
        return false;
    }
    sleep(800);
    cv::Mat detail_image = ctrler()->get_image();
    if (detail_image.empty()) {
        return false;
    }
    cv::resize(detail_image, detail_image, cv::Size(1920, 1080), 0.0, 0.0, cv::INTER_LINEAR);
    const auto card_error = recognize({ x - 120, 980, 240, 80 });
    const auto purchase_error = recognize({ 1400, 120, 520, 140 });
    if (contains(purchase_error, "资金不足") || contains(card_error, "无法购买")) {
        click_canvas(Point { 960, 600 });
        sleep(400);
        return false;
    }
    OCRer detail_ocr(detail_image, Rect { 0, 250, 700, 620 }, inst());
    detail_ocr.set_use_raw(true);
    const auto detail = detail_ocr.analyze().value_or(OcrResults {});
    if (!contains(detail, "特质") && !contains(detail, "天赋") && !contains(detail, "特性")) {
        click_canvas(Point { x, 920 });
        sleep(400);
        click_canvas(Point { 960, 600 });
        sleep(400);
        return false;
    }

    if (!click_canvas(Point { x, 1026 })) {
        return false;
    }
    sleep(1200);
    after = ctrler()->get_image();
    if (!after.empty()) {
        cv::resize(after, after, cv::Size(1920, 1080), 0.0, 0.0, cv::INTER_LINEAR);
    }
    std::optional<int> gold_after;
    if (!after.empty()) {
        OCRer gold_after_ocr(after, Rect { 1785, 768, 65, 48 }, inst());
        gold_after_ocr.set_use_raw(true);
        gold_after = read_number(gold_after_ocr.analyze().value_or(OcrResults {}));
    }
    if (card.cost != 0 && gold_before && gold_after && *gold_after >= *gold_before) {
        click_canvas(Point { 960, 600 });
        sleep(400);
        return false;
    }
    return true;
}

std::optional<asst::Point> asst::SPAFarmTaskPlugin::locate_new_bench_slot(
    const cv::Mat& before,
    const cv::Mat& after) const
{
    if (before.empty() || after.empty() || before.size() != after.size()) {
        return std::nullopt;
    }
    const cv::Rect bench_region { 300, 100, 1400, 460 };
    if (bench_region.x + bench_region.width > before.cols || bench_region.y + bench_region.height > before.rows) {
        return std::nullopt;
    }

    cv::Mat difference;
    cv::absdiff(before(bench_region), after(bench_region), difference);
    cv::cvtColor(difference, difference, cv::COLOR_BGR2GRAY);
    cv::threshold(difference, difference, 40, 255, cv::THRESH_BINARY);
    const auto kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(15, 15));
    cv::morphologyEx(difference, difference, cv::MORPH_CLOSE, kernel);

    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroids;
    const int component_count = cv::connectedComponentsWithStats(difference, labels, stats, centroids);
    if (component_count <= 1) {
        return std::nullopt;
    }

    int largest_component = 1;
    for (int component = 2; component < component_count; ++component) {
        if (stats.at<int>(component, cv::CC_STAT_AREA) > stats.at<int>(largest_component, cv::CC_STAT_AREA)) {
            largest_component = component;
        }
    }
    return Point {
        bench_region.x + static_cast<int>(centroids.at<double>(largest_component, 0)),
        bench_region.y + static_cast<int>(centroids.at<double>(largest_component, 1))
    };
}

bool asst::SPAFarmTaskPlugin::deploy_from_bench(const Point& bench, const std::string& name, int tier)
{
    static const std::array<Point, 2> safeTiles { Point { 1195, 445 }, Point { 1210, 548 } };
    static const std::array<Point, 3> pathTiles { Point { 1305, 345 }, Point { 1192, 345 }, Point { 1305, 445 } };
    static const std::array<Point, 3> restTiles { Point { 1305, 548 }, Point { 1318, 650 }, Point { 1440, 650 } };
    const auto is_used = [this](const Point& point) {
        return std::ranges::any_of(m_used_tiles, [&point](const Point& used) {
            return std::abs(used.x - point.x) < 50 && std::abs(used.y - point.y) < 50;
        });
    };
    cv::Mat image = ctrler()->get_image();
    if (image.empty()) {
        return false;
    }
    cv::resize(image, image, cv::Size(1920, 1080), 0.0, 0.0, cv::INTER_LINEAR);
    cv::Mat hsv;
    cv::Mat green;
    cv::cvtColor(image, hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, cv::Scalar(35, 100, 100), cv::Scalar(85, 255, 255), green);

    const auto available = [&green, &is_used](const auto& tiles) {
        std::vector<Point> result;
        for (const Point& point : tiles) {
            if (is_used(point)) {
                continue;
            }
            const cv::Rect roi(point.x - 18, point.y - 18, 36, 36);
            if (roi.x < 0 || roi.y < 0 || roi.x + roi.width > green.cols || roi.y + roi.height > green.rows) {
                continue;
            }
            if (cv::countNonZero(green(roi)) > 36 * 36 * 0.8) {
                result.emplace_back(point);
            }
        }
        return result;
    };

    const auto is_safe = [&name](const auto& names) {
        return std::ranges::find(names, name) != names.end();
    };
    const bool prefer_safe = m_low_profile ? is_safe(LowSafeNames) : is_safe(FullSafeNames);
    std::vector<Point> candidates;
    if (prefer_safe) {
        candidates = available(safeTiles);
    }
    if (candidates.empty()) {
        candidates = available(pathTiles);
    }
    if (candidates.empty()) {
        candidates = available(restTiles);
    }
    if (candidates.empty()) {
        return false;
    }

    const Point target = candidates.front();
    if (!swipe_canvas(bench, target, 850)) {
        return false;
    }
    sleep(900);
    const auto facing = recognize({ 300, 0, 1400, 820 });
    if (contains(facing, "点击取消")) {
        swipe_canvas(target, Point { target.x + 300, target.y }, 500);
        sleep(700);
    }
    m_used_tiles.emplace_back(target);
    LogInfo << "SPA deployed " << name << " tier " << tier << " at " << target;
    return true;
}

bool asst::SPAFarmTaskPlugin::contains(const OcrResults& results, std::string_view text) const
{
    return std::ranges::any_of(results, [text](const auto& result) {
        return contains_text(compact(result.text), text);
    });
}

std::optional<asst::OCRer::Result> asst::SPAFarmTaskPlugin::find_text(
    const OcrResults& results,
    std::string_view text) const
{
    const auto found = std::ranges::find_if(results, [text](const auto& result) {
        return contains_text(compact(result.text), text);
    });
    return found == results.end() ? std::nullopt : std::optional(*found);
}

std::optional<int> asst::SPAFarmTaskPlugin::read_passed_count(const OcrResults& results) const
{
    for (const auto& result : results) {
        const size_t passed_at = result.text.find("通过");
        if (passed_at == std::string::npos) {
            continue;
        }
        const std::string tail = normalize_digits(result.text.substr(passed_at + std::string_view("通过").size()));
        std::string digits;
        std::ranges::copy_if(tail, std::back_inserter(digits), [](unsigned char ch) { return std::isdigit(ch) != 0; });
        if (digits.empty()) {
            return 1;
        }
        int count = 0;
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), count);
        if (error == std::errc {} && end == digits.data() + digits.size()) {
            return count;
        }
        return std::nullopt;
    }
    return 0;
}

std::optional<std::string> asst::SPAFarmTaskPlugin::read_strategy_name(const OcrResults& results) const
{
    for (const auto& result : results) {
        const int x = result.rect.x + result.rect.width / 2;
        const int y = result.rect.y + result.rect.height / 2;
        if (y <= 520 || y >= 650 || !contains_cjk(result.text)) {
            continue;
        }
        const std::string text = compact(result.text);
        if (text.empty() || contains_text(text, "生命") || contains_text(text, "通过") || contains_text(text, "【")) {
            continue;
        }
        (void)x;
        return text;
    }
    return std::nullopt;
}

std::optional<std::string> asst::SPAFarmTaskPlugin::choose_strategy()
{
    click_canvas(Point { 1500, 580 });
    sleep(1000);
    std::unordered_set<std::string> tried;
    std::vector<int> rows(StrategyRows.begin(), StrategyRows.end());

    for (int page = 0; page < MaxStrategyPages && !need_exit(); ++page) {
        cv::Mat page_image = ctrler()->get_image();
        if (page_image.empty()) {
            continue;
        }
        cv::resize(page_image, page_image, cv::Size(1920, 1080), 0.0, 0.0, cv::INTER_LINEAR);
        for (const int y : rows) {
            if (y < 310 || y > 1010) {
                continue;
            }
            for (const int x : StrategyColumns) {
                const Point grid_point { x, y };
                if (has_color_badge(page_image, grid_point) || (m_target_clears <= 3 && has_badge(page_image, grid_point))) {
                    continue;
                }
                if (!click_canvas(grid_point)) {
                    continue;
                }
                sleep(700);

                OcrResults panel;
                std::optional<std::string> name;
                for (int attempt = 0; attempt < 3; ++attempt) {
                    panel = recognize({ 1440, 150, 480, 650 });
                    name = read_strategy_name(panel);
                    if (name) {
                        break;
                    }
                    sleep(600);
                }

                if (!name) {
                    LogInfo << "SPA strategy detail could not be read at " << grid_point;
                    continue;
                }
                if (!tried.emplace(*name).second) {
                    continue;
                }
                const int count = read_passed_count(panel).value_or(0);
                if (count >= m_target_clears) {
                    continue;
                }

                bool selected = false;
                for (int confirm = 0; confirm < 3 && !need_exit(); ++confirm) {
                    sleep(400);
                    const auto current = recognize({ 1440, 150, 480, 650 });
                    if (read_strategy_name(current) == name) {
                        selected = true;
                        break;
                    }
                    click_canvas(grid_point);
                }
                if (!selected) {
                    LogInfo << "SPA selected strategy name did not match: " << *name;
                    continue;
                }
                click_canvas(Point { 1680, 990 });
                sleep(1200);
                LogInfo << "SPA selected strategy " << *name << " with " << count << "/" << m_target_clears
                        << " clears";
                m_initial_clears = count;
                report("SPAFarmStrategySelected", json::object { { "name", *name }, { "clears", count } });
                return name;
            }
        }

        const cv::Mat before = page_image;
        swipe_canvas(Point { 1000, 900 }, Point { 1000, 480 }, 500);
        sleep(800);
        cv::Mat after = ctrler()->get_image();
        if (after.empty()) {
            break;
        }
        cv::resize(after, after, cv::Size(1920, 1080), 0.0, 0.0, cv::INTER_LINEAR);
        const auto shift = grid_shift(before, after);
        if (!shift || std::abs(*shift) < 20) {
            break;
        }
        rows = detect_strategy_rows(after);
        if (rows.empty()) {
            break;
        }
    }

    LogInfo << "SPA has no strategy below the requested clear count";
    return std::nullopt;
}

bool asst::SPAFarmTaskPlugin::choose_squad()
{
    const auto operators = recognize({ 560, 80, 1160, 880 });
    struct Candidate
    {
        Point center;
        std::string name;
    };
    std::vector<Candidate> names;
    std::vector<Point> banned;
    for (const auto& result : operators) {
        const Point center { result.rect.x + result.rect.width / 2, result.rect.y + result.rect.height / 2 };
        const std::string name = compact(result.text);
        if (contains_text(name, "禁用")) {
            banned.emplace_back(center);
            continue;
        }
        if (name.empty() || !contains_cjk(name) || contains_text(name, "盟约") || contains_text(name, "阵容") ||
            contains_text(name, "确定") || contains_text(name, "编队")) {
            continue;
        }
        names.push_back({ center, name });
    }

    const auto priority_rank = [this](std::string_view name) {
        if (m_low_profile) {
            for (size_t index = 0; index < LowPriorityKeys.size(); ++index) {
                if (match_priority_key(name, LowPriorityKeys[index].first)) {
                    return 1000 - static_cast<int>(index);
                }
            }
        }
        else {
            for (size_t index = 0; index < FullPriorityKeys.size(); ++index) {
                if (match_priority_key(name, FullPriorityKeys[index].first)) {
                    return 1000 - static_cast<int>(index);
                }
            }
        }
        return 2 + (is_dps_name(name) ? 5 : 0);
    };

    int selected = 0;
    for (const auto [row_top, row_bottom] : { std::pair { 380, 520 }, std::pair { 760, 900 } }) {
        std::vector<const Candidate*> row;
        for (const auto& candidate : names) {
            const Point center = candidate.center;
            const bool near_banned = std::ranges::any_of(banned, [&center](const Point& point) {
                return std::abs(center.x - point.x) < 90 && point.y - center.y > 0 && point.y - center.y < 80;
            });
            if (center.y >= row_top && center.y <= row_bottom && !near_banned) {
                row.emplace_back(&candidate);
            }
        }
        std::ranges::sort(row, [&priority_rank](const Candidate* left, const Candidate* right) {
            return priority_rank(left->name) > priority_rank(right->name);
        });
        for (size_t index = 0; index < std::min<size_t>(2, row.size()); ++index) {
            const Point center = row[index]->center;
            click_canvas(Point { center.x, center.y - 170 });
            sleep(400);
            ++selected;
        }
    }

    LogInfo << "SPA custom squad operators selected: " << selected;
    click_canvas(Point { 1686, 986 });
    sleep(1500);
    return true;
}

bool asst::SPAFarmTaskPlugin::handle_preparation(const OcrResults& results, int round)
{
    cv::Mat image = ctrler()->get_image();
    if (image.empty() || image.cols * 9 != image.rows * 16) {
        return false;
    }
    cv::resize(image, image, cv::Size(1920, 1080), 0.0, 0.0, cv::INTER_LINEAR);
    auto cards = scan_shop(image);
    const auto shop_panel = recognize({ 0, 820, 760, 240 });
    const bool maxed = contains(shop_panel, "满级");
    const auto buy_and_place = [this](const ShopCard& card) {
        cv::Mat before;
        cv::Mat after;
        if (!buy_shop_card(card, before, after)) {
            m_abort_reason = "purchase_unconfirmed";
            return false;
        }
        const auto bench = locate_new_bench_slot(before, after);
        if (!bench) {
            LogWarn << "SPA bought operator but could not locate its new bench slot: " << card.name;
            m_abort_reason = "bench_slot_unconfirmed";
            return false;
        }
        if (!deploy_from_bench(*bench, card.name, card.tier)) {
            LogWarn << "SPA bought operator but could not find an available deployment tile: " << card.name;
            m_abort_reason = "deployment_unconfirmed";
            return false;
        }
        return true;
    };

    if (round <= 1) {
        int bought = 0;
        int healers = 0;
        for (const auto& card : cards) {
            if (bought >= 2 || !card.is_operator || card.cost < 0) {
                continue;
            }
            if (card.is_healer && healers >= 1) {
                continue;
            }
            const auto current_gold = read_number(recognize({ 1760, 760, 120, 65 }));
            if (current_gold && card.cost > *current_gold) {
                continue;
            }
            if (buy_and_place(card)) {
                ++bought;
                healers += card.is_healer ? 1 : 0;
                sleep(500);
            }
            else if (!m_abort_reason.empty()) {
                report("SPAFarmStopped", json::object { { "reason", m_abort_reason }, { "operator", card.name } });
                return true;
            }
        }
    }

    bool shop_maxed = maxed;
    if (!shop_maxed) {
        for (int attempt = 0; attempt < 6 && !need_exit(); ++attempt) {
            const auto panel = recognize({ 0, 820, 760, 240 });
            if (contains(panel, "满级")) {
                shop_maxed = true;
                break;
            }
            const auto upgrade = find_text(panel, "升级");
            if (!upgrade) {
                break;
            }
            const int upgrade_x = upgrade->rect.x + upgrade->rect.width / 2;
            const auto current_gold = read_number(recognize({ 1760, 760, 120, 65 }));
            const auto upgrade_cost = read_number(recognize({ upgrade_x - 22, 780, 44, 42 }));
            if (current_gold && upgrade_cost && *current_gold < *upgrade_cost) {
                break;
            }
            click_canvas(Point { upgrade->rect.x + upgrade->rect.width / 2, 920 });
            sleep(600);
        }
        shop_maxed = contains(recognize({ 0, 820, 760, 240 }), "满级");
    }
    if (shop_maxed) {
        int unchanged_refreshes = 0;
        std::vector<std::string> previous_signature;
        for (int shop_action = 0; shop_action < 10 && !need_exit(); ++shop_action) {
            if (shop_action > 0) {
                const auto current_gold = read_number(recognize({ 1760, 760, 120, 65 }));
                if (current_gold && *current_gold < 4) {
                    break;
                }
                click_canvas(Point { 1786, 686 });
                sleep(900);
                image = ctrler()->get_image();
                if (image.empty()) {
                    break;
                }
                cv::resize(image, image, cv::Size(1920, 1080), 0.0, 0.0, cv::INTER_LINEAR);
            }
            cards = scan_shop(image);
            std::vector<std::string> signature;
            for (const auto& card : cards) {
                signature.emplace_back(card.name + ":" + std::to_string(card.tier) + ":" + std::to_string(card.cost));
            }
            if (shop_action > 0 && signature == previous_signature) {
                if (++unchanged_refreshes >= 2) {
                    LogWarn << "SPA shop refresh did not change the visible cards; stopping refreshes";
                    break;
                }
            }
            else {
                unchanged_refreshes = 0;
            }
            previous_signature = std::move(signature);
            std::ranges::sort(cards, [this](const ShopCard& left, const ShopCard& right) {
                const auto score = [this](const ShopCard& card) {
                    const auto& names = m_low_profile ? std::span<const std::string_view>(LowPriority)
                                                      : std::span<const std::string_view>(FullPriority);
                    const auto it = std::ranges::find(names, card.name);
                    return it == names.end() ? static_cast<int>(names.size()) + (card.is_dps ? 1 : 2)
                                             : static_cast<int>(std::distance(names.begin(), it));
                };
                return score(left) < score(right);
            });
            bool bought = false;
            bool had_affordable_candidate = false;
            const auto current_gold = read_number(recognize({ 1760, 760, 120, 65 }));
            for (const auto& card : cards) {
                if (!card.is_operator || card.tier < 5 || card.cost < 0 || (current_gold && card.cost > *current_gold)) {
                    continue;
                }
                had_affordable_candidate = true;
                if (buy_and_place(card)) {
                    bought = true;
                    break;
                }
                report("SPAFarmStopped", json::object { { "reason", m_abort_reason }, { "operator", card.name } });
                return true;
            }
            if (bought) {
                previous_signature.clear();
                continue;
            }
            if (shop_action == 9) {
                break;
            }
            if (had_affordable_candidate) {
                break;
            }
        }
    }

    const auto ready = find_text(recognize(), "准备就绪");
    if (!ready) {
        return false;
    }
    click_canvas(Point { 1780, 170 });
    sleep(1200);
    return true;
}

bool asst::SPAFarmTaskPlugin::handle_screen(
    const OcrResults& results,
    std::string& current_strategy,
    int& clears)
{
    if (contains(results, "断线重连")) {
        const auto reconnect = find_text(results, "断线重连");
        if (reconnect) {
            click_canvas(reconnect->rect);
        }
        else {
            click_canvas(Point { 1720, 1010 });
        }
        sleep(5000);
        return true;
    }

    if (contains(results, "选择策略")) {
        auto strategy = choose_strategy();
        if (!strategy) {
            m_completed = true;
            report("SPAFarmCompleted", json::object { { "target", m_target_clears } });
            return true;
        }
        current_strategy = std::move(*strategy);
        m_used_tiles.clear();
        clears = m_initial_clears;
        return true;
    }

    if (contains(results, "确定自选编队")) {
        return choose_squad();
    }

    if (contains(results, "模拟完成")) {
        ++clears;
        report("SPAFarmRunCompleted", json::object {
            { "strategy", current_strategy }, { "clears", clears }, { "target", m_target_clears }
        });
        if (auto home = find_text(results, "返回主页")) {
            click_canvas(home->rect);
        } else {
            click_canvas(Point { 1100, 650 });
        }
        sleep(3000);
        m_used_tiles.clear();
        if (clears >= m_target_clears) {
            m_done_strategies.emplace_back(current_strategy);
            current_strategy.clear();
        }
        return true;
    }

    if (contains(results, "返回主页")) {
        if (auto home = find_text(results, "返回主页")) {
            click_canvas(home->rect);
            sleep(2500);
        }
        return true;
    }

    if (contains(results, "极速模拟") && !contains(results, "选择策略")) {
        if (auto start = find_text(results, "极速模拟")) {
            click_canvas(start->rect);
        } else {
            click_canvas(Point { 1280, 1000 });
        }
        sleep(1500);
        return true;
    }

    if (contains(results, "回合") && (contains(results, "准备就绪") || contains(results, "升级") ||
                                      contains(results, "刷新") || contains(results, "满级"))) {
        int round = 1;
        for (const auto& result : results) {
            if (result.text.find("回合") == std::string::npos) {
                continue;
            }
            std::smatch match;
            if (std::regex_search(result.text, match, std::regex(R"(([0-9]+)\s*回合)"))) {
                round = std::stoi(match[1].str());
            }
        }
        return handle_preparation(results, round);
    }

    if (contains(results, "请选择模拟难度")) {
        if (auto quickly = find_text(results, "极速模拟")) {
            click_canvas(quickly->rect);
        } else {
            click_canvas(Point { 1050, 630 });
        }
        sleep(1200);
        return true;
    }

    if (contains(results, "确认本局信息")) {
        click_canvas(Point { 1686, 986 });
        sleep(1000);
        return true;
    }
    return false;
}

bool asst::SPAFarmTaskPlugin::_run()
{
    LogTraceFunction;
    const auto start = std::chrono::steady_clock::now();
    auto last_progress = start;
    auto last_garrison = start;
    std::string current_strategy;
    int clears = 0;
    int idle = 0;

    for (int action = 0; action < MaxActions && !need_exit(); ++action) {
        const auto results = recognize();
        if (results.empty()) {
            sleep(1000);
            continue;
        }

        if (contains(results, "极速模拟") || contains(results, "选择策略") || contains(results, "初始生命值") ||
            contains(results, "确定自选编队") || contains(results, "回合") || contains(results, "卫戍")) {
            last_garrison = std::chrono::steady_clock::now();
        }
        if (std::chrono::steady_clock::now() - last_garrison > LeavePageLimit) {
            LogError << "SPA stopped because the Garrison Protocol page was not detected for 30 seconds";
            save_img(utils::path("debug") / utils::path("spa_farm"), false);
            report("SPAFarmStopped", json::object { { "reason", "left_garrison" } });
            return false;
        }
        if (std::chrono::steady_clock::now() - last_progress > NoProgressLimit) {
            LogError << "SPA stopped after ten minutes without progress";
            save_img(utils::path("debug") / utils::path("spa_farm"), false);
            report("SPAFarmStopped", json::object { { "reason", "stalled" } });
            return false;
        }

        if (handle_screen(results, current_strategy, clears)) {
            if (!m_abort_reason.empty()) {
                return false;
            }
            if (m_completed) {
                return true;
            }
            last_progress = std::chrono::steady_clock::now();
            idle = 0;
        } else {
            ++idle;
            if (idle == 5) {
                save_img(utils::path("debug") / utils::path("spa_farm"), false);
            }
            sleep(1200);
        }
    }

    if (need_exit()) {
        return false;
    }
    if (m_completed) {
        return true;
    }
    LogError << "SPA farm action limit reached";
    save_img(utils::path("debug") / utils::path("spa_farm"), false);
    report("SPAFarmStopped", json::object { { "reason", "action_limit" } });
    return false;
}

void asst::SPAFarmTaskPlugin::report(std::string what, json::value details)
{
    auto info = basic_info_with_what(std::move(what));
    info["details"] = std::move(details);
    callback(AsstMsg::SubTaskExtraInfo, info);
}
