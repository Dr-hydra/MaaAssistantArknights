#include "SPAFarmTaskPlugin.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "Config/Miscellaneous/OcrPack.h"
#include "Controller/Controller.h"
#include "Controller/ControllerAPI.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Utils/Logger.hpp"
#include "Utils/WorkingDir.hpp"

namespace asst
{
namespace
{
// ============================================================================
// 设计坐标与常量（原实现按 1920x1080 编写）
// ============================================================================

constexpr int DesignWidth = 1920;
constexpr int DesignHeight = 1080;
constexpr double LogicScale = 2.0 / 3.0; // 1920 -> MAA 逻辑画布 1280

constexpr std::array<int, 4> GridX { 680, 890, 1100, 1310 };
constexpr std::array<int, 4> GridY { 370, 580, 790, 1000 };
constexpr int GridTop = 250;
constexpr int GridBottom = 1050;

constexpr std::array<int, 6> CardX { 304, 550, 790, 1034, 1274, 1520 };
constexpr int CardY = 920;
constexpr int GoldBoxX1 = 1785;
constexpr int GoldBoxY1 = 768;
constexpr int GoldBoxX2 = 1850;
constexpr int GoldBoxY2 = 816;
constexpr std::pair<int, int> SellPoint { 1760, 900 };
constexpr std::pair<int, int> RefreshPoint { 1786, 686 };
constexpr std::pair<int, int> ReadyPoint { 1780, 170 };

constexpr int LeaveTimeoutSec = 30;
constexpr int StallTimeoutSec = 600;

// Win32 注入是异步投递、没有内置节拍：按下后立刻移动、整段 move 在毫秒级发完时，
// 游戏按帧采样不到中间轨迹，会把「按住拖拽」判成一次点击。原实现走 adb
// `input motionevent`，每条命令本身就有几十到上百毫秒开销，天然满足节拍。
// 这里显式补回等价时序，判定逻辑不变。
constexpr int DragDownHoldMs = 300;
constexpr int DragMoveStepMs = 50;

constexpr int TouchDown = static_cast<int>(InputEvent::Type::TOUCH_DOWN);
constexpr int TouchUp = static_cast<int>(InputEvent::Type::TOUCH_UP);
constexpr int TouchMove = static_cast<int>(InputEvent::Type::TOUCH_MOVE);

using Point2 = std::pair<int, int>;

const std::array<const char*, 13> GarrisonKeywords {
    "极速模拟", "卫戍", "STRONGHOLD", "确认准备", "资金将在", "选择策略", "确认本局信息",
    "确定自选编队", "返回主页", "模拟完成", "模拟中断", "COUNTDOWN", "断线重连"
};

const std::array<const char*, 8> GenericKeywords { "点击任意", "继续", "返回", "确认", "确定", "领取", "下一步", "完成" };

// ============================================================================
// 小工具
// ============================================================================

Point to_logic_point(int x, int y)
{
    return Point { static_cast<int>(std::lround(x * LogicScale)), static_cast<int>(std::lround(y * LogicScale)) };
}

double now_sec()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

cv::Rect clamp_rect(const cv::Mat& image, int x1, int y1, int x2, int y2)
{
    const int left = std::clamp(std::min(x1, x2), 0, image.cols);
    const int top = std::clamp(std::min(y1, y2), 0, image.rows);
    const int right = std::clamp(std::max(x1, x2), 0, image.cols);
    const int bottom = std::clamp(std::max(y1, y2), 0, image.rows);
    return { left, top, right - left, bottom - top };
}

std::vector<char32_t> decode_utf8(const std::string& text)
{
    std::vector<char32_t> out;
    out.reserve(text.size());
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        const auto c = static_cast<uint8_t>(text[i]);
        if (c < 0x80) {
            out.push_back(c);
            i += 1;
        }
        else if ((c >> 5) == 0x6 && i + 1 < n) {
            out.push_back(((c & 0x1F) << 6) | (static_cast<uint8_t>(text[i + 1]) & 0x3F));
            i += 2;
        }
        else if ((c >> 4) == 0xE && i + 2 < n) {
            out.push_back(
                ((c & 0x0F) << 12) | ((static_cast<uint8_t>(text[i + 1]) & 0x3F) << 6) |
                (static_cast<uint8_t>(text[i + 2]) & 0x3F));
            i += 3;
        }
        else if ((c >> 3) == 0x1E && i + 3 < n) {
            out.push_back(
                ((c & 0x07) << 18) | ((static_cast<uint8_t>(text[i + 1]) & 0x3F) << 12) |
                ((static_cast<uint8_t>(text[i + 2]) & 0x3F) << 6) | (static_cast<uint8_t>(text[i + 3]) & 0x3F));
            i += 4;
        }
        else {
            i += 1;
        }
    }
    return out;
}

bool has_cjk(const std::string& text)
{
    for (const char32_t cp : decode_utf8(text)) {
        if (cp >= 0x4E00 && cp <= 0x9FFF) {
            return true;
        }
    }
    return false;
}

bool is_alpha_codepoint(char32_t cp)
{
    return (cp >= 0x0041 && cp <= 0x005A) || (cp >= 0x0061 && cp <= 0x007A) || (cp >= 0x00C0 && cp <= 0x02AF) ||
           (cp >= 0x0370 && cp <= 0x052F) || (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF);
}

bool has_cjk_or_alpha(const std::string& text)
{
    for (const char32_t cp : decode_utf8(text)) {
        if (is_alpha_codepoint(cp)) {
            return true;
        }
    }
    return false;
}

// 对应 Python str.isdigit() + unicodedata.digit()：覆盖 ASCII、全角、带圈数字等常见形态。
std::optional<int> digit_value(char32_t cp)
{
    if (cp >= 0x0030 && cp <= 0x0039) {
        return static_cast<int>(cp - 0x0030);
    }
    if (cp >= 0xFF10 && cp <= 0xFF19) {
        return static_cast<int>(cp - 0xFF10);
    }
    if (cp >= 0x0660 && cp <= 0x0669) {
        return static_cast<int>(cp - 0x0660);
    }
    if (cp >= 0x06F0 && cp <= 0x06F9) {
        return static_cast<int>(cp - 0x06F0);
    }
    if (cp >= 0x2460 && cp <= 0x2473) { // ①-⑳
        return static_cast<int>(cp - 0x2460) + 1;
    }
    if (cp >= 0x2776 && cp <= 0x277F) { // ❶-❿
        return static_cast<int>(cp - 0x2776) + 1;
    }
    if (cp >= 0x2780 && cp <= 0x2789) { // ➀-➉
        return static_cast<int>(cp - 0x2780) + 1;
    }
    if (cp >= 0x278A && cp <= 0x2793) { // ➊-➓
        return static_cast<int>(cp - 0x278A) + 1;
    }
    if (cp >= 0x2074 && cp <= 0x2079) { // ⁴-⁹
        return static_cast<int>(cp - 0x2074) + 4;
    }
    if (cp == 0x2070) {
        return 0;
    }
    if (cp >= 0x2080 && cp <= 0x2089) { // ₀-₉
        return static_cast<int>(cp - 0x2080);
    }
    return std::nullopt;
}

std::string extract_digits(const std::string& text)
{
    std::string digits;
    for (const char32_t cp : decode_utf8(text)) {
        if (const auto value = digit_value(cp)) {
            digits.push_back(static_cast<char>('0' + *value));
        }
    }
    return digits;
}

bool is_ascii_digits(const std::string& text)
{
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

std::string strip(const std::string& text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && static_cast<unsigned char>(text[begin]) <= 0x20) {
        ++begin;
    }
    while (end > begin && static_cast<unsigned char>(text[end - 1]) <= 0x20) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string upper_ascii(std::string text)
{
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return text;
}

std::string replace_all(std::string text, const std::string& from, const std::string& to)
{
    if (from.empty()) {
        return text;
    }
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

std::string join(const std::vector<std::string>& items, const std::string& separator)
{
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out += separator;
        }
        out += items[i];
    }
    return out;
}

size_t char_count(const std::string& text)
{
    return decode_utf8(text).size();
}

// 1:1 复刻 difflib.SequenceMatcher(None, a, b).ratio()
double difflib_ratio(const std::string& lhs, const std::string& rhs)
{
    const std::vector<char32_t> a = decode_utf8(lhs);
    const std::vector<char32_t> b = decode_utf8(rhs);
    if (a.empty() && b.empty()) {
        return 1.0;
    }
    if (a.empty() || b.empty()) {
        return 0.0;
    }

    const auto find_longest_match = [&](int alo, int ahi, int blo, int bhi) {
        int best_i = alo;
        int best_j = blo;
        int best_size = 0;
        std::map<int, int> j2len;
        for (int i = alo; i < ahi; ++i) {
            std::map<int, int> new_j2len;
            for (int j = blo; j < bhi; ++j) {
                if (a[i] != b[j]) {
                    continue;
                }
                const int k = (j - 1 >= 0 && j2len.contains(j - 1) ? j2len[j - 1] : 0) + 1;
                new_j2len[j] = k;
                if (k > best_size) {
                    best_i = i - k + 1;
                    best_j = j - k + 1;
                    best_size = k;
                }
            }
            j2len.swap(new_j2len);
        }
        return std::tuple<int, int, int> { best_i, best_j, best_size };
    };

    std::vector<std::tuple<int, int, int, int>> queue { { 0, static_cast<int>(a.size()), 0,
                                                          static_cast<int>(b.size()) } };
    int matches = 0;
    while (!queue.empty()) {
        const auto [alo, ahi, blo, bhi] = queue.back();
        queue.pop_back();
        const auto [i, j, k] = find_longest_match(alo, ahi, blo, bhi);
        if (k > 0) {
            matches += k;
            if (alo < i && blo < j) {
                queue.emplace_back(alo, i, blo, j);
            }
            if (i + k < ahi && j + k < bhi) {
                queue.emplace_back(i + k, ahi, j + k, bhi);
            }
        }
    }
    return (2.0 * matches) / static_cast<double>(a.size() + b.size());
}

// ============================================================================
// 干员数据（与原实现完全一致）
// ============================================================================

const std::vector<std::pair<std::string, std::string>>& priority_table(bool low_profile)
{
    static const std::vector<std::pair<std::string, std::string>> full {
        { "塔露拉", "塔露拉" },       { "维什戴尔", "维什戴尔" },     { "星熊", "斩业星熊" },
        { "安洁莉娜", "予愿安洁莉娜" }, { "能天使", "新约能天使" },     { "凯尔希", "凯尔希·思衡托" },
        { "望", "望" },               { "初雪", "圣聆初雪" },         { "逻各斯", "逻各斯" },
        { "蕾缪安", "蕾缪安" },       { "贝洛内", "贝洛内" },         { "冻河", "冻河" },
        { "明霄陈", "赤刃明霄陈" },   { "结城理", "结城理" },
    };
    static const std::vector<std::pair<std::string, std::string>> low {
        { "塔露拉", "塔露拉" },         { "史尔特尔", "史尔特尔" },     { "耀骑士", "耀骑士临光" },
        { "逻各斯", "逻各斯" },         { "克莱门莎", "克莱门莎" },     { "锏", "锏" },
        { "帕拉斯", "帕拉斯" },         { "真言", "真言" },             { "蕾缪安", "蕾缪安" },
        { "烛煌", "烛煌" },             { "艾丽妮", "艾丽妮" },         { "薇薇安娜", "薇薇安娜" },
        { "维娜", "维娜·维多利亚" },   { "乌尔比安", "乌尔比安" },     { "棘刺", "棘刺" },
        { "凛冬", "怒潮凛冬" },         { "初雪", "圣聆初雪" },         { "迷迭香", "迷迭香" },
        { "鸿雪", "鸿雪" },             { "贝洛内", "贝洛内" },         { "佩佩", "佩佩" },
        { "隐德来希", "隐德来希" },     { "冻河", "冻河" },             { "酒神", "酒神" },
    };
    return low_profile ? low : full;
}

const std::vector<std::string>& safe_table(bool low_profile)
{
    static const std::vector<std::string> full { "维什戴尔", "望", "能天使", "初雪", "蕾缪安", "逻各斯", "冻河" };
    static const std::vector<std::string> low { "逻各斯", "真言", "蕾缪安", "烛煌", "初雪",
                                                "迷迭香", "鸿雪", "冻河", "酒神" };
    return low_profile ? low : full;
}

const std::unordered_set<std::string>& dps_table()
{
    static const std::unordered_set<std::string> dps {
        "怒潮凛冬", "艾丽妮",   "棘刺",       "佩佩",       "圣聆初雪", "乌尔比安",   "薇薇安娜", "贝洛内",
        "维娜·维多利亚", "维娜", "迷迭香",     "鸿雪",       "阿米娅",   "赤刃明霄陈", "司霆惊蛰", "维什戴尔",
        "斩业星熊", "丰川祥子", "结城理",     "凛御银灰",   "歌蕾蒂娅", "Sharp",      "领主·Sharp", "Pith",
        "Stormeye", "预备干员-近卫", "预备干员-术师", "引星棘刺", "克莱门莎", "耀骑士临光", "荒芜拉普兰德", "逻各斯",
        "真言",     "史尔特尔", "锏",         "塔露拉",     "夕",       "蕾缪安",     "帕拉斯",   "望",
        "塑心",
    };
    return dps;
}

const std::vector<std::string>& healer_table()
{
    static const std::vector<std::string> healers { "褐果", "莎草",   "哈洛德", "瑰盐",   "乌啾", "风絮",
                                                    "流明", "夜莺",   "华法琳", "纯烬艾雅法拉", "Mon3tr", "Touch" };
    return healers;
}

// 拖拽视角下的格子中心（原程序包 tiles.json）
struct LayoutConfig
{
    std::vector<Point2> safe;
    std::vector<Point2> path;
    std::vector<Point2> rest;
    Point2 lift { 760, 650 };
};

const LayoutConfig& layout()
{
    static const LayoutConfig config {
        { { 1195, 445 }, { 1210, 548 } },
        { { 1305, 345 }, { 1192, 345 }, { 1305, 445 } },
        { { 1305, 548 }, { 1318, 650 }, { 1440, 650 } },
        { 760, 650 },
    };
    return config;
}

bool layout_ready()
{
    return !layout().safe.empty() && !layout().path.empty();
}

std::filesystem::path done_file_path()
{
    if (UserDir.get().empty()) {
        return {};
    }
    return UserDir.get() / "config" / "SPAFarmStrategies.txt";
}
} // namespace

// ============================================================================
// 插件入口
// ============================================================================

bool SPAFarmTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }
    const std::string& task = details.get("details", "task", "");
    return task.ends_with("MiniGame@SPA@BadgeFarm");
}

void SPAFarmTaskPlugin::apply_profile()
{
    m_priority = priority_table(m_low_profile);
    m_safe_keys = safe_table(m_low_profile);
}

bool SPAFarmTaskPlugin::is_healer(const std::string& name)
{
    if (name.empty()) {
        return false;
    }
    const size_t length = char_count(name);
    for (const auto& healer : healer_table()) {
        if (name.find(healer) != std::string::npos || (length >= 2 && healer.find(name) != std::string::npos)) {
            return true;
        }
    }
    return false;
}

bool SPAFarmTaskPlugin::is_dps(const std::string& name)
{
    if (name.empty()) {
        return false;
    }
    for (const auto& dps : dps_table()) {
        if (name.find(dps) != std::string::npos || dps.find(name) != std::string::npos || difflib_ratio(dps, name) >= 0.6) {
            return true;
        }
    }
    return false;
}

namespace
{
// 对应原实现 match_key：单字关键词只接受整名相等，或短名包含
bool match_key(const std::string& name, const std::string& key)
{
    if (name.empty()) {
        return false;
    }
    if (char_count(key) == 1) {
        const std::string stripped = strip(name);
        return stripped == key || (char_count(stripped) <= 2 && name.find(key) != std::string::npos);
    }
    return name.find(key) != std::string::npos;
}
} // namespace

std::optional<int> SPAFarmTaskPlugin::priority_index(const std::string& name) const
{
    for (size_t i = 0; i < m_priority.size(); ++i) {
        if (match_key(name, m_priority[i].first)) {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

bool SPAFarmTaskPlugin::is_safe_operator(const std::string& name) const
{
    for (const auto& key : m_safe_keys) {
        if (match_key(name, key)) {
            return true;
        }
    }
    return false;
}

int SPAFarmTaskPlugin::rank_operator(const std::string& name, int tier) const
{
    if (const auto index = priority_index(name)) {
        return 1000 - *index;
    }
    if (tier >= 5) {
        return 500 + (is_dps(name) ? 50 : 0);
    }
    return tier;
}

// ============================================================================
// 画布与输入输出（MAA 接口）
// ============================================================================

cv::Mat SPAFarmTaskPlugin::capture() const
{
    for (int i = 0; i < 3; ++i) {
        // 取原始分辨率截图：MAA 逻辑画布固定 1280x720，直接用它会放大 1.5 倍，
        // 小字号（金币、卡牌费用）会被插值糊掉，导致同一帧两次识别结果不一致。
        cv::Mat image = ctrler()->get_image(true);
        if (image.empty()) {
            if (!sleep(300)) {
                return {};
            }
            continue;
        }
        if (image.cols == DesignWidth && image.rows == DesignHeight) {
            return image;
        }
        cv::Mat resized;
        cv::resize(
            image,
            resized,
            cv::Size(DesignWidth, DesignHeight),
            0,
            0,
            image.cols > DesignWidth ? cv::INTER_AREA : cv::INTER_LINEAR);
        return resized;
    }
    return {};
}

void SPAFarmTaskPlugin::touch_event(int type, int x, int y) const
{
    InputEvent event;
    event.type = static_cast<InputEvent::Type>(type);
    event.pointerId = 0;
    event.point = to_logic_point(x, y);
    if (!ctrler()->inject_input_event(event)) {
        LogWarn << "SPA | inject input event failed:" << type << x << y;
        return;
    }
    InputEvent commit;
    commit.type = InputEvent::Type::COMMIT;
    ctrler()->inject_input_event(commit);

    if (type == TouchDown) {
        sleep(DragDownHoldMs);
    }
}

void SPAFarmTaskPlugin::click_at(int x, int y, double wait_sec) const
{
    ctrler()->click(to_logic_point(x, y));
    sleep(static_cast<unsigned>(std::lround(wait_sec * 1000)));
}

void SPAFarmTaskPlugin::swipe_to(int x1, int y1, int x2, int y2, int duration_ms, double wait_sec) const
{
    ctrler()->swipe(to_logic_point(x1, y1), to_logic_point(x2, y2), duration_ms);
    sleep(static_cast<unsigned>(std::lround(wait_sec * 1000)));
}

void SPAFarmTaskPlugin::drag_path(int x1, int y1, int x2, int y2, int steps) const
{
    const auto start = std::chrono::steady_clock::now();
    for (int k = 1; k <= steps; ++k) {
        const double ratio = static_cast<double>(k) / steps;
        const int cx = static_cast<int>(x1 + (x2 - x1) * ratio);
        const int cy = static_cast<int>(y1 + (y2 - y1) * ratio);
        touch_event(TouchMove, cx, cy);
        std::this_thread::sleep_until(start + std::chrono::milliseconds(DragMoveStepMs * k));
    }
}

void SPAFarmTaskPlugin::scroll_grid() const
{
    constexpr int x = 1000;
    touch_event(TouchDown, x, 900);
    drag_path(x, 900, x, 480, 14);
    sleep(500);
    touch_event(TouchUp, x, 480);
    sleep(800);
}

void SPAFarmTaskPlugin::save_snapshot(const cv::Mat& image, const std::string& tag) const
{
    if (image.empty() || UserDir.get().empty()) {
        return;
    }
    std::error_code ec;
    const auto dir = UserDir.get() / "debug" / "spa_farm";
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return;
    }

    const std::time_t now = std::time(nullptr);
    std::tm tm_buf {};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char file_name[96] = {};
    std::snprintf(
        file_name,
        sizeof(file_name),
        "%02d%02d%02d_%s.png",
        tm_buf.tm_hour,
        tm_buf.tm_min,
        tm_buf.tm_sec,
        tag.c_str());

    std::vector<uint8_t> buffer;
    if (!cv::imencode(".png", image, buffer)) {
        return;
    }
    std::ofstream ofs(dir / file_name, std::ios::binary);
    if (!ofs) {
        return;
    }
    ofs.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
}

// ============================================================================
// OCR
// ============================================================================

std::string SPAFarmTaskPlugin::join_text(const std::vector<OcrItem>& items)
{
    std::string text;
    for (const auto& item : items) {
        text += item.text;
    }
    return text;
}

std::vector<SPAFarmTaskPlugin::OcrItem> SPAFarmTaskPlugin::ocr(const cv::Mat& image, std::optional<Box> box) const
{
    std::vector<OcrItem> items;
    if (image.empty()) {
        return items;
    }

    cv::Mat target = image;
    double offset_x = 0;
    double offset_y = 0;
    if (box.has_value()) {
        // 原实现给裁剪图四周补 40px 边缘再识别，缓解贴边文字漏检
        constexpr int Padding = 40;
        const cv::Rect roi = clamp_rect(image, box->x1, box->y1, box->x2, box->y2);
        if (roi.width <= 0 || roi.height <= 0) {
            return items;
        }
        cv::copyMakeBorder(image(roi), target, Padding, Padding, Padding, Padding, cv::BORDER_REPLICATE);
        offset_x = roi.x - Padding;
        offset_y = roi.y - Padding;
    }

    const auto results = WordOcr::get_instance().recognize(target, false, std::nullopt);
    items.reserve(results.size());
    for (const auto& result : results) {
        items.push_back(OcrItem {
            offset_x + result.rect.x + result.rect.width / 2.0,
            offset_y + result.rect.y + result.rect.height / 2.0,
            result.text,
        });
    }
    return items;
}

std::optional<SPAFarmTaskPlugin::OcrItem> SPAFarmTaskPlugin::find(
    const std::vector<OcrItem>& items,
    const std::vector<std::string>& keywords) const
{
    for (const auto& item : items) {
        for (const auto& keyword : keywords) {
            if (item.text.find(keyword) != std::string::npos) {
                return item;
            }
        }
    }
    return std::nullopt;
}

std::optional<int> SPAFarmTaskPlugin::read_number(const cv::Mat& image, Box box) const
{
    std::string best;
    for (const auto& item : ocr(image, box)) {
        const std::string digits = extract_digits(item.text);
        if (digits.size() > best.size()) {
            best = digits;
        }
    }
    if (!best.empty()) {
        return std::atoi(best.c_str());
    }
    return std::nullopt;
}

std::optional<int> SPAFarmTaskPlugin::read_number_rec(const cv::Mat& image, Box box) const
{
    const cv::Rect roi = clamp_rect(image, box.x1, box.y1, box.x2, box.y2);
    if (roi.width <= 0 || roi.height <= 0) {
        return std::nullopt;
    }

    std::string text;
    double score = 0.0;
    const auto results = WordOcr::get_instance().recognize(image(roi), true, std::nullopt);
    if (!results.empty()) {
        text = results.front().text;
        score = results.front().score;
    }
    const std::string digits = extract_digits(text);
    if (!digits.empty() && score >= 0.4) {
        return std::atoi(digits.c_str());
    }
    return read_number(image, box);
}

int SPAFarmTaskPlugin::passed_count(const std::vector<OcrItem>& panel) const
{
    const std::string key = "通过";
    for (const auto& item : panel) {
        const size_t pos = item.text.rfind(key);
        if (pos == std::string::npos) {
            continue;
        }
        const std::string tail = item.text.substr(pos + key.size());
        const std::string digits = extract_digits(tail);
        return digits.empty() ? 1 : std::atoi(digits.c_str());
    }
    return 0;
}

std::string SPAFarmTaskPlugin::strategy_name(const std::vector<OcrItem>& panel) const
{
    for (const auto& item : panel) {
        if (item.y > 520.0 && item.y < 650.0 && has_cjk(item.text) && item.text.find("生命") == std::string::npos &&
            item.text.find("通过") == std::string::npos && item.text.find("【") == std::string::npos) {
            return item.text;
        }
    }
    return {};
}

// ============================================================================
// 策略网格 / 角标
// ============================================================================

namespace
{
double yellow_ratio(const cv::Mat& image, int x1, int y1, int x2, int y2)
{
    const cv::Rect roi = clamp_rect(image, x1, y1, x2, y2);
    if (roi.width <= 0 || roi.height <= 0) {
        return 0.0;
    }
    cv::Mat hsv;
    cv::cvtColor(image(roi), hsv, cv::COLOR_BGR2HSV);
    cv::Mat mask;
    cv::inRange(hsv, cv::Scalar(8, 120, 150), cv::Scalar(35, 255, 255), mask);
    return cv::mean(mask)[0] / 255.0;
}

bool has_color_badge(const cv::Mat& image, int gx, int gy)
{
    const int x1 = gx + 40;
    const int y1 = std::max(0, gy - 110);
    const int x2 = gx + 95;
    const int y2 = gy - 45;
    const cv::Rect roi = clamp_rect(image, x1, y1, x2, y2);
    if (roi.width <= 0 || roi.height <= 0) {
        return false;
    }
    cv::Mat hsv;
    cv::cvtColor(image(roi), hsv, cv::COLOR_BGR2HSV);
    cv::Mat mask;
    cv::inRange(hsv, cv::Scalar(70, 40, 150), cv::Scalar(140, 255, 255), mask);
    const double cyan = cv::mean(mask)[0] / 255.0;
    return cyan > 0.15 && yellow_ratio(image, x1, y1, x2, y2) < 0.05;
}

bool has_badge(const cv::Mat& image, int gx, int gy)
{
    return yellow_ratio(image, gx + 40, gy - 100, gx + 90, gy - 50) > 0.12 ||
           yellow_ratio(image, gx + 30, std::max(0, gy - 130), gx + 100, gy - 30) > 0.1;
}
} // namespace

std::optional<int> SPAFarmTaskPlugin::grid_shift(const cv::Mat& before, const cv::Mat& after) const
{
    if (before.empty() || after.empty()) {
        return std::nullopt;
    }
    const cv::Rect strip_roi = clamp_rect(before, 600, 560, 1400, 700);
    const cv::Rect area_roi = clamp_rect(after, 600, GridTop, 1400, GridBottom);
    if (strip_roi.width <= 0 || strip_roi.height <= 0 || area_roi.width <= 0 || area_roi.height <= 0) {
        return std::nullopt;
    }

    cv::Mat result;
    cv::matchTemplate(after(area_roi), before(strip_roi), result, cv::TM_CCOEFF_NORMED);
    double max_value = 0.0;
    cv::Point location;
    cv::minMaxLoc(result, nullptr, &max_value, nullptr, &location);
    if (max_value < 0.6) {
        return std::nullopt;
    }
    return 560 - (GridTop + location.y);
}

std::vector<int> SPAFarmTaskPlugin::detect_rows(const cv::Mat& image) const
{
    std::vector<int> rows;
    if (image.empty()) {
        return rows;
    }
    const cv::Rect band_roi = clamp_rect(image, 620, 0, 740, image.rows);
    if (band_roi.width <= 0 || band_roi.height <= 0) {
        return rows;
    }

    cv::Mat gray;
    cv::cvtColor(image(band_roi), gray, cv::COLOR_BGR2GRAY);
    cv::Mat band;
    gray.convertTo(band, CV_32F);

    std::vector<bool> on(static_cast<size_t>(band.rows), false);
    for (int y = 0; y < band.rows; ++y) {
        double sum = 0.0;
        for (int x = 0; x < band.cols; ++x) {
            sum += band.at<float>(y, x);
        }
        const double mean = sum / band.cols;
        double variance = 0.0;
        for (int x = 0; x < band.cols; ++x) {
            const double diff = band.at<float>(y, x) - mean;
            variance += diff * diff;
        }
        on[static_cast<size_t>(y)] = std::sqrt(variance / band.cols) > 18.0;
    }

    std::optional<int> start;
    for (int y = GridTop - 60; y < GridBottom + 20; ++y) {
        if (y < 0 || y >= band.rows) {
            continue;
        }
        const bool active = on[static_cast<size_t>(y)];
        if (active && !start.has_value()) {
            start = y;
        }
        if ((!active || y == GridBottom + 19) && start.has_value()) {
            if (y - *start >= 60) {
                const int center = (*start + y) / 2;
                if (GridTop <= center && center <= GridBottom - 20) {
                    rows.push_back(center);
                }
            }
            start = std::nullopt;
        }
    }
    return rows;
}

std::vector<std::pair<int, int>> SPAFarmTaskPlugin::green_tiles(const cv::Mat& image) const
{
    std::vector<std::pair<int, int>> points;
    if (image.empty()) {
        return points;
    }

    cv::Mat hsv;
    cv::Mat mask_u8;
    cv::cvtColor(image, hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, cv::Scalar(35, 100, 100), cv::Scalar(85, 255, 255), mask_u8);

    cv::Mat mask;
    mask_u8.convertTo(mask, CV_32F, 1.0 / 255.0);
    for (int y = 0; y < mask.rows; ++y) {
        for (int x = 0; x < mask.cols; ++x) {
            if (y < 120 || y >= 760 || x < 180 || x >= 1650) {
                mask.at<float>(y, x) = 0.0F;
            }
        }
    }

    cv::Mat filtered;
    cv::blur(mask, filtered, cv::Size(61, 61));

    std::vector<int> xs;
    std::vector<int> ys;
    for (int y = 0; y < filtered.rows; ++y) {
        for (int x = 0; x < filtered.cols; ++x) {
            if (filtered.at<float>(y, x) > 0.93F) {
                ys.push_back(y);
                xs.push_back(x);
            }
        }
    }

    std::vector<size_t> order(xs.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t lhs, size_t rhs) { return xs[lhs] > xs[rhs]; });

    for (const size_t index : order) {
        const int x = xs[index];
        const int y = ys[index];
        bool keep = true;
        for (const auto& [px, py] : points) {
            if (!(std::abs(x - px) > 105 || std::abs(y - py) > 75)) {
                keep = false;
                break;
            }
        }
        if (keep) {
            points.emplace_back(x, y);
        }
        if (points.size() >= 40) {
            break;
        }
    }
    return points;
}

std::vector<std::pair<int, int>>
    SPAFarmTaskPlugin::right3(const std::vector<std::pair<int, int>>& tiles, const cv::Mat& image) const
{
    if (image.empty()) {
        return {};
    }

    cv::Mat hsv;
    cv::Mat mask;
    cv::cvtColor(image, hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, cv::Scalar(35, 100, 100), cv::Scalar(85, 255, 255), mask);
    for (int y = 0; y < mask.rows; ++y) {
        for (int x = 0; x < mask.cols; ++x) {
            if (y < 120 || y >= 760 || x >= 1650) {
                mask.at<uint8_t>(y, x) = 0;
            }
        }
    }

    std::vector<int> xs;
    std::vector<int> ys;
    for (int y = 0; y < mask.rows; ++y) {
        for (int x = 0; x < mask.cols; ++x) {
            if (mask.at<uint8_t>(y, x) > 0) {
                ys.push_back(y);
                xs.push_back(x);
            }
        }
    }
    if (xs.empty()) {
        return {};
    }

    std::vector<std::pair<double, double>> edges;
    for (int y0 = 140; y0 < 760; y0 += 40) {
        std::vector<double> selected;
        for (size_t i = 0; i < ys.size(); ++i) {
            if (ys[i] >= y0 && ys[i] < y0 + 40) {
                selected.push_back(static_cast<double>(xs[i]));
            }
        }
        if (selected.size() > 200) {
            std::sort(selected.begin(), selected.end());
            const double index = 0.995 * static_cast<double>(selected.size() - 1);
            const size_t lower = static_cast<size_t>(std::floor(index));
            const size_t upper = static_cast<size_t>(std::ceil(index));
            const double fraction = index - static_cast<double>(lower);
            const double percentile = selected[lower] + fraction * (selected[upper] - selected[lower]);
            edges.emplace_back(static_cast<double>(y0 + 20), percentile);
        }
    }
    if (edges.empty()) {
        return {};
    }

    double slope = 0.0;
    double intercept = 0.0;
    if (edges.size() >= 2) {
        const double count = static_cast<double>(edges.size());
        double sum_y = 0.0;
        double sum_x = 0.0;
        double sum_xy = 0.0;
        double sum_yy = 0.0;
        for (const auto& [edge_y, edge_x] : edges) {
            sum_y += edge_y;
            sum_x += edge_x;
            sum_xy += edge_y * edge_x;
            sum_yy += edge_y * edge_y;
        }
        const double denominator = count * sum_yy - sum_y * sum_y;
        slope = std::abs(denominator) > 1e-12 ? ((count * sum_xy - sum_y * sum_x) / denominator) : 0.0;
        intercept = (sum_x - slope * sum_y) / count;
        double max_residual = 0.0;
        for (const auto& [edge_y, edge_x] : edges) {
            max_residual = std::max(max_residual, edge_x - (slope * edge_y + intercept));
        }
        intercept += max_residual;
    }
    else {
        slope = 0.0;
        intercept = edges.front().second;
    }

    std::vector<std::pair<int, int>> kept;
    for (const auto& [x, y] : tiles) {
        const double edge = slope * y + intercept;
        const double tile_width = 100.0 + (y - 140) * 0.06;
        if (x >= edge - 3.0 * tile_width) {
            kept.emplace_back(x, y);
        }
    }
    return kept;
}

std::optional<std::string> SPAFarmTaskPlugin::pick_strategy()
{
    click_at(1500, 580, 1.0);

    std::vector<int> rows(GridY.begin(), GridY.end());
    std::unordered_set<std::string> tried;

    for (int page = 0; page < 8 && !need_exit(); ++page) {
        const cv::Mat image = capture();
        for (const int gy : rows) {
            if (!(GridTop + 60 <= gy && gy <= GridBottom - 40)) {
                continue;
            }
            for (const int gx : GridX) {
                if (has_color_badge(image, gx, gy) || (m_target_clears <= 3 && has_badge(image, gx, gy))) {
                    continue;
                }
                click_at(gx, gy, 0.7);

                std::vector<OcrItem> panel;
                std::string name;
                for (int attempt = 0; attempt < 3; ++attempt) {
                    panel = ocr(capture(), Box { 1440, 150, 1920, 800 });
                    name = strategy_name(panel);
                    if (!name.empty()) {
                        break;
                    }
                    sleep(600);
                }
                if (name.empty()) {
                    report_log(
                        "策略面板读取失败，跳过该格 (" + std::to_string(gx) + ", " + std::to_string(gy) + ")",
                        "warn");
                    continue;
                }

                if (tried.contains(name)) {
                    continue;
                }
                tried.insert(name);

                const int count = passed_count(panel);
                if (count >= m_target_clears) {
                    continue;
                }

                bool selected = false;
                for (int attempt = 0; attempt < 3; ++attempt) {
                    sleep(400);
                    if (strategy_name(ocr(capture(), Box { 1440, 150, 1920, 800 })) == name) {
                        selected = true;
                        break;
                    }
                    click_at(gx, gy, 0.9);
                }
                if (!selected) {
                    report_log("核对策略名不一致，跳过该格：" + name, "warn");
                    continue;
                }

                report_log(
                    "选择策略：" + name + "（已通过 " + std::to_string(count) + " 次" +
                    (count != 0 ? "，继续凑满 " + std::to_string(m_target_clears) + " 次）" : "，新策略）"));
                return name;
            }
        }

        scroll_grid();
        const cv::Mat after = capture();
        const auto shift = grid_shift(image, after);
        if (shift.has_value() && std::abs(*shift) < 20) {
            break;
        }
        rows = detect_rows(after);
        if (rows.empty()) {
            break;
        }
        report_log("策略列表下滑，继续查找（本页 " + std::to_string(rows.size()) + " 行）");
    }

    report_log("没有找到未获得角标的策略，全部完成？", "warn");
    return std::nullopt;
}

// ============================================================================
// 自选编队
// ============================================================================

void SPAFarmTaskPlugin::pick_squad()
{
    const cv::Mat image = capture();
    const auto items = ocr(image, Box { 560, 80, 1720, 960 });

    std::vector<OcrItem> names;
    std::vector<std::pair<double, double>> banned;
    for (const auto& item : items) {
        if (has_cjk(item.text) && item.text.find("盟约") == std::string::npos &&
            item.text.find("阵容") == std::string::npos && item.text.find("确定") == std::string::npos &&
            item.text.find("编队") == std::string::npos) {
            names.push_back(item);
        }
        if (item.text.find("禁用") != std::string::npos) {
            banned.emplace_back(item.x, item.y);
        }
    }

    std::vector<std::string> picked;
    const std::array<std::pair<int, int>, 2> bands { std::pair { 380, 520 }, std::pair { 760, 900 } };
    for (const auto& [low, high] : bands) {
        std::vector<OcrItem> row;
        for (const auto& candidate : names) {
            if (!(low <= candidate.y && candidate.y <= high) || candidate.text.find("禁用") != std::string::npos) {
                continue;
            }
            bool is_banned = false;
            for (const auto& [bx, by] : banned) {
                if (std::abs(candidate.x - bx) < 90.0 && (by - candidate.y) > 0.0 && (by - candidate.y) < 80.0) {
                    is_banned = true;
                    break;
                }
            }
            if (!is_banned) {
                row.push_back(candidate);
            }
        }

        std::stable_sort(row.begin(), row.end(), [this](const OcrItem& lhs, const OcrItem& rhs) {
            const int lhs_score = -(rank_operator(lhs.text, 6) + (is_dps(lhs.text) ? 5 : 0));
            const int rhs_score = -(rank_operator(rhs.text, 6) + (is_dps(rhs.text) ? 5 : 0));
            return lhs_score < rhs_score;
        });

        for (size_t i = 0; i < std::min<size_t>(2, row.size()); ++i) {
            click_at(static_cast<int>(row[i].x), static_cast<int>(row[i].y - 170.0), 0.4);
            picked.push_back(row[i].text);
        }
    }

    report_log("自选编队选择: " + (picked.empty() ? std::string("无（自选编队为空，直接开始）") : ("[" + join(picked, ", ") + "]")));
    click_at(1686, 986, 1.5);
}

// ============================================================================
// 商店解析
// ============================================================================

std::tuple<std::optional<int>, std::optional<int>, bool, std::optional<int>>
    SPAFarmTaskPlugin::read_shop(const cv::Mat& image)
{
    const auto gold = read_number_rec(image, Box { GoldBoxX1, GoldBoxY1, GoldBoxX2, GoldBoxY2 });
    const auto bottom = ocr(image, Box { 0, 820, 760, 1010 });
    const auto upgrade_hit = find(bottom, { "满级", "升级" });
    m_up_x = upgrade_hit.has_value() ? static_cast<int>(upgrade_hit->x) : 100;

    m_cards.clear();
    for (const int x : CardX) {
        if (x > m_up_x + 150) {
            m_cards.push_back(x);
        }
    }

    const bool maxed = find(bottom, { "满级" }).has_value();
    const auto up_cost = maxed ? std::nullopt : read_number_rec(image, Box { m_up_x - 22, 780, m_up_x + 22, 822 });
    const auto round = read_number_rec(image, Box { 728, 32, 778, 68 });
    return { gold, up_cost, maxed, round };
}

std::vector<SPAFarmTaskPlugin::ShopCard> SPAFarmTaskPlugin::shop_cards(const cv::Mat& image) const
{
    static const std::unordered_map<std::string, int> roman {
        { "I", 1 }, { "II", 2 }, { "III", 3 }, { "IV", 4 }, { "V", 5 }, { "VI", 6 },
    };

    const auto items = ocr(image, Box { 180, 770, 1700, 1060 });
    std::vector<ShopCard> cards;

    for (size_t i = 0; i < m_cards.size(); ++i) {
        const int x = m_cards[i];
        std::vector<OcrItem> mine;
        for (const auto& item : items) {
            if (std::abs(item.x - x) < 115.0) {
                mine.push_back(item);
            }
        }

        bool empty = false;
        for (const auto& item : mine) {
            if (upper_ascii(item.text).find("INFO") != std::string::npos) {
                empty = true;
                break;
            }
        }

        std::optional<int> cost;
        for (const auto& item : mine) {
            const std::string text = strip(item.text);
            if (item.y < 830.0 && std::abs(item.x - x) < 45.0 && is_ascii_digits(text)) {
                cost = std::atoi(text.c_str());
            }
        }
        if (!cost.has_value() && !empty) {
            cost = read_number_rec(image, Box { x - 22, 780, x + 22, 822 });
        }

        bool is_operator = false;
        for (const auto& item : mine) {
            if (item.y > 930.0 && item.y < 1000.0 && has_cjk(item.text)) {
                is_operator = true;
                break;
            }
        }

        int tier = 0;
        for (const auto& item : mine) {
            std::string text = upper_ascii(strip(item.text));
            text = replace_all(text, "1", "I");
            text = replace_all(text, "L", "I");
            if (item.y < 840.0 && item.x < (x - 50) && roman.contains(text)) {
                tier = roman.at(text);
            }
        }

        const bool high_tier = tier != 0 ? tier >= 5 : yellow_ratio(image, x - 104, 800, x - 78, 826) > 0.1;

        std::vector<std::string> names;
        for (const auto& item : mine) {
            if (item.y >= 1000.0 && has_cjk_or_alpha(item.text)) {
                names.push_back(item.text);
            }
        }
        const std::string name = names.empty() ? std::string() : strip(names.back());

        cards.push_back(ShopCard {
            static_cast<int>(i),
            x,
            empty,
            cost,
            high_tier,
            is_operator && !empty,
            name,
            is_dps(name),
        });
    }
    return cards;
}

bool SPAFarmTaskPlugin::buy(int index, std::optional<int> cost)
{
    if (index < 0 || index >= static_cast<int>(m_cards.size())) {
        return false;
    }
    const int x = m_cards[static_cast<size_t>(index)];
    const auto gold_before = read_number_rec(capture(), Box { GoldBoxX1, GoldBoxY1, GoldBoxX2, GoldBoxY2 });

    click_at(x, CardY, 0.8);
    const cv::Mat image = capture();
    if (find(ocr(image, Box { x - 120, 980, x + 120, 1060 }), { "无法购买" }).has_value() ||
        find(ocr(image, Box { 1400, 120, 1920, 260 }), { "资金不足" }).has_value()) {
        click_at(960, 600, 0.4);
        return false;
    }

    const auto left = ocr(image, Box { 0, 250, 700, 620 });
    const bool is_operator = find(left, { "特质", "天赋", "特性" }).has_value();
    if (!is_operator) {
        click_at(x, CardY, 0.4);
        click_at(960, 600, 0.4);
        return false;
    }

    click_at(x, CardY + 106, 1.2);
    const auto gold_after = read_number_rec(capture(), Box { GoldBoxX1, GoldBoxY1, GoldBoxX2, GoldBoxY2 });
    if ((!cost.has_value() || *cost != 0) && gold_before.has_value() && gold_after.has_value() &&
        *gold_after >= *gold_before) {
        click_at(960, 600, 0.4);
        return false;
    }
    return true;
}

std::optional<int> SPAFarmTaskPlugin::read_gold(const cv::Mat* image, int tries) const
{
    for (int attempt = 0; attempt < tries; ++attempt) {
        const cv::Mat target = (attempt == 0 && image != nullptr) ? *image : capture();
        auto gold = read_number_rec(target, Box { GoldBoxX1, GoldBoxY1, GoldBoxX2, GoldBoxY2 });
        if (!gold.has_value()) {
            gold = read_number(target, Box { 1760, 760, 1880, 825 });
        }
        if (gold.has_value() && *gold >= 0 && *gold <= 99) {
            return gold;
        }
        sleep(400);
    }
    return std::nullopt;
}

std::optional<int> SPAFarmTaskPlugin::gold_now()
{
    const auto gold = read_gold();
    if (!gold.has_value()) {
        ++m_gold_fail;
        if (m_gold_fail <= 3) {
            save_snapshot(capture(), "gold_unreadable");
        }
        report_log("警告：资金数字读取失败，按「未知」处理继续尝试", "warn");
    }
    return gold;
}

// ============================================================================
// 部署
// ============================================================================

bool SPAFarmTaskPlugin::is_green(const cv::Mat& image, std::pair<int, int> point, int radius)
{
    const cv::Rect roi = clamp_rect(
        image,
        point.first - radius,
        point.second - radius,
        point.first + radius,
        point.second + radius);
    if (roi.width <= 0 || roi.height <= 0) {
        return false;
    }
    cv::Mat hsv;
    cv::cvtColor(image(roi), hsv, cv::COLOR_BGR2HSV);
    cv::Mat mask;
    cv::inRange(hsv, cv::Scalar(35, 100, 100), cv::Scalar(85, 255, 255), mask);
    return (cv::mean(mask)[0] / 255.0) > 0.8;
}

std::string SPAFarmTaskPlugin::drag_name(const cv::Mat& image) const
{
    for (const auto& item : ocr(image, Box { 0, 262, 330, 322 })) {
        if (has_cjk_or_alpha(item.text) && upper_ascii(item.text).find("LV") == std::string::npos) {
            return strip(item.text);
        }
    }
    return {};
}

std::optional<int> SPAFarmTaskPlugin::slots_left(const cv::Mat& image) const
{
    const auto value = read_number_rec(image, Box { 1862, 566, 1897, 607 });
    if (value.has_value()) {
        return *value % 10;
    }
    return std::nullopt;
}

std::optional<std::pair<int, int>>
    SPAFarmTaskPlugin::new_bench_slot(const cv::Mat& before, const cv::Mat& after)
{
    if (before.empty() || after.empty()) {
        return std::nullopt;
    }
    const cv::Rect roi_before = clamp_rect(before, 200, 560, 1500, 770);
    const cv::Rect roi_after = clamp_rect(after, 200, 560, 1500, 770);
    if (roi_before.width <= 0 || roi_before.height <= 0 || roi_after.size() != roi_before.size()) {
        return std::nullopt;
    }

    cv::Mat diff;
    cv::absdiff(before(roi_before), after(roi_after), diff);
    cv::Mat gray;
    cv::cvtColor(diff, gray, cv::COLOR_BGR2GRAY);
    cv::Mat mask;
    cv::threshold(gray, mask, 40, 255, cv::THRESH_BINARY);
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, cv::Mat::ones(15, 15, CV_8U));

    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroids;
    const int count = cv::connectedComponentsWithStats(mask, labels, stats, centroids);
    if (count <= 1) {
        return std::nullopt;
    }

    int best = 1;
    int best_area = stats.at<int>(1, cv::CC_STAT_AREA);
    for (int k = 2; k < count; ++k) {
        const int area = stats.at<int>(k, cv::CC_STAT_AREA);
        if (area > best_area) {
            best_area = area;
            best = k;
        }
    }
    return std::make_pair(
        static_cast<int>(centroids.at<double>(best, 0)) + roi_after.x,
        static_cast<int>(centroids.at<double>(best, 1)) + roi_after.y);
}

bool SPAFarmTaskPlugin::sell_at(int x, int y) const
{
    const auto gold_before = read_number_rec(capture(), Box { GoldBoxX1, GoldBoxY1, GoldBoxX2, GoldBoxY2 });
    touch_event(TouchDown, x, y);
    drag_path(x, y, x, y - 120, 6);
    drag_path(x, y - 120, SellPoint.first, SellPoint.second, 10);
    sleep(400);
    touch_event(TouchUp, SellPoint.first, SellPoint.second);
    sleep(1000);
    const auto gold_after = read_number_rec(capture(), Box { GoldBoxX1, GoldBoxY1, GoldBoxX2, GoldBoxY2 });
    return gold_before.has_value() && gold_after.has_value() && *gold_after > *gold_before;
}

bool SPAFarmTaskPlugin::in_facing_mode(const cv::Mat* image) const
{
    const cv::Mat target = image != nullptr ? *image : capture();
    return find(ocr(target, Box { 300, 0, 1700, 820 }), { "点击取消" }).has_value();
}

bool SPAFarmTaskPlugin::set_facing(int tx, int ty, int tries)
{
    for (int attempt = 0; attempt < tries; ++attempt) {
        const int distance = 300 + 60 * attempt;
        touch_event(TouchDown, tx, ty);
        drag_path(tx, ty, tx + distance, ty, 10);
        sleep(300);
        touch_event(TouchUp, tx + distance, ty);
        sleep(1200);
        if (!in_facing_mode()) {
            return true;
        }
        report_log("设置朝向未生效，重试 (" + std::to_string(tx) + ", " + std::to_string(ty) + ")", "warn");
    }
    save_snapshot(capture(), "facing_failed");
    report_log("设置朝向多次失败，截图已保存到 debug/spa_farm", "warn");
    return false;
}

std::vector<std::string> SPAFarmTaskPlugin::tile_order(const std::string& name) const
{
    std::vector<std::string> safe_tiles;
    std::vector<std::string> path_tiles;
    std::vector<std::string> rest_tiles;
    for (size_t i = 0; i < layout().safe.size(); ++i) {
        safe_tiles.push_back("safe" + std::to_string(i));
    }
    for (size_t i = 0; i < layout().path.size(); ++i) {
        path_tiles.push_back("path" + std::to_string(i));
    }
    for (size_t i = 0; i < layout().rest.size(); ++i) {
        rest_tiles.push_back("rest" + std::to_string(i));
    }

    std::vector<std::string> order;
    if (is_safe_operator(name)) {
        order.insert(order.end(), safe_tiles.begin(), safe_tiles.end());
        order.insert(order.end(), path_tiles.begin(), path_tiles.end());
        order.insert(order.end(), rest_tiles.begin(), rest_tiles.end());
    }
    else {
        order.insert(order.end(), path_tiles.begin(), path_tiles.end());
        order.insert(order.end(), rest_tiles.begin(), rest_tiles.end());
        order.insert(order.end(), safe_tiles.begin(), safe_tiles.end());
    }
    return order;
}

std::pair<int, int> SPAFarmTaskPlugin::tile_xy(const std::string& tile_name)
{
    size_t pos = tile_name.size();
    while (pos > 0 && std::isdigit(static_cast<unsigned char>(tile_name[pos - 1])) != 0) {
        --pos;
    }
    const std::string kind = tile_name.substr(0, pos);
    const int index = std::atoi(tile_name.substr(pos).c_str());
    if (kind == "safe") {
        return layout().safe[static_cast<size_t>(index)];
    }
    if (kind == "path") {
        return layout().path[static_cast<size_t>(index)];
    }
    return layout().rest[static_cast<size_t>(index)];
}

std::optional<std::pair<int, int>> SPAFarmTaskPlugin::deploy_layout(
    std::pair<int, int> bench_xy,
    Board& board,
    const std::string& name_hint,
    int tier_hint,
    int depth)
{
    const auto [bx, by] = bench_xy;
    const auto [lx, ly] = layout().lift;

    const cv::Mat before = capture();
    const auto slots_before = slots_left(before);

    touch_event(TouchDown, bx, by);
    drag_path(bx, by, bx, by - 120, 6);
    drag_path(bx, by - 120, lx, ly, 8);
    sleep(800);
    const cv::Mat image = capture();

    std::vector<Point2> all_points;
    all_points.insert(all_points.end(), layout().safe.begin(), layout().safe.end());
    all_points.insert(all_points.end(), layout().path.begin(), layout().path.end());
    all_points.insert(all_points.end(), layout().rest.begin(), layout().rest.end());

    std::vector<std::string> tabs;
    for (const auto& item : ocr(image, Box { 0, 520, 700, 600 })) {
        const std::string text = strip(item.text);
        if (text == "特质" || text == "技能" || text == "特性" || text == "天赋") {
            tabs.push_back(item.text);
        }
    }

    bool any_green = false;
    for (const auto& point : all_points) {
        if (is_green(image, point)) {
            any_green = true;
            break;
        }
    }

    const std::string drag_label = drag_name(image);
    const bool has_lv = find(ocr(image, Box { 0, 262, 330, 380 }), { "LV" }).has_value();
    const bool picked = !tabs.empty() || !drag_label.empty() || has_lv || any_green;
    report_log(
        "拿起判定: tabs=" + std::to_string(tabs.size()) + " 名称=" + (drag_label.empty() ? "-" : drag_label) +
        " LV=" + (has_lv ? "1" : "0") + " 绿格=" + (any_green ? "1" : "0") + " -> " + (picked ? "已拿起" : "未拿起"));
    if (!picked) {
        touch_event(TouchUp, lx, ly);
        sleep(800);
        return std::nullopt;
    }

    bool has_operator_tab = false;
    for (const auto& tab : tabs) {
        const std::string text = strip(tab);
        if (text == "特质" || text == "天赋") {
            has_operator_tab = true;
            break;
        }
    }
    if (!has_operator_tab) {
        touch_event(TouchUp, lx, ly);
        sleep(800);
        const std::string held = drag_name(image);
        report_log("拿起的是装备，放回: " + (held.empty() ? std::string("?") : held));
        return std::nullopt;
    }

    const std::string held_name = drag_name(image);
    const std::string name = !held_name.empty() ? held_name : name_hint;
    int tier = yellow_ratio(image, 20, 328, 58, 362) > 0.08 ? 5 : 1;
    if (priority_index(name).has_value()) {
        tier = std::max(tier, 5);
    }
    if (depth >= 1 && tier < 5) {
        drag_path(lx, ly, SellPoint.first, SellPoint.second, 10);
        touch_event(TouchUp, SellPoint.first, SellPoint.second);
        sleep(1000);
        report_log("换下的是低星干员，卖掉: " + (name.empty() ? std::string("?") : name));
        return std::nullopt;
    }
    tier = std::max(tier, depth == 0 ? tier_hint : 0);
    const int my_rank = rank_operator(name, tier);

    const auto occupant = [&](const std::string& tile_name) -> std::optional<std::pair<std::string, int>> {
        const auto it = board.occ.find(tile_name);
        if (it != board.occ.end()) {
            return std::make_pair(it->second.name, it->second.tier);
        }
        if (is_green(image, tile_xy(tile_name))) {
            return std::nullopt;
        }
        return std::make_pair(std::string("?"), 5);
    };

    const auto value = [&](const std::string& tile_name, const std::pair<std::string, int>& occ) -> int {
        int rank = rank_operator(occ.first, occ.second);
        if (tile_name.starts_with("safe") && is_safe_operator(name) && !is_safe_operator(occ.first)) {
            rank -= 2000;
        }
        return rank;
    };

    const std::vector<std::string> order = tile_order(name);
    std::vector<std::string> safe_tiles;
    std::vector<std::string> other_tiles;
    for (const auto& tile_name : order) {
        if (tile_name.starts_with("safe")) {
            safe_tiles.push_back(tile_name);
        }
        else {
            other_tiles.push_back(tile_name);
        }
    }
    const std::vector<std::vector<std::string>> groups = is_safe_operator(name)
                                                             ? std::vector<std::vector<std::string>> { safe_tiles,
                                                                                                       other_tiles }
                                                             : std::vector<std::vector<std::string>> { other_tiles,
                                                                                                       safe_tiles };

    std::string target;
    std::optional<std::pair<std::string, int>> swap_out;
    for (const auto& group : groups) {
        std::map<std::string, std::optional<std::pair<std::string, int>>> occupants;
        std::vector<std::string> free_tiles;
        for (const auto& tile_name : group) {
            occupants[tile_name] = occupant(tile_name);
            if (!occupants[tile_name].has_value()) {
                free_tiles.push_back(tile_name);
            }
        }
        if (!free_tiles.empty()) {
            target = free_tiles.front();
            break;
        }

        std::vector<std::tuple<int, int, std::string>> candidates;
        for (size_t index = 0; index < group.size(); ++index) {
            const std::string& tile_name = group[index];
            const auto& occ = *occupants[tile_name];
            if (occ.second < 5 || (occ.first != "?" && value(tile_name, occ) < my_rank)) {
                candidates.emplace_back(value(tile_name, occ), static_cast<int>(index), tile_name);
            }
        }
        std::sort(candidates.begin(), candidates.end());
        if (!candidates.empty()) {
            target = std::get<2>(candidates.front());
            swap_out = *occupants[target];
            break;
        }
    }

    if (target.empty()) {
        touch_event(TouchUp, lx, ly);
        sleep(800);
        report_log("没有可用位置，留在备战席: " + (name.empty() ? std::string("?") : name));
        return std::nullopt;
    }
    report_log(
        "目标格: " + target + " 换下=" + (swap_out.has_value() ? swap_out->first : std::string("-")) + " 可放置数=" +
        (slots_before.has_value() ? std::to_string(*slots_before) : std::string("?")));

    const auto [tx, ty] = tile_xy(target);
    drag_path(lx, ly, tx, ty, 10);
    touch_event(TouchUp, tx, ty);
    sleep(1000);
    if (!find(ocr(capture(), Box { 300, 0, 1700, 820 }), { "点击取消", "取消" }).has_value()) {
        report_log("放置失败: " + name + " " + target, "warn");
        return std::nullopt;
    }

    m_last_tile = Point2 { tx, ty };
    set_facing(tx, ty);
    const cv::Mat after = capture();
    board.occ[target] = OccupantInfo { name, tier, false };

    const auto slots_after = slots_left(after);
    if (!swap_out.has_value() && slots_before.has_value() && slots_after.has_value() &&
        *slots_after >= *slots_before) {
        report_log("意外交换: " + target + " 上原本有干员，重新安排被换下的干员", "warn");
        swap_out = std::make_pair(std::string("?"), 5);
    }
    report_log(
        "部署 " + (name.empty() ? std::string("?") : name) + " T" + std::to_string(tier) + " -> " + target +
        (swap_out.has_value() ? ("（换下 " + swap_out->first + "）") : std::string()));

    if (swap_out.has_value()) {
        board.pending = true;
        const auto out = new_bench_slot(before, after);
        if (out.has_value()) {
            if (swap_out->second < 5) {
                const bool ok = sell_at(out->first, out->second);
                report_log("卖掉换下的干员 " + swap_out->first + (ok ? " 成功" : "（未确认）"));
            }
            else if (depth == 0) {
                deploy_layout(*out, board, swap_out->first, swap_out->second, depth + 1);
            }
        }
    }

    m_last_tier = tier;
    return Point2 { tx, ty };
}

std::optional<std::pair<int, int>> SPAFarmTaskPlugin::deploy_from_bench_old(
    std::pair<int, int> bench_xy,
    bool quiet,
    Board* board,
    bool hi)
{
    const auto [bx, by] = bench_xy;
    const cv::Mat before = capture();
    const auto slots_before = slots_left(before);

    std::vector<std::pair<int, int>> local_used;
    std::vector<std::pair<int, int>> local_hi;
    auto& used = board != nullptr ? board->used : local_used;
    auto& hi_tiles = board != nullptr ? board->hi_tiles : local_hi;

    const auto is_near = [](std::pair<int, int> point, const std::vector<std::pair<int, int>>& list, int distance = 95) {
        for (const auto& [x, y] : list) {
            if (std::abs(point.first - x) <= distance && std::abs(point.second - y) <= distance) {
                return true;
            }
        }
        return false;
    };

    for (int attempt = 0; attempt < 3; ++attempt) {
        touch_event(TouchDown, bx, by);
        drag_path(bx, by, bx, by - 180, 9);
        sleep(800);
        const cv::Mat image = capture();
        m_last_tier = yellow_ratio(image, 20, 328, 58, 362) > 0.08 ? 5 : 1;
        const bool is_hi = hi || (m_last_tier >= 5);

        const auto candidates = right3(green_tiles(image), image);
        std::vector<std::pair<int, int>> free_tiles;
        for (const auto& tile : candidates) {
            if (!is_near(tile, used)) {
                free_tiles.push_back(tile);
            }
        }

        bool swap = false;
        int tx = 0;
        int ty = 0;
        if (!free_tiles.empty()) {
            const size_t index = std::min<size_t>(static_cast<size_t>(attempt), free_tiles.size() - 1);
            std::tie(tx, ty) = free_tiles[index];
        }
        else {
            std::vector<std::pair<int, int>> targets;
            if (is_hi) {
                for (const auto& tile : used) {
                    if (!is_near(tile, hi_tiles)) {
                        targets.push_back(tile);
                    }
                }
                for (const auto& tile : candidates) {
                    if (!is_near(tile, hi_tiles)) {
                        targets.push_back(tile);
                    }
                }
            }
            std::stable_sort(targets.begin(), targets.end(), [](const auto& lhs, const auto& rhs) {
                return (-lhs.first) < (-rhs.first);
            });
            if (targets.empty()) {
                touch_event(TouchUp, bx, by - 180);
                sleep(800);
                if (!quiet) {
                    report_log("最右3列没有可用格", "warn");
                    save_snapshot(image, "no_tiles");
                }
                return std::nullopt;
            }
            std::tie(tx, ty) = targets.front();
            swap = true;
        }

        drag_path(bx, by - 180, tx, ty, 10);
        touch_event(TouchUp, tx, ty);
        sleep(1000);
        const cv::Mat placed = capture();
        if (find(ocr(placed, Box { 300, 0, 1700, 820 }), { "点击取消", "取消" }).has_value()) {
            m_last_tile = Point2 { tx, ty };
            set_facing(tx, ty);
            const cv::Mat after = capture();
            if (!is_near({ tx, ty }, used)) {
                used.push_back({ tx, ty });
            }
            const auto slots_after = slots_left(after);
            const bool swapped = slots_before.has_value() && slots_after.has_value() && *slots_after >= *slots_before;
            if (swapped && !swap && !is_hi) {
                report_log(
                    "该格已有干员（发生交换），标记为占用 (" + std::to_string(tx) + ", " + std::to_string(ty) + ")",
                    "warn");
                return std::nullopt;
            }
            if (swapped) {
                const auto out = new_bench_slot(before, after).value_or(std::make_pair(bx, by));
                swipe_to(out.first, out.second, SellPoint.first, SellPoint.second, 900, 1.3);
                report_log(
                    "V/VI 替换低星干员 (" + std::to_string(tx) + ", " + std::to_string(ty) + ")，已卖出换下的干员");
            }
            if (is_hi) {
                hi_tiles.push_back({ tx, ty });
            }
            report_log("部署到 (" + std::to_string(tx) + ", " + std::to_string(ty) + ") " + (is_hi ? "V/VI" : ""));
            m_last_tile = Point2 { tx, ty };
            return Point2 { tx, ty };
        }
        report_log("放置失败，重试 (" + std::to_string(tx) + ", " + std::to_string(ty) + ")", "warn");
    }
    return std::nullopt;
}

std::optional<std::pair<int, int>> SPAFarmTaskPlugin::deploy_from_bench(
    std::pair<int, int> bench_xy,
    bool quiet,
    Board* board,
    bool hi)
{
    // 内置 tiles.json 布局可用时走固定布局换位部署，否则回退到最右3列自动识别
    if (layout_ready() && board != nullptr) {
        return deploy_layout(bench_xy, *board, "", hi ? 5 : 0, 0);
    }
    return deploy_from_bench_old(bench_xy, quiet, board, hi);
}

bool SPAFarmTaskPlugin::sell_lowest(Board& board)
{
    std::vector<size_t> low_indexes;
    for (size_t i = 0; i < board.ops.size(); ++i) {
        if (board.ops[i].tier < 5) {
            low_indexes.push_back(i);
        }
    }
    if (low_indexes.empty()) {
        return false;
    }

    const size_t min_index = *std::min_element(low_indexes.begin(), low_indexes.end(), [&](size_t lhs, size_t rhs) {
        return board.ops[lhs].tier < board.ops[rhs].tier;
    });
    const OpRecord record = board.ops[min_index];
    const auto slots_before = slots_left(capture());
    swipe_to(record.x, record.y, SellPoint.first, SellPoint.second, 900, 1.3);
    const auto slots_after = slots_left(capture());

    const std::string repr =
        "(" + std::to_string(record.x) + ", " + std::to_string(record.y) + ", " + std::to_string(record.tier) + ")";
    if (slots_before.has_value() && slots_after.has_value() && *slots_after <= *slots_before) {
        report_log("卖出可能失败（可放置数未增加）" + repr, "warn");
    }

    board.ops.erase(board.ops.begin() + static_cast<std::ptrdiff_t>(min_index));
    if (record.tile.has_value()) {
        const auto it = std::find(board.used.begin(), board.used.end(), *record.tile);
        if (it != board.used.end()) {
            board.used.erase(it);
        }
    }
    report_log("卖掉低星干员 " + repr);
    return true;
}

bool SPAFarmTaskPlugin::handle_popups() const
{
    const auto items = ocr(capture());
    if (find(items, { "确认准备", "资金将在" }).has_value()) {
        click_at(860, 602, 0.5);
        click_at(1410, 728, 1.5);
        return true;
    }
    return false;
}

bool SPAFarmTaskPlugin::buy_and_deploy(Board& board, int index, int tier, std::optional<int> cost)
{
    const cv::Mat before = capture();
    if (!buy(index, cost)) {
        return false;
    }
    const cv::Mat after = capture();
    const auto slot = new_bench_slot(before, after);
    report_log(
        "购买[" + std::to_string(index) + "] 备战席差分: " +
        (slot.has_value() ? ("(" + std::to_string(slot->first) + ", " + std::to_string(slot->second) + ")")
                          : std::string("未检测到")));
    if (!slot.has_value()) {
        report_log("没找到新买的干员（可能合成晋升了）");
        handle_popups();
        board.pending = true;
        return true;
    }
    const auto position = deploy_from_bench(*slot, false, &board, tier >= 5);
    if (position.has_value()) {
        board.ops.push_back(OpRecord { position->first, position->second, std::max(tier, m_last_tier), m_last_tile });
    }
    else {
        board.pending = true;
    }
    return true;
}

void SPAFarmTaskPlugin::deploy_bench(Board& board)
{
    static const std::vector<std::vector<std::pair<int, int>>> bench_rows {
        { { 1390, 700 }, { 1300, 700 }, { 1210, 700 }, { 1120, 700 }, { 1030, 700 }, { 940, 700 } },
        { { 1260, 600 }, { 1150, 600 }, { 1040, 600 }, { 930, 600 }, { 820, 600 } },
    };

    for (int iteration = 0; iteration < 6 && !need_exit(); ++iteration) {
        bool placed = false;
        for (const auto& row : bench_rows) {
            int empty = 0;
            for (const auto& point : row) {
                const auto position = deploy_from_bench(point, true, &board, false);
                if (position.has_value()) {
                    board.ops.push_back(OpRecord { position->first, position->second, m_last_tier, m_last_tile });
                    placed = true;
                    break;
                }
                ++empty;
                if (empty >= 2) {
                    break;
                }
            }
            if (placed) {
                break;
            }
        }
        if (!placed) {
            return;
        }
    }
}

// ============================================================================
// 休整期三阶段运营
// ============================================================================

void SPAFarmTaskPlugin::prep_phase(Board& board, const cv::Mat& image_in)
{
    cv::Mat image = image_in;
    auto [gold, up_cost, maxed, round_opt] = read_shop(image);

    int round = 0;
    if (!round_opt.has_value()) {
        if (board.prep_done) {
            return;
        }
        round = std::max(1, board.round_done + 1);
    }
    else {
        round = *round_opt;
    }
    if (round == board.round_done) {
        return;
    }

    const double start_time = now_sec();
    const auto opt_str = [](const std::optional<int>& value) {
        return value.has_value() ? std::to_string(*value) : std::string("None");
    };
    report_log(
        "回合" + std::to_string(round) + " 资金" + opt_str(gold) + " 升级费用" + opt_str(up_cost) +
        " 满级" + (maxed ? "True" : "False"));
    save_snapshot(image, "shop_r" + std::to_string(round));
    report_progress();

    // 诊断日志：本回合卡牌解析结果（费用/高星/是否干员/名字），便于对照识别是否正常
    std::string card_dump;
    for (const auto& card : shop_cards(image)) {
        card_dump += "[" + std::to_string(card.index) + (card.empty ? "空" : "") + " 费" + opt_str(card.cost) +
                     (card.high_tier ? " 高" : "") + (card.is_operator ? " 人" : " 备") + " " + card.name + "]";
    }
    report_log("卡牌(UPX=" + std::to_string(m_up_x) + "): " + card_dump);

    // 第一阶段：Round 1 先买 2 个干员
    if (board.round_done < 0 && round == 1) {
        int bought = 0;
        for (const auto& card : shop_cards(image)) {
            const auto current_gold = gold_now();
            if (bought >= 2) {
                break;
            }
            if (card.is_operator && card.cost.has_value() &&
                (!current_gold.has_value() || *card.cost <= *current_gold)) {
                if (is_healer(card.name) && board.healers >= 1) {
                    report_log("跳过第二个医疗干员: " + card.name);
                    continue;
                }
                if (buy_and_deploy(board, card.index, card.high_tier ? 5 : 1, card.cost)) {
                    ++bought;
                    board.healers += is_healer(card.name) ? 1 : 0;
                }
            }
        }
    }

    // 第二阶段：连升 4 次商店
    if (!maxed) {
        for (int iteration = 0; iteration < 4 && !need_exit(); ++iteration) {
            image = capture();
            std::tie(gold, up_cost, maxed, std::ignore) = read_shop(image);
            if (maxed || (gold.has_value() && up_cost.has_value() && *gold < *up_cost)) {
                break;
            }
            const int previous_up_x = m_up_x;
            click_at(m_up_x, 920, 0.6);
            click_at(m_up_x, 920, 1.0);
            const auto [gold_after, cost_after, maxed_after, ignored] = read_shop(capture());
            (void)ignored;
            maxed = maxed_after;
            const bool upgraded = maxed || (m_up_x != previous_up_x) ||
                                  (gold.has_value() && gold_after.has_value() && *gold_after < *gold) ||
                                  (up_cost.has_value() && cost_after.has_value() && *cost_after != *up_cost);
            if (!upgraded) {
                break;
            }
            report_log(
                "商店升级，资金 " + opt_str(gold) + "->" + opt_str(gold_after) + "，升级费用 " + opt_str(up_cost) +
                "->" + (maxed ? std::string("满级") : opt_str(cost_after)));
        }

        // 第三阶段：没满级就继续补买
        if (!maxed) {
            for (const auto& card : shop_cards(capture())) {
                const cv::Mat current = capture();
                const auto current_gold = read_gold(&current);
                const auto slots = slots_left(current);
                if ((current_gold.has_value() && *current_gold == 0) || (slots.has_value() && *slots == 0)) {
                    report_log(
                        std::string("停止购买：") +
                        (current_gold.has_value() && *current_gold == 0 ? "资金为 0" : "部署位已满"));
                    break;
                }
                if (!current_gold.has_value()) {
                    report_log("警告：资金读取失败，仍尝试购买（买不起时游戏会提示）", "warn");
                }
                if (card.is_operator && card.cost.has_value() &&
                    (!current_gold.has_value() || *card.cost <= *current_gold)) {
                    if (is_healer(card.name) && board.healers >= 1) {
                        report_log("跳过第二个医疗干员: " + card.name);
                        continue;
                    }
                    if (buy_and_deploy(board, card.index, card.high_tier ? 5 : 1, card.cost)) {
                        board.healers += is_healer(card.name) ? 1 : 0;
                    }
                }
            }
        }
    }

    // 第四阶段：满级后刷新 + 购买 5/6 级干员
    if (maxed) {
        int refreshes = 0;
        int same_count = 0;
        std::optional<std::vector<std::pair<std::string, std::optional<int>>>> last_signature;

        for (int iteration = 0; iteration < 40 && !need_exit(); ++iteration) {
            image = capture();
            gold = read_gold(&image);
            bool bought_any = false;
            const auto all_cards = shop_cards(image);

            std::vector<std::pair<std::string, std::optional<int>>> signature;
            signature.reserve(all_cards.size());
            for (const auto& card : all_cards) {
                signature.emplace_back(card.name, card.cost);
            }

            if (last_signature.has_value()) {
                if (signature == *last_signature) {
                    ++same_count;
                    if (same_count >= 2) {
                        save_snapshot(image, "refresh_no_effect");
                        report_log("停止刷新：连续刷新后商店没有变化（可能资金不足或刷新按钮没点到）", "warn");
                        break;
                    }
                }
                else {
                    same_count = 0;
                }
            }
            last_signature = signature;

            if (!gold.has_value()) {
                ++m_gold_fail;
                if (m_gold_fail <= 3) {
                    save_snapshot(image, "gold_unreadable");
                }
                report_log("警告：资金数字读取失败，按「未知」处理（尝试购买和刷新，由游戏提示判断）", "warn");
            }
            const int gold_value = gold.value_or(99);

            std::vector<ShopCard> free_cards;
            std::vector<ShopCard> affordable;
            for (const auto& card : all_cards) {
                if (card.is_operator && card.cost.has_value() && *card.cost == 0) {
                    free_cards.push_back(card);
                }
                if (card.high_tier && card.is_operator && card.cost.has_value() && *card.cost <= gold_value) {
                    affordable.push_back(card);
                }
            }

            const auto is_owned = [&](const ShopCard& card) {
                for (const auto& [name, count] : board.owned) {
                    (void)count;
                    if (!name.empty() && (card.name.find(name) != std::string::npos ||
                                          name.find(card.name) != std::string::npos)) {
                        return true;
                    }
                }
                return false;
            };

            std::vector<ShopCard> priority_cards;
            for (const auto& card : affordable) {
                if (priority_index(card.name).has_value()) {
                    priority_cards.push_back(card);
                }
            }
            std::stable_sort(
                priority_cards.begin(),
                priority_cards.end(),
                [this](const ShopCard& lhs, const ShopCard& rhs) {
                    return *priority_index(lhs.name) < *priority_index(rhs.name);
                });

            std::vector<ShopCard> pick;
            if (!free_cards.empty()) {
                auto sorted_free = free_cards;
                std::stable_sort(
                    sorted_free.begin(),
                    sorted_free.end(),
                    [this](const ShopCard& lhs, const ShopCard& rhs) {
                        return (-rank_operator(lhs.name, lhs.high_tier ? 5 : 1)) <
                               (-rank_operator(rhs.name, rhs.high_tier ? 5 : 1));
                    });
                pick.push_back(sorted_free.front());
            }
            else if (!priority_cards.empty()) {
                pick = priority_cards;
            }
            else {
                std::vector<ShopCard> owned_cards;
                for (const auto& card : affordable) {
                    if (is_owned(card)) {
                        owned_cards.push_back(card);
                    }
                }
                if (!owned_cards.empty()) {
                    pick = owned_cards;
                }
                else {
                    std::vector<ShopCard> dps_cards;
                    for (const auto& card : affordable) {
                        if (card.dps) {
                            dps_cards.push_back(card);
                        }
                    }
                    if (!dps_cards.empty()) {
                        pick = dps_cards;
                    }
                    else if (gold.has_value() && (*gold - 1 < 4)) {
                        pick = affordable;
                    }
                }
            }

            bool failed = false;
            for (const auto& card : pick) {
                const std::string tag =
                    priority_index(card.name).has_value() ? "优先" : (card.dps ? "输出" : "非输出");
                report_log("购买 V/VI: " + card.name + " " + tag + " 费用" + opt_str(card.cost));
                if (buy_and_deploy(board, card.index, card.high_tier ? 5 : 1, card.cost)) {
                    board.owned[card.name] += 1;
                    bought_any = true;
                    break;
                }
                failed = true;
                break;
            }
            if (failed) {
                report_log("购买失败，结束本回合购买", "warn");
                break;
            }
            if (bought_any) {
                last_signature = std::nullopt;
                continue;
            }
            if (gold.has_value() && *gold < 4) {
                report_log("停止刷新：资金 " + std::to_string(*gold) + " 不足 4（本回合刷新 " +
                           std::to_string(refreshes) + " 次）");
                break;
            }

            ++refreshes;
            int high_count = 0;
            for (const auto& card : all_cards) {
                if (card.high_tier && card.is_operator) {
                    ++high_count;
                }
            }
            report_log(
                "刷新商店（第 " + std::to_string(refreshes) + " 次）：资金 " +
                (gold.has_value() ? std::to_string(*gold) : std::string("未知")) + "，当前 5/6 级干员 " +
                std::to_string(high_count) + " 张，可买 " + std::to_string(affordable.size()) + " 张");
            click_at(RefreshPoint.first, RefreshPoint.second, 1.0);
        }
    }

    if (board.pending) {
        deploy_bench(board);
        board.pending = false;
    }
    board.round_done = round;
    board.prep_done = true;
    sleep(1500);
    click_at(ReadyPoint.first, ReadyPoint.second, 1.5);
    board.last_ready = now_sec();
    handle_popups();

    const int elapsed = static_cast<int>(std::nearbyint(now_sec() - start_time));
    report_log("回合" + std::to_string(round) + " 准备耗时 " + std::to_string(elapsed) + "s");
}

// ============================================================================
// 进度持久化
// ============================================================================

std::set<std::string> SPAFarmTaskPlugin::load_done() const
{
    std::set<std::string> done;
    const auto path = done_file_path();
    if (path.empty()) {
        return done;
    }
    std::ifstream ifs(path);
    if (!ifs) {
        return done;
    }
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        line = strip(line);
        if (!line.empty()) {
            done.insert(line);
        }
    }
    return done;
}

void SPAFarmTaskPlugin::save_done(const std::set<std::string>& done) const
{
    const auto path = done_file_path();
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs) {
        return;
    }
    for (const auto& item : done) {
        ofs << item << "\n";
    }
}

// ============================================================================
// 上报
// ============================================================================

void SPAFarmTaskPlugin::report_log(const std::string& message, const std::string& level)
{
    if (level == "warn") {
        LogWarn << "SPA |" << message;
    }
    else if (level == "error") {
        LogError << "SPA |" << message;
    }
    else {
        LogInfo << "SPA |" << message;
    }

    auto info = basic_info_with_what("SPAFarmLog");
    auto& details = info["details"];
    details["message"] = message;
    details["level"] = level;
    callback(AsstMsg::SubTaskExtraInfo, info);
}

void SPAFarmTaskPlugin::report_progress()
{
    auto info = basic_info_with_what("SPAFarmProgress");
    auto& details = info["details"];
    details["runs"] = m_successful_runs;
    details["attempts"] = m_attempts;
    details["target"] = m_max_runs;
    details["strategy"] = m_current_strategy;
    callback(AsstMsg::SubTaskExtraInfo, info);
}

// ============================================================================
// 主循环
// ============================================================================

bool SPAFarmTaskPlugin::_run()
{
    LogTraceFunction;

    m_target_clears = std::clamp(m_target_clears, 1, 20);
    if (m_max_runs <= 0) {
        m_max_runs = std::numeric_limits<int>::max();
    }
    apply_profile();
    m_cards.assign(CardX.begin(), CardX.end());

    report_log(
        "运行模式：" + std::string(m_low_profile ? "低配模式" : "标准模式") + " · " +
        (m_target_clears >= 6 ? "彩色角标（同一策略通关 6 次）" : "金色角标（同一策略通关 3 次）"));

    // 起始界面自检：必须能看到「极速模拟」或未完成对局的「断线重连」
    bool startable = false;
    for (int attempt = 0; attempt < 3 && !need_exit(); ++attempt) {
        const std::string text = join_text(ocr(capture()));
        if (text.find("极速模拟") != std::string::npos || text.find("断线重连") != std::string::npos) {
            startable = true;
            break;
        }
        sleep(2000);
    }
    if (!startable) {
        report_log(
            "当前不是卫戍协议的极速模拟界面，任务已停止。请先进入卫戍协议主界面（能看到「极速模拟」按钮）后再开始。",
            "error");
        return false;
    }

    std::set<std::string> done = load_done();
    Board board;
    double last_seen = now_sec();
    double last_progress = now_sec();
    double run_start = now_sec();
    std::optional<int> last_round;
    int idle = 0;

    while (m_successful_runs < m_max_runs && !need_exit()) {
        const cv::Mat image = capture();
        if (image.empty()) {
            sleep(1000);
            continue;
        }
        const auto items = ocr(image);
        const std::string text = join_text(items);
        const std::string upper_text = upper_ascii(text);

        if (now_sec() - last_progress > StallTimeoutSec) {
            save_snapshot(image, "stalled");
            report_log("已有 10 分钟没有进展，判定为卡死，停止运行。", "warn");
            break;
        }

        bool in_garrison = false;
        for (const char* keyword : GarrisonKeywords) {
            if (upper_text.find(keyword) != std::string::npos) {
                in_garrison = true;
                break;
            }
        }
        if (in_garrison) {
            last_seen = now_sec();
        }
        else if (now_sec() - last_seen > LeaveTimeoutSec) {
            save_snapshot(image, "left_garrison");
            report_log("已有 30 秒检测不到卫戍协议界面，立即停止。", "warn");
            break;
        }

        if (text.find("点击取消") != std::string::npos && text.find("准备就绪") != std::string::npos) {
            report_log(
                "检测到停在选择朝向界面，自动补设朝向 " +
                (m_last_tile.has_value()
                     ? ("(" + std::to_string(m_last_tile->first) + ", " + std::to_string(m_last_tile->second) + ")")
                     : std::string("None")));
            if (m_last_tile.has_value()) {
                set_facing(m_last_tile->first, m_last_tile->second);
            }
            else if (const auto hit = find(items, { "点击取消" }); hit.has_value()) {
                click_at(static_cast<int>(hit->x), static_cast<int>(hit->y), 1.0);
            }
            continue;
        }

        if (text.find("返回主页") != std::string::npos) {
            save_snapshot(image, "result");
            const bool cleared = text.find("模拟完成") != std::string::npos;
            if (cleared && !m_current_strategy.empty()) {
                done.insert(m_current_strategy);
                save_done(done);
            }
            if (cleared) {
                ++m_successful_runs;
            }
            ++m_attempts;
            const int duration = static_cast<int>(std::nearbyint(now_sec() - run_start));
            const std::string strategy = m_current_strategy.empty() ? std::string("None") : m_current_strategy;
            report_log(
                std::string("本局") + (cleared ? "通关" : "失败") + "，策略 " + strategy + "，用时 " +
                std::to_string(duration) + "s，已通关 " + std::to_string(m_successful_runs) + "/" +
                (m_max_runs == std::numeric_limits<int>::max() ? std::string("∞") : std::to_string(m_max_runs)) +
                "，已打 " + std::to_string(m_attempts) + " 局");
            m_current_strategy.clear();
            last_progress = now_sec();
            if (const auto hit = find(items, { "返回主页" }); hit.has_value()) {
                click_at(static_cast<int>(hit->x), static_cast<int>(hit->y), 4.0);
            }
            board = Board();
            report_progress();
            if (m_successful_runs >= m_max_runs) {
                break;
            }
        }
        else if (
            text.find("请选择模拟难度") != std::string::npos ||
            (text.find("开始模拟") != std::string::npos && text.find("极速模拟") != std::string::npos &&
             text.find("回合") == std::string::npos)) {
            const auto hit = find(items, { "开始模拟" });
            click_at(hit.has_value() ? static_cast<int>(hit->x) : 1700, hit.has_value() ? static_cast<int>(hit->y) : 986, 3.0);
            board = Board();
        }
        else if (text.find("确认准备") != std::string::npos || text.find("资金将在") != std::string::npos) {
            click_at(860, 602, 0.5);
            click_at(1410, 728, 2.0);
        }
        else if (text.find("断线重连") != std::string::npos) {
            const auto hit = find(items, { "断线重连" });
            report_log("检测到未完成的对局，断线重连");
            click_at(hit.has_value() ? static_cast<int>(hit->x) : 1660, hit.has_value() ? static_cast<int>(hit->y) : 970, 5.0);
            board = Board();
        }
        else if (text.find("独立模拟") != std::string::npos && text.find("极速模拟") != std::string::npos) {
            const auto hit = find(items, { "极速模拟" });
            click_at(
                hit.has_value() ? static_cast<int>(hit->x) : 1280,
                hit.has_value() ? static_cast<int>(hit->y) : 1000,
                3.0);
        }
        else if (text.find("确认本局信息") != std::string::npos) {
            click_at(1686, 986, 3.0);
        }
        else if (text.find("选择策略") != std::string::npos) {
            run_start = now_sec();
            last_progress = now_sec();
            const auto strategy = pick_strategy();
            save_done(done);
            if (!strategy.has_value()) {
                break;
            }
            m_current_strategy = *strategy;
            report_progress();
            click_at(1680, 990, 3.0);
        }
        else if (text.find("确定自选编队") != std::string::npos) {
            pick_squad();
        }
        else if (
            text.find("回合") != std::string::npos && text.find("准备就绪") != std::string::npos &&
            (text.find("刷新") != std::string::npos || text.find("升级") != std::string::npos ||
             text.find("满级") != std::string::npos)) {
            prep_phase(board, image);
            if (board.prep_done && (now_sec() - board.last_ready > 10.0)) {
                report_log("仍在休整期，重新点击准备就绪");
                click_at(ReadyPoint.first, ReadyPoint.second, 1.5);
                handle_popups();
                board.last_ready = now_sec();
            }
            if (!last_round.has_value() || board.round_done != *last_round) {
                last_round = board.round_done;
                last_progress = now_sec();
            }
            idle = 0;
            continue;
        }
        else if (
            text.find("回合") != std::string::npos ||
            (text.find("查看") != std::string::npos && text.find("极速模拟") != std::string::npos) ||
            upper_text.find("COUNTDOWN") != std::string::npos) {
            board.prep_done = false;
            sleep(4000);
            idle = 0;
            continue;
        }
        else {
            std::optional<OcrItem> hit;
            if (in_garrison) {
                std::vector<std::string> generic;
                generic.reserve(GenericKeywords.size());
                for (const char* keyword : GenericKeywords) {
                    generic.emplace_back(keyword);
                }
                hit = find(items, generic);
            }
            if (hit.has_value()) {
                report_log("通用点击: " + hit->text);
                click_at(static_cast<int>(hit->x), static_cast<int>(hit->y), 2.0);
                idle = 0;
            }
            else {
                ++idle;
                if (idle % 5 == 0) {
                    save_snapshot(image, "unknown");
                }
                sleep(2000);
            }
            continue;
        }
        sleep(1000);
    }

    std::vector<std::string> done_list(done.begin(), done.end());
    report_log(
        "结束。本次已通关 " + std::to_string(m_successful_runs) + " 局，共打 " + std::to_string(m_attempts) + " 局");
    report_log("已获得角标的策略: [" + join(done_list, ", ") + "]");
    report_progress();
    return !need_exit();
}
} // namespace asst
