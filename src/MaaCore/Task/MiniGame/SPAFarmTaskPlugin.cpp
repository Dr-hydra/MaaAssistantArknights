#include "SPAFarmTaskPlugin.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <numeric>

#include <opencv2/imgproc.hpp>

#include "Config/GeneralConfig.h"
#include "Config/Miscellaneous/OcrPack.h"
#include "Config/ResourceLoader.h"
#include "Controller/Controller.h"
#include "Status.h"
#include "Utils/Logger.hpp"

using namespace asst;

namespace
{

constexpr int kGridX[4] = { 680, 890, 1100, 1310 };
constexpr int kGridY[4] = { 370, 580, 790, 1000 };
constexpr int kGridTop = 250;
constexpr int kGridBottom = 1050;

constexpr int kCardX[6] = { 304, 550, 790, 1034, 1274, 1520 };
constexpr int kCardY = 920;
constexpr std::array<int, 4> kGoldBox = { 1785, 768, 1850, 816 };
constexpr std::pair<int, int> kSellPos = { 1760, 900 };
constexpr std::pair<int, int> kRefreshPos = { 1786, 686 };
constexpr std::pair<int, int> kReadyPos = { 1780, 170 };

constexpr double kLeaveTimeoutSec = 30.0;
constexpr double kStallTimeoutSec = 600.0;

const std::vector<std::pair<std::string, std::string>> kPriorityFull = {
    { "塔露拉", "塔露拉" },     { "维什戴尔", "维什戴尔" }, { "星熊", "斩业星熊" },
    { "安洁莉娜", "予愿安洁莉娜" }, { "能天使", "新约能天使" }, { "凯尔希", "凯尔希·思衡托" },
    { "望", "望" },             { "初雪", "圣聆初雪" },     { "逻各斯", "逻各斯" },
    { "蕾缪安", "蕾缪安" },     { "贝洛内", "贝洛内" },     { "冻河", "冻河" },
    { "明霄陈", "赤刃明霄陈" }, { "结城理", "结城理" }
};

const std::vector<std::string> kSafeKeysFull = {
    "维什戴尔", "望", "能天使", "初雪", "蕾缪安", "逻各斯", "冻河"
};

const std::vector<std::pair<std::string, std::string>> kPriorityLow = {
    { "塔露拉", "塔露拉" },     { "史尔特尔", "史尔特尔" }, { "耀骑士", "耀骑士临光" },
    { "逻各斯", "逻各斯" },     { "克莱门莎", "克莱门莎" }, { "锏", "锏" },
    { "帕拉斯", "帕拉斯" },     { "真言", "真言" },         { "蕾缪安", "蕾缪安" },
    { "烛煌", "烛煌" },         { "艾丽妮", "艾丽妮" },     { "薇薇安娜", "薇薇安娜" },
    { "维娜", "维娜·维多利亚" }, { "乌尔比安", "乌尔比安" }, { "棘刺", "棘刺" },
    { "凛冬", "怒潮凛冬" },     { "初雪", "圣聆初雪" },     { "迷迭香", "迷迭香" },
    { "鸿雪", "鸿雪" },         { "贝洛内", "贝洛内" },     { "佩佩", "佩佩" },
    { "隐德来希", "隐德来希" }, { "冻河", "冻河" },         { "酒神", "酒神" }
};

const std::vector<std::string> kSafeKeysLow = {
    "逻各斯", "真言", "蕾缪安", "烛煌", "初雪", "迷迭香", "鸿雪", "冻河", "酒神"
};

const std::vector<std::string> kDpsList = {
    "怒潮凛冬", "艾丽妮", "棘刺", "佩佩", "圣聆初雪", "乌尔比安", "薇薇安娜", "贝洛内",
    "维娜·维多利亚", "维娜", "迷迭香", "鸿雪", "阿米娅", "赤刃明霄陈", "司霆惊蛰",
    "维什戴尔", "斩业星熊", "丰川祥子", "结城理", "凛御银灰", "歌蕾蒂娅", "Sharp",
    "领主·Sharp", "Pith", "Stormeye", "预备干员-近卫", "预备干员-术师", "引星棘刺",
    "克莱门莎", "耀骑士临光", "荒芜拉普兰德", "逻各斯", "真言", "史尔特尔", "锏",
    "塔露拉", "夕", "蕾缪安", "帕拉斯", "望", "塑心"
};

const std::vector<std::string> kHealers = {
    "褐果", "莎草", "哈洛德", "瑰盐", "乌啾", "风絮", "流明", "夜莺", "华法琳", "纯烬艾雅法拉", "Mon3tr", "Touch"
};

const std::unordered_map<std::string, int> kRoman = {
    { "I", 1 }, { "II", 2 }, { "III", 3 }, { "IV", 4 }, { "V", 5 }, { "VI", 6 }
};

const std::vector<std::vector<std::pair<int, int>>> kBenchRows = {
    { { 1390, 700 }, { 1300, 700 }, { 1210, 700 }, { 1120, 700 }, { 1030, 700 }, { 940, 700 } },
    { { 1260, 600 }, { 1150, 600 }, { 1040, 600 }, { 930, 600 }, { 820, 600 } }
};

const std::vector<std::string> kGenericKeywords = {
    "点击任意", "继续", "返回", "确认", "确定", "领取", "下一步", "完成"
};

const std::vector<std::string> kGarrisonKeywords = {
    "极速模拟", "卫戍", "STRONGHOLD", "确认准备", "资金将在", "选择策略",
    "确认本局信息", "确定自选编队", "返回主页", "模拟完成", "模拟中断",
    "COUNTDOWN", "断线重连", "放弃模拟"
};

double now_sec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string trim_copy(std::string_view sv)
{
    size_t b = 0;
    size_t e = sv.size();
    while (b < e && static_cast<unsigned char>(sv[b]) <= 0x20) {
        ++b;
    }
    while (e > b && static_cast<unsigned char>(sv[e - 1]) <= 0x20) {
        --e;
    }
    return std::string(sv.substr(b, e - b));
}

std::string to_upper_ascii(std::string_view sv)
{
    std::string out(sv);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

std::vector<std::string> split_utf8_codepoints(std::string_view s)
{
    std::vector<std::string> chars;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0x80) == 0) {
            len = 1;
        }
        else if ((c & 0xE0) == 0xC0) {
            len = 2;
        }
        else if ((c & 0xF0) == 0xE0) {
            len = 3;
        }
        else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        if (i + len > s.size()) {
            len = s.size() - i;
        }
        chars.emplace_back(s.substr(i, len));
        i += len;
    }
    return chars;
}

bool has_cjk(std::string_view s)
{
    for (const auto& ch : split_utf8_codepoints(s)) {
        if (ch.size() == 3) {
            unsigned char b0 = static_cast<unsigned char>(ch[0]);
            unsigned char b1 = static_cast<unsigned char>(ch[1]);
            unsigned char b2 = static_cast<unsigned char>(ch[2]);
            uint32_t cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
            if (cp >= 0x4E00 && cp <= 0x9FFF) {
                return true;
            }
        }
    }
    return false;
}

bool has_cjk_or_alpha(std::string_view s)
{
    if (has_cjk(s)) {
        return true;
    }
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            return true;
        }
    }
    return false;
}

std::string extract_digits(std::string_view s)
{
    std::string d;
    for (unsigned char c : s) {
        if (c >= '0' && c <= '9') {
            d.push_back(static_cast<char>(c));
        }
    }
    return d;
}

} // namespace

bool SPAFarmTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }
    const std::string& task = details.get("details", "task", "");
    return task.ends_with("MiniGame@SPA@BadgeFarm");
}

bool SPAFarmTaskPlugin::set_params(const json::value& params)
{
    m_profile = params.get("profile", std::string("full"));
    m_target = params.get("target", 3) == 6 ? 6 : 3;
    m_runs = params.get("runs", 20);
    m_attempts = params.get("attempts", 0);
    apply_profile(m_profile);
    return true;
}

Point SPAFarmTaskPlugin::to_720p(int x_1080, int y_1080)
{
    return Point(
        static_cast<int>(std::lround(x_1080 * 1280.0 / 1920.0)),
        static_cast<int>(std::lround(y_1080 * 720.0 / 1080.0)));
}

cv::Mat SPAFarmTaskPlugin::shot() const
{
    for (int i = 0; i < 5; ++i) {
        cv::Mat img = ctrler()->get_image();
        if (!img.empty()) {
            if (img.cols != 1920 || img.rows != 1080) {
                // 使用 INTER_CUBIC 放大以保留小字号数字与罗马数字边缘锐度
                cv::resize(img, img, cv::Size(1920, 1080), 0.0, 0.0, cv::INTER_CUBIC);
            }
            return img;
        }
    }
    return {};
}

bool SPAFarmTaskPlugin::sleep_sec(double sec)
{
    if (sec <= 0.0) {
        return !need_exit();
    }
    return sleep(static_cast<unsigned int>(std::lround(sec * 1000.0)));
}

void SPAFarmTaskPlugin::tap(double x, double y, double wait_sec)
{
    Point pt = to_720p(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)));
    ctrler()->click(pt);
    sleep_sec(wait_sec);
}

void SPAFarmTaskPlugin::swipe(double x1, double y1, double x2, double y2, int ms, double wait_sec)
{
    Point p1 = to_720p(static_cast<int>(std::lround(x1)), static_cast<int>(std::lround(y1)));
    Point p2 = to_720p(static_cast<int>(std::lround(x2)), static_cast<int>(std::lround(y2)));
    ctrler()->swipe(Rect(p1.x, p1.y, 1, 1), Rect(p2.x, p2.y, 1, 1), ms);
    sleep_sec(wait_sec);
}

void SPAFarmTaskPlugin::drag_move(int x1, int y1, int x2, int y2, int duration_ms, double post_wait_sec)
{
    Point p1 = to_720p(x1, y1);
    Point p2 = to_720p(x2, y2);
    ctrler()->swipe(Rect(p1.x, p1.y, 1, 1), Rect(p2.x, p2.y, 1, 1), duration_ms);
    sleep_sec(post_wait_sec);
}

// 分段拖拽节拍，与原实现一致：按下保持 30ms，move 每步 12ms
// （MAA Win32 AnchoredTouch 底层 touch_move 每步约需 12ms 才能提交一帧）
namespace
{
constexpr int DragDownHoldMs = 30;
constexpr int DragMoveStepMs = 12;
} // namespace

void SPAFarmTaskPlugin::touch_event(int type, double x, double y) const
{
    InputEvent event;
    event.type = static_cast<InputEvent::Type>(type);
    event.pointerId = 0;
    event.point = to_720p(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)));
    ctrler()->inject_input_event(event);
}

void SPAFarmTaskPlugin::drag_path(double x1, double y1, double x2, double y2, int steps) const
{
    const auto start = std::chrono::steady_clock::now();
    for (int k = 1; k <= steps; ++k) {
        const double r = static_cast<double>(k) / static_cast<double>(steps);
        touch_event(
            static_cast<int>(InputEvent::Type::TOUCH_MOVE),
            x1 + (x2 - x1) * r,
            y1 + (y2 - y1) * r);
        std::this_thread::sleep_until(start + std::chrono::milliseconds(DragMoveStepMs * k));
    }
}

void SPAFarmTaskPlugin::log_info(const std::string& msg)
{
    Log.info("SPAFarm |", msg);
    json::value info = basic_info();
    info["what"] = "SPAFarmStatus";
    info["details"]["message"] = msg;
    callback(AsstMsg::SubTaskExtraInfo, info);
}

// ============================================================================
// 核心修复：1:1 还原 GarrisonFarm.exe 的 ocr / numrec / num 识别链路
// 1. 针对指定 box=(x1,y1,x2,y2)，裁剪后统一增加 P=40 的 cv::BORDER_REPLICATE 边缘复制扩充，
//    解决 PP-OCR DBNet 检测模型在窄框边缘漏检文字（如拖拽干员名、特质/天赋标签、商店底栏）的问题。
// 2. 直接调用 WordOcr::get_instance().recognize(work, without_det)，彻底避开：
//    - VisionHelper 基类构造函数中虚函数 _set_roi 未派发到子类导致 OCRerConfig::m_roi 为 {0,0,0,0} 的 C++ 缺陷；
//    - VisionHelper::correct_rect 对 1280x720 边界的潜在截断；
//    - OCRer::postproc_replace_ / OcrConfig 等价类字符强制替换导致的罗马数字/干员名失真。
// 3. numrec 显式传入 without_det = true（对应原版 use_det=False, use_rec=True），
//    解决单个数字（如资金 GOLD_BOX、费用、回合数、剩余可部署数）被检测模型漏检的问题。
// ============================================================================
std::vector<SPAFarmTaskPlugin::OcrItem> SPAFarmTaskPlugin::ocr(
    const cv::Mat& img,
    const std::optional<std::array<int, 4>>& box) const
{
    if (img.empty()) {
        return {};
    }

    cv::Mat work;
    double ox = 0.0;
    double oy = 0.0;

    if (box.has_value()) {
        auto [x1, y1, x2, y2] = *box;
        x1 = std::clamp(x1, 0, img.cols);
        x2 = std::clamp(x2, 0, img.cols);
        y1 = std::clamp(y1, 0, img.rows);
        y2 = std::clamp(y2, 0, img.rows);
        if (x2 <= x1 || y2 <= y1) {
            return {};
        }
        constexpr int P = 40;
        cv::Mat crop = img(cv::Rect(x1, y1, x2 - x1, y2 - y1));
        cv::copyMakeBorder(crop, work, P, P, P, P, cv::BORDER_REPLICATE);
        ox = static_cast<double>(x1 - P);
        oy = static_cast<double>(y1 - P);
    }
    else {
        work = img;
    }

    auto raw_results = WordOcr::get_instance().recognize(work, false);
    std::vector<OcrItem> out;
    out.reserve(raw_results.size());
    for (const auto& item : raw_results) {
        double cx = ox + item.rect.x + item.rect.width * 0.5;
        double cy = oy + item.rect.y + item.rect.height * 0.5;
        out.push_back(OcrItem { cx, cy, item.text, item.score });
    }
    return out;
}

std::optional<SPAFarmTaskPlugin::OcrItem> SPAFarmTaskPlugin::find(
    const std::vector<OcrItem>& items,
    std::initializer_list<std::string_view> kw)
{
    for (const auto& it : items) {
        for (std::string_view k : kw) {
            if (!k.empty() && it.text.find(k) != std::string::npos) {
                return it;
            }
        }
    }
    return std::nullopt;
}

std::optional<int> SPAFarmTaskPlugin::numrec(const cv::Mat& img, const std::array<int, 4>& box) const
{
    if (img.empty()) {
        return std::nullopt;
    }
    auto [x1, y1, x2, y2] = box;
    x1 = std::clamp(x1, 0, img.cols);
    x2 = std::clamp(x2, 0, img.cols);
    y1 = std::clamp(y1, 0, img.rows);
    y2 = std::clamp(y2, 0, img.rows);
    if (x2 > x1 && y2 > y1) {
        cv::Mat crop = img(cv::Rect(x1, y1, x2 - x1, y2 - y1)).clone();
        // without_det = true: 只用识别模型读固定位置的数字（对应 use_det=False, use_rec=True）
        auto raw_results = WordOcr::get_instance().recognize(crop, true);
        if (!raw_results.empty()) {
            std::string d = extract_digits(raw_results.front().text);
            if (!d.empty() && raw_results.front().score >= 0.4) {
                return std::stoi(d);
            }
        }
    }
    return num(img, box);
}

std::optional<int> SPAFarmTaskPlugin::num(const cv::Mat& img, const std::array<int, 4>& box) const
{
    std::string best;
    for (const auto& it : ocr(img, box)) {
        std::string d = extract_digits(it.text);
        if (d.size() > best.size()) {
            best = std::move(d);
        }
    }
    if (!best.empty()) {
        return std::stoi(best);
    }
    return std::nullopt;
}

double SPAFarmTaskPlugin::yellow_ratio(const cv::Mat& img, const std::array<int, 4>& box)
{
    auto [x1, y1, x2, y2] = box;
    x1 = std::clamp(x1, 0, img.cols);
    x2 = std::clamp(x2, 0, img.cols);
    y1 = std::clamp(y1, 0, img.rows);
    y2 = std::clamp(y2, 0, img.rows);
    if (x2 <= x1 || y2 <= y1) {
        return 0.0;
    }
    cv::Mat hsv;
    cv::cvtColor(img(cv::Rect(x1, y1, x2 - x1, y2 - y1)), hsv, cv::COLOR_BGR2HSV);
    cv::Mat mask;
    cv::inRange(hsv, cv::Scalar(8, 120, 150), cv::Scalar(35, 255, 255), mask);
    return cv::mean(mask)[0] / 255.0;
}

bool SPAFarmTaskPlugin::has_color_badge(const cv::Mat& img, int gx, int gy)
{
    int x1 = gx + 40;
    int y1 = std::max(0, gy - 110);
    int x2 = gx + 95;
    int y2 = gy - 45;
    x1 = std::clamp(x1, 0, img.cols);
    x2 = std::clamp(x2, 0, img.cols);
    y1 = std::clamp(y1, 0, img.rows);
    y2 = std::clamp(y2, 0, img.rows);
    if (x2 <= x1 || y2 <= y1) {
        return false;
    }
    cv::Mat hsv;
    cv::cvtColor(img(cv::Rect(x1, y1, x2 - x1, y2 - y1)), hsv, cv::COLOR_BGR2HSV);
    cv::Mat cyan_mask;
    cv::inRange(hsv, cv::Scalar(70, 40, 150), cv::Scalar(140, 255, 255), cyan_mask);
    double cyan = cv::mean(cyan_mask)[0] / 255.0;
    return cyan > 0.15 && yellow_ratio(img, { x1, y1, x2, y2 }) < 0.05;
}

bool SPAFarmTaskPlugin::has_badge(const cv::Mat& img, int gx, int gy)
{
    return yellow_ratio(img, { gx + 40, gy - 100, gx + 90, gy - 50 }) > 0.12
        || yellow_ratio(img, { gx + 30, std::max(0, gy - 130), gx + 100, gy - 30 }) > 0.1;
}

int SPAFarmTaskPlugin::passed_count(const std::vector<OcrItem>& panel)
{
    for (const auto& it : panel) {
        auto pos = it.text.find("通过");
        if (pos != std::string::npos) {
            std::string tail = it.text.substr(pos + std::string("通过").size());
            std::string d = extract_digits(tail);
            return d.empty() ? 1 : std::stoi(d);
        }
    }
    return 0;
}

std::optional<int> SPAFarmTaskPlugin::grid_shift(const cv::Mat& before, const cv::Mat& after)
{
    cv::Mat strip = before(cv::Rect(600, 560, 800, 140));
    cv::Mat area = after(cv::Rect(600, kGridTop, 800, kGridBottom - kGridTop));
    cv::Mat res;
    cv::matchTemplate(area, strip, res, cv::TM_CCOEFF_NORMED);
    double min_val = 0.0, max_val = 0.0;
    cv::Point min_loc, max_loc;
    cv::minMaxLoc(res, &min_val, &max_val, &min_loc, &max_loc);
    if (max_val < 0.6) {
        return std::nullopt;
    }
    return 560 - (kGridTop + max_loc.y);
}

std::vector<int> SPAFarmTaskPlugin::detect_rows(const cv::Mat& img)
{
    cv::Mat gray;
    cv::cvtColor(img(cv::Rect(620, 0, 120, img.rows)), gray, cv::COLOR_BGR2GRAY);
    cv::Mat flt;
    gray.convertTo(flt, CV_32F);

    std::vector<bool> on(img.rows, false);
    for (int y = 0; y < img.rows; ++y) {
        cv::Scalar mean, stddev;
        cv::meanStdDev(flt.row(y), mean, stddev);
        on[y] = (stddev[0] > 18.0);
    }

    std::vector<int> rows;
    std::optional<int> start;
    for (int y = kGridTop - 60; y < kGridBottom + 20 && y < img.rows; ++y) {
        if (on[y] && !start.has_value()) {
            start = y;
        }
        if ((!on[y] || y == kGridBottom + 19) && start.has_value()) {
            if (y - *start >= 60) {
                int c = (*start + y) / 2;
                if (c >= kGridTop && c <= kGridBottom - 20) {
                    rows.push_back(c);
                }
            }
            start.reset();
        }
    }
    return rows;
}

void SPAFarmTaskPlugin::scroll_grid()
{
    // 原实现：慢速向上拖动约两行，结束前停顿以消除惯性
    touch_event(static_cast<int>(InputEvent::Type::TOUCH_DOWN), 1000, 900);
    std::this_thread::sleep_for(std::chrono::milliseconds(DragDownHoldMs));
    drag_path(1000, 900, 1000, 480, 14);
    sleep_sec(0.5);
    touch_event(static_cast<int>(InputEvent::Type::TOUCH_UP), 1000, 480);
    sleep_sec(0.8);
}

std::string SPAFarmTaskPlugin::strategy_name(const std::vector<OcrItem>& panel)
{
    for (const auto& it : panel) {
        if (it.cy > 520.0 && it.cy < 650.0 && has_cjk(it.text)
            && it.text.find("生命") == std::string::npos
            && it.text.find("通过") == std::string::npos
            && it.text.find("【") == std::string::npos) {
            return it.text;
        }
    }
    return "";
}

std::optional<std::string> SPAFarmTaskPlugin::pick_strategy(const std::set<std::string>& /*done*/)
{
    tap(1500, 580, 1.0);
    std::vector<int> rows(std::begin(kGridY), std::end(kGridY));
    std::set<std::string> tried;

    for (int page = 0; page < 8 && !need_exit(); ++page) {
        cv::Mat img = shot();
        for (int gy : rows) {
            if (gy < kGridTop + 60 || gy > kGridBottom - 40) {
                continue;
            }
            for (int gx : kGridX) {
                if (has_color_badge(img, gx, gy) || (m_target <= 3 && has_badge(img, gx, gy))) {
                    continue;
                }
                tap(gx, gy, 0.7);
                std::vector<OcrItem> panel;
                std::string sname;
                for (int t = 0; t < 3; ++t) {
                    panel = ocr(shot(), std::array<int, 4> { 1440, 150, 1920, 800 });
                    sname = strategy_name(panel);
                    if (!sname.empty()) {
                        break;
                    }
                    sleep_sec(0.6);
                }
                if (sname.empty()) {
                    log_info("策略面板读取失败，跳过该格");
                    continue;
                }
                if (tried.count(sname)) {
                    continue;
                }
                tried.insert(sname);
                int n = passed_count(panel);
                if (n >= m_target) {
                    continue;
                }
                bool ok_sel = false;
                for (int v = 0; v < 3; ++v) {
                    sleep_sec(0.4);
                    if (strategy_name(ocr(shot(), std::array<int, 4> { 1440, 150, 1920, 800 })) == sname) {
                        ok_sel = true;
                        break;
                    }
                    tap(gx, gy, 0.9);
                }
                if (!ok_sel) {
                    log_info("核对策略名不一致，跳过该格: " + sname);
                    continue;
                }
                log_info("选择策略: " + sname + "（已通过 " + std::to_string(n) + " 次）");
                return sname;
            }
        }
        scroll_grid();
        cv::Mat after = shot();
        auto sh = grid_shift(img, after);
        if (sh.has_value() && std::abs(*sh) < 20) {
            break;
        }
        rows = detect_rows(after);
        if (rows.empty()) {
            break;
        }
    }
    log_info("没有找到未获得角标的策略");
    return std::nullopt;
}

void SPAFarmTaskPlugin::apply_profile(const std::string& name)
{
    m_profile = (name == "low") ? "low" : "full";
    if (m_profile == "low") {
        m_priority = kPriorityLow;
        m_safe_keys = kSafeKeysLow;
    }
    else {
        m_priority = kPriorityFull;
        m_safe_keys = kSafeKeysFull;
    }
}

bool SPAFarmTaskPlugin::match_key(std::string_view name, std::string_view key)
{
    if (name.empty() || key.empty()) {
        return false;
    }
    auto key_chars = split_utf8_codepoints(key);
    std::string trimmed = trim_copy(name);
    auto name_chars = split_utf8_codepoints(trimmed);
    if (key_chars.size() == 1) {
        return trimmed == key || (name_chars.size() <= 2 && name.find(key) != std::string_view::npos);
    }
    return name.find(key) != std::string_view::npos;
}

std::optional<int> SPAFarmTaskPlugin::prio_index(std::string_view name) const
{
    for (size_t i = 0; i < m_priority.size(); ++i) {
        if (match_key(name, m_priority[i].first)) {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

bool SPAFarmTaskPlugin::is_safe_op(std::string_view name) const
{
    for (const auto& k : m_safe_keys) {
        if (match_key(name, k)) {
            return true;
        }
    }
    return false;
}

bool SPAFarmTaskPlugin::is_healer(std::string_view name)
{
    if (name.empty()) {
        return false;
    }
    auto name_chars = split_utf8_codepoints(name);
    for (const auto& h : kHealers) {
        if (name.find(h) != std::string_view::npos
            || (name_chars.size() >= 2 && std::string_view(h).find(name) != std::string_view::npos)) {
            return true;
        }
    }
    return false;
}

double SPAFarmTaskPlugin::seq_ratio(std::string_view a, std::string_view b)
{
    auto ca = split_utf8_codepoints(a);
    auto cb = split_utf8_codepoints(b);
    if (ca.empty() && cb.empty()) {
        return 1.0;
    }
    if (ca.empty() || cb.empty()) {
        return 0.0;
    }
    auto count_matches = [&](auto& self, size_t alo, size_t ahi, size_t blo, size_t bhi) -> size_t {
        size_t best_i = alo, best_j = blo, best_size = 0;
        for (size_t i = alo; i < ahi; ++i) {
            for (size_t j = blo; j < bhi; ++j) {
                size_t k = 0;
                while (i + k < ahi && j + k < bhi && ca[i + k] == cb[j + k]) {
                    ++k;
                }
                if (k > best_size) {
                    best_i = i;
                    best_j = j;
                    best_size = k;
                }
            }
        }
        if (best_size == 0) {
            return 0;
        }
        size_t left = (alo < best_i && blo < best_j) ? self(self, alo, best_i, blo, best_j) : 0;
        size_t right = (best_i + best_size < ahi && best_j + best_size < bhi)
            ? self(self, best_i + best_size, ahi, best_j + best_size, bhi)
            : 0;
        return left + best_size + right;
    };
    size_t matches = count_matches(count_matches, 0, ca.size(), 0, cb.size());
    return (2.0 * static_cast<double>(matches)) / static_cast<double>(ca.size() + cb.size());
}

bool SPAFarmTaskPlugin::is_dps(std::string_view name)
{
    if (name.empty()) {
        return false;
    }
    for (const auto& d : kDpsList) {
        if (name.find(d) != std::string_view::npos
            || std::string_view(d).find(name) != std::string_view::npos
            || seq_ratio(d, name) >= 0.6) {
            return true;
        }
    }
    return false;
}

int SPAFarmTaskPlugin::rank(std::string_view name, int tier) const
{
    if (auto idx = prio_index(name)) {
        return 1000 - *idx;
    }
    if (tier >= 5) {
        return 500 + (is_dps(name) ? 50 : 0);
    }
    return tier;
}

void SPAFarmTaskPlugin::pick_squad()
{
    cv::Mat img = shot();
    auto items = ocr(img, std::array<int, 4> { 560, 80, 1720, 960 });
    std::vector<OcrItem> names;
    std::vector<std::pair<double, double>> banned;

    for (const auto& it : items) {
        if (it.text.find("禁用") != std::string::npos) {
            banned.emplace_back(it.cx, it.cy);
        }
        if (has_cjk(it.text)
            && it.text.find("盟约") == std::string::npos
            && it.text.find("阵容") == std::string::npos
            && it.text.find("确定") == std::string::npos
            && it.text.find("编队") == std::string::npos) {
            names.push_back(it);
        }
    }

    for (auto [lo, hi] : { std::pair<int, int> { 380, 520 }, std::pair<int, int> { 760, 900 } }) {
        std::vector<OcrItem> row;
        for (const auto& it : names) {
            if (it.cy >= lo && it.cy <= hi && it.text.find("禁用") == std::string::npos) {
                bool is_banned = false;
                for (auto [bx, by] : banned) {
                    if (std::abs(it.cx - bx) < 90.0 && (by - it.cy) > 0.0 && (by - it.cy) < 80.0) {
                        is_banned = true;
                        break;
                    }
                }
                if (!is_banned) {
                    row.push_back(it);
                }
            }
        }
        std::sort(row.begin(), row.end(), [&](const OcrItem& a, const OcrItem& b) {
            int sa = rank(a.text, 6) + (is_dps(a.text) ? 5 : 0);
            int sb = rank(b.text, 6) + (is_dps(b.text) ? 5 : 0);
            return sa > sb;
        });
        for (size_t i = 0; i < std::min<size_t>(2, row.size()); ++i) {
            tap(row[i].cx, row[i].cy - 170.0, 0.4);
        }
    }
    tap(1686, 986, 1.5);
}

std::tuple<std::optional<int>, std::optional<int>, bool, std::optional<int>>
SPAFarmTaskPlugin::read_shop(const cv::Mat& img)
{
    auto gold = numrec(img, kGoldBox);
    auto bottom = ocr(img, std::array<int, 4> { 0, 820, 760, 1010 });
    auto hit = find(bottom, { "满级", "升级" });
    m_upx = hit.has_value() ? static_cast<int>(hit->cx) : 100;
    m_cards.clear();
    for (int x : kCardX) {
        if (x > m_upx + 150) {
            m_cards.push_back(x);
        }
    }
    bool maxed = find(bottom, { "满级" }).has_value();
    std::optional<int> up_cost = maxed
        ? std::nullopt
        : numrec(img, { m_upx - 22, 780, m_upx + 22, 822 });
    auto rnd = numrec(img, { 728, 32, 778, 68 });
    return { gold, up_cost, maxed, rnd };
}

std::vector<SPAFarmTaskPlugin::ShopCard> SPAFarmTaskPlugin::shop_cards(const cv::Mat& img) const
{
    auto items = ocr(img, std::array<int, 4> { 180, 770, 1700, 1060 });
    std::vector<ShopCard> out;
    for (size_t idx = 0; idx < m_cards.size(); ++idx) {
        int x = m_cards[idx];
        std::vector<OcrItem> mine;
        for (const auto& it : items) {
            if (std::abs(it.cx - x) < 115.0) {
                mine.push_back(it);
            }
        }
        bool empty = false;
        for (const auto& it : mine) {
            if (to_upper_ascii(it.text).find("INFO") != std::string::npos) {
                empty = true;
                break;
            }
        }
        std::optional<int> cost;
        for (const auto& it : mine) {
            std::string t = trim_copy(it.text);
            if (it.cy < 830.0 && std::abs(it.cx - x) < 45.0 && !t.empty() && extract_digits(t) == t) {
                cost = std::stoi(t);
            }
        }
        if (!cost.has_value() && !empty) {
            cost = numrec(img, { x - 22, 780, x + 22, 822 });
        }
        bool op = false;
        for (const auto& it : mine) {
            if (it.cy > 930.0 && it.cy < 1000.0 && has_cjk(it.text)) {
                op = true;
                break;
            }
        }
        int r = 0;
        for (const auto& it : mine) {
            std::string tt = to_upper_ascii(trim_copy(it.text));
            for (char& c : tt) {
                if (c == '1' || c == 'L') {
                    c = 'I';
                }
            }
            if (it.cy < 840.0 && it.cx < x - 50 && kRoman.count(tt)) {
                r = kRoman.at(tt);
            }
        }
        bool hi = (r > 0) ? (r >= 5) : (yellow_ratio(img, { x - 104, 800, x - 78, 826 }) > 0.1);
        std::string name;
        for (const auto& it : mine) {
            if (it.cy >= 1000.0 && has_cjk_or_alpha(it.text)) {
                name = trim_copy(it.text);
            }
        }
        out.push_back(ShopCard {
            static_cast<int>(idx),
            x,
            empty,
            cost,
            hi,
            op && !empty,
            name,
            is_dps(name) });
    }
    return out;
}

bool SPAFarmTaskPlugin::buy(int i, std::optional<int> cost)
{
    int x = m_cards[i];
    auto g0 = numrec(shot(), kGoldBox);
    tap(x, kCardY, 0.8);
    cv::Mat img = shot();
    if (find(ocr(img, std::array<int, 4> { x - 120, 980, x + 120, 1060 }), { "无法购买" }).has_value()
        || find(ocr(img, std::array<int, 4> { 1400, 120, 1920, 260 }), { "资金不足" }).has_value()) {
        tap(960, 600, 0.4);
        return false;
    }
    auto left = ocr(img, std::array<int, 4> { 0, 250, 700, 620 });
    bool is_op = find(left, { "特质", "天赋", "特性" }).has_value();
    if (!is_op) {
        tap(x, kCardY, 0.4);
        tap(960, 600, 0.4);
        return false;
    }
    tap(x, kCardY + 106, 1.2);
    auto g1 = numrec(shot(), kGoldBox);
    if (cost != 0 && g0.has_value() && g1.has_value() && *g1 >= *g0) {
        tap(960, 600, 0.4);
        return false;
    }
    return true;
}

std::vector<std::pair<int, int>> SPAFarmTaskPlugin::green_tiles(const cv::Mat& img)
{
    cv::Mat hsv;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
    cv::Mat mask;
    cv::inRange(hsv, cv::Scalar(35, 100, 100), cv::Scalar(85, 255, 255), mask);
    cv::Mat flt;
    mask.convertTo(flt, CV_32F, 1.0 / 255.0);
    flt.rowRange(0, 120).setTo(0.0f);
    flt.rowRange(760, flt.rows).setTo(0.0f);
    flt.colRange(0, 180).setTo(0.0f);
    flt.colRange(1650, flt.cols).setTo(0.0f);

    cv::Mat blurred;
    cv::blur(flt, blurred, cv::Size(61, 61));

    std::vector<std::pair<int, int>> candidates;
    for (int y = 120; y < 760; ++y) {
        const float* row = blurred.ptr<float>(y);
        for (int x = 180; x < 1650; ++x) {
            if (row[x] > 0.93f) {
                candidates.emplace_back(x, y);
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;
    });

    std::vector<std::pair<int, int>> pts;
    for (auto [x, y] : candidates) {
        bool far_enough = true;
        for (auto [px, py] : pts) {
            if (std::abs(x - px) <= 105 && std::abs(y - py) <= 75) {
                far_enough = false;
                break;
            }
        }
        if (far_enough) {
            pts.emplace_back(x, y);
            if (pts.size() >= 40) {
                break;
            }
        }
    }
    return pts;
}

std::vector<std::pair<int, int>> SPAFarmTaskPlugin::right3(
    const std::vector<std::pair<int, int>>& tiles,
    const cv::Mat& img)
{
    cv::Mat hsv;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
    cv::Mat mask;
    cv::inRange(hsv, cv::Scalar(35, 100, 100), cv::Scalar(85, 255, 255), mask);
    mask.rowRange(0, 120).setTo(0);
    mask.rowRange(760, mask.rows).setTo(0);
    mask.colRange(1650, mask.cols).setTo(0);

    std::vector<std::pair<double, double>> edges;
    for (int y0 = 140; y0 < 760; y0 += 40) {
        std::vector<int> sel;
        for (int y = y0; y < std::min(y0 + 40, 760); ++y) {
            const uint8_t* row = mask.ptr<uint8_t>(y);
            for (int x = 0; x < 1650; ++x) {
                if (row[x] > 0) {
                    sel.push_back(x);
                }
            }
        }
        if (sel.size() > 200) {
            std::sort(sel.begin(), sel.end());
            size_t idx = std::min(sel.size() - 1, static_cast<size_t>(std::floor(0.995 * (sel.size() - 1))));
            edges.emplace_back(y0 + 20.0, static_cast<double>(sel[idx]));
        }
    }
    if (edges.empty()) {
        return {};
    }

    double k = 0.0;
    double c = edges.front().second;
    if (edges.size() >= 2) {
        double sum_y = 0.0, sum_x = 0.0, sum_yy = 0.0, sum_yx = 0.0;
        for (auto [ey, ex] : edges) {
            sum_y += ey;
            sum_x += ex;
            sum_yy += ey * ey;
            sum_yx += ey * ex;
        }
        double n = static_cast<double>(edges.size());
        double denom = n * sum_yy - sum_y * sum_y;
        if (std::abs(denom) > 1e-6) {
            k = (n * sum_yx - sum_y * sum_x) / denom;
            c = (sum_x - k * sum_y) / n;
        }
        double max_diff = 0.0;
        for (auto [ey, ex] : edges) {
            max_diff = std::max(max_diff, ex - (k * ey + c));
        }
        c += max_diff;
    }

    std::vector<std::pair<int, int>> keep;
    for (auto [x, y] : tiles) {
        double edge = k * y + c;
        double tile_w = 100.0 + (y - 140.0) * 0.06;
        if (x >= edge - 3.0 * tile_w) {
            keep.emplace_back(x, y);
        }
    }
    return keep;
}

void SPAFarmTaskPlugin::load_layout()
{
    // 默认内置用户提供的 tiles.json 坐标，无需额外外部文件即可直接工作
    m_layout.loaded = true;
    m_layout.safe = { { 1195, 445 }, { 1210, 548 } };
    m_layout.path = { { 1305, 345 }, { 1192, 345 }, { 1305, 445 } };
    m_layout.rest = { { 1305, 548 }, { 1318, 650 }, { 1440, 650 } };
    m_layout.lift = { 760, 650 };
}

bool SPAFarmTaskPlugin::is_green(const cv::Mat& img, std::pair<int, int> pt, int r)
{
    int x = pt.first;
    int y = pt.second;
    int x1 = std::clamp(x - r, 0, img.cols);
    int x2 = std::clamp(x + r, 0, img.cols);
    int y1 = std::clamp(y - r, 0, img.rows);
    int y2 = std::clamp(y + r, 0, img.rows);
    if (x2 <= x1 || y2 <= y1) {
        return false;
    }
    cv::Mat hsv;
    cv::cvtColor(img(cv::Rect(x1, y1, x2 - x1, y2 - y1)), hsv, cv::COLOR_BGR2HSV);
    cv::Mat mask;
    cv::inRange(hsv, cv::Scalar(35, 100, 100), cv::Scalar(85, 255, 255), mask);
    return (cv::mean(mask)[0] / 255.0) > 0.8;
}

std::string SPAFarmTaskPlugin::drag_name(const cv::Mat& img) const
{
    for (const auto& it : ocr(img, std::array<int, 4> { 0, 262, 330, 322 })) {
        if (has_cjk_or_alpha(it.text) && to_upper_ascii(it.text).find("LV") == std::string::npos) {
            return trim_copy(it.text);
        }
    }
    return "";
}

bool SPAFarmTaskPlugin::near_pt(std::pair<int, int> t, const std::vector<std::pair<int, int>>& lst, int d)
{
    for (auto u : lst) {
        if (std::abs(t.first - u.first) <= d && std::abs(t.second - u.second) <= d) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> SPAFarmTaskPlugin::tile_order(std::string_view name) const
{
    std::vector<std::string> safe, path, rest;
    for (size_t i = 0; i < m_layout.safe.size(); ++i) {
        safe.push_back("safe" + std::to_string(i));
    }
    for (size_t i = 0; i < m_layout.path.size(); ++i) {
        path.push_back("path" + std::to_string(i));
    }
    for (size_t i = 0; i < m_layout.rest.size(); ++i) {
        rest.push_back("rest" + std::to_string(i));
    }
    std::vector<std::string> out;
    if (is_safe_op(name)) {
        out.insert(out.end(), safe.begin(), safe.end());
        out.insert(out.end(), path.begin(), path.end());
        out.insert(out.end(), rest.begin(), rest.end());
    }
    else {
        out.insert(out.end(), path.begin(), path.end());
        out.insert(out.end(), rest.begin(), rest.end());
        out.insert(out.end(), safe.begin(), safe.end());
    }
    return out;
}

std::pair<int, int> SPAFarmTaskPlugin::tile_xy(std::string_view tn) const
{
    if (tn.rfind("safe", 0) == 0) {
        return m_layout.safe.at(std::stoul(std::string(tn.substr(4))));
    }
    if (tn.rfind("path", 0) == 0) {
        return m_layout.path.at(std::stoul(std::string(tn.substr(4))));
    }
    return m_layout.rest.at(std::stoul(std::string(tn.substr(4))));
}

std::optional<std::pair<int, int>> SPAFarmTaskPlugin::deploy_from_bench(
    std::pair<int, int> bench_xy,
    bool quiet,
    Board* board,
    bool hi)
{
    if (m_layout.loaded && board != nullptr) {
        return deploy_layout(bench_xy, *board, "", hi ? 5 : 0, 0);
    }
    return deploy_from_bench_old(bench_xy, quiet, board, hi);
}

std::optional<std::pair<int, int>> SPAFarmTaskPlugin::deploy_from_bench_old(
    std::pair<int, int> bench_xy,
    bool /*quiet*/,
    Board* board,
    bool hi)
{
    auto [bx, by] = bench_xy;
    cv::Mat before = shot();
    auto left0 = slots_left(before);
    for (int attempt = 0; attempt < 3; ++attempt) {
        // 原实现：先在备战席按下并竖直上移 180px 把干员「拿起」，
        // 只有拖拽态下游戏才会显示可部署绿格，静止截图里是找不到的。
        touch_event(static_cast<int>(InputEvent::Type::TOUCH_DOWN), bx, by);
        std::this_thread::sleep_for(std::chrono::milliseconds(DragDownHoldMs));
        drag_path(bx, by, bx, by - 180, 9);
        sleep_sec(0.8);
        cv::Mat img = shot();
        auto cand = right3(green_tiles(img), img);
        if (cand.empty()) {
            touch_event(static_cast<int>(InputEvent::Type::TOUCH_UP), bx, by - 180);
            sleep_sec(0.8);
            return std::nullopt;
        }
        auto [tx, ty] = cand[std::min<size_t>(attempt, cand.size() - 1)];
        drag_path(bx, by - 180, tx, ty, 10);
        touch_event(static_cast<int>(InputEvent::Type::TOUCH_UP), tx, ty);
        sleep_sec(1.0);
        cv::Mat im = shot();
        if (find(ocr(im, std::array<int, 4> { 300, 0, 1700, 820 }), { "点击取消", "取消" }).has_value()) {
            m_last_tile = { tx, ty };
            set_facing(tx, ty);
            if (board) {
                board->used.emplace_back(tx, ty);
                if (hi) {
                    board->hi_tiles.emplace_back(tx, ty);
                }
            }
            return std::pair<int, int> { tx, ty };
        }
    }
    return std::nullopt;
}

std::optional<std::pair<int, int>> SPAFarmTaskPlugin::deploy_layout(
    std::pair<int, int> bench_xy,
    Board& board,
    std::string name_hint,
    int tier_hint,
    int depth)
{
    auto [bx, by] = bench_xy;
    cv::Mat before = shot();
    auto left0 = slots_left(before);

    // 点击备战席干员读取左侧干员面板信息
    tap(bx, by, 0.5);
    cv::Mat img = shot();
    std::vector<std::string> tabs;
    for (const auto& it : ocr(img, std::array<int, 4> { 0, 520, 700, 600 })) {
        std::string t = trim_copy(it.text);
        if (t == "特质" || t == "技能" || t == "特性" || t == "天赋") {
            tabs.push_back(t);
        }
    }
    std::string dname = drag_name(img);
    bool picked = !tabs.empty()
        || !dname.empty()
        || find(ocr(img, std::array<int, 4> { 0, 262, 330, 380 }), { "LV" }).has_value();
    if (!picked) {
        tap(960, 100, 0.3);
        return std::nullopt;
    }
    bool has_op_tab = false;
    for (const auto& t : tabs) {
        if (t == "特质" || t == "天赋") {
            has_op_tab = true;
            break;
        }
    }
    if (!tabs.empty() && !has_op_tab) {
        tap(960, 100, 0.3);
        return std::nullopt;
    }

    std::string name = !dname.empty() ? dname : name_hint;
    int tier = (yellow_ratio(img, { 20, 328, 58, 362 }) > 0.08) ? 5 : 1;
    if (prio_index(name).has_value()) {
        tier = std::max(tier, 5);
    }
    if (depth >= 1 && tier < 5) {
        sell_at(bx, by);
        return std::nullopt;
    }
    tier = std::max(tier, (depth == 0) ? tier_hint : 0);
    int my = rank(name, tier);
    auto order = tile_order(name);

    auto occupant = [&](const std::string& tn) -> std::optional<std::pair<std::string, int>> {
        if (board.occ.count(tn)) {
            return std::pair<std::string, int> { board.occ[tn].name, board.occ[tn].tier };
        }
        return std::nullopt;
    };

    auto value = [&](const std::string& tn, const std::pair<std::string, int>& occ) -> int {
        int r = rank(occ.first, occ.second);
        if (tn.rfind("safe", 0) == 0 && is_safe_op(name) && !is_safe_op(occ.first)) {
            r -= 2000;
        }
        return r;
    };

    std::vector<std::string> safe, other;
    for (const auto& t : order) {
        if (t.rfind("safe", 0) == 0) {
            safe.push_back(t);
        }
        else {
            other.push_back(t);
        }
    }
    std::vector<std::vector<std::string>> groups = is_safe_op(name)
        ? std::vector<std::vector<std::string>> { safe, other }
        : std::vector<std::vector<std::string>> { other, safe };

    std::string target;
    std::optional<std::pair<std::string, int>> swap_out;

    for (const auto& grp : groups) {
        std::vector<std::string> free_tiles;
        for (const auto& tn : grp) {
            if (!occupant(tn).has_value()) {
                free_tiles.push_back(tn);
            }
        }
        if (!free_tiles.empty()) {
            target = free_tiles.front();
            break;
        }
        struct Cand
        {
            int val;
            size_t idx;
            std::string tn;
            std::pair<std::string, int> occ;
        };
        std::vector<Cand> cands;
        for (size_t idx = 0; idx < grp.size(); ++idx) {
            const auto& tn = grp[idx];
            auto occ = occupant(tn);
            if (occ.has_value()) {
                int v = value(tn, *occ);
                if (occ->second < 5 || (occ->first != "?" && v < my)) {
                    cands.push_back(Cand { v, idx, tn, *occ });
                }
            }
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            return (a.val != b.val) ? (a.val < b.val) : (a.idx < b.idx);
        });
        if (!cands.empty()) {
            target = cands.front().tn;
            swap_out = cands.front().occ;
            break;
        }
    }

    if (target.empty()) {
        tap(960, 100, 0.3);
        return std::nullopt;
    }

    auto [tx, ty] = tile_xy(target);
    // 原实现 deploy_layout 的三段式拖拽：
    //   按下 -> 竖直上移 120px「拿起」-> 移到 lift(760,650) 不挡目标格 -> 停顿
    //   -> 从 lift 拖到目标格 -> 在目标格松手
    // 一步到位的 swipe 容易被游戏判成「滑动」而不是「按住拖拽」，干员拿不起来。
    touch_event(static_cast<int>(InputEvent::Type::TOUCH_DOWN), bx, by);
    std::this_thread::sleep_for(std::chrono::milliseconds(DragDownHoldMs));
    drag_path(bx, by, bx, by - 120, 6);
    drag_path(bx, by - 120, m_layout.lift.first, m_layout.lift.second, 8);
    sleep_sec(0.8);
    drag_path(m_layout.lift.first, m_layout.lift.second, tx, ty, 10);
    touch_event(static_cast<int>(InputEvent::Type::TOUCH_UP), tx, ty);
    sleep_sec(1.0);
    if (!find(ocr(shot(), std::array<int, 4> { 300, 0, 1700, 820 }), { "点击取消", "取消" }).has_value()) {
        return std::nullopt;
    }
    m_last_tile = { tx, ty };
    set_facing(tx, ty);
    cv::Mat after = shot();
    board.occ[target] = OccupantInfo { name, tier, false };
    auto left1 = slots_left(after);
    if (!swap_out.has_value() && left0.has_value() && left1.has_value() && *left1 >= *left0) {
        swap_out = std::pair<std::string, int> { "?", 5 };
    }
    if (swap_out.has_value()) {
        board.pending = true;
        if (auto out = new_bench_slot(before, after)) {
            if (swap_out->second < 5) {
                sell_at(out->first, out->second);
            }
            else if (depth == 0) {
                deploy_layout(*out, board, swap_out->first, swap_out->second, depth + 1);
            }
        }
    }
    m_last_tier = tier;
    m_last_name = name;
    m_last_tile = { tx, ty };
    return std::pair<int, int> { tx, ty };
}

bool SPAFarmTaskPlugin::sell_at(int x, int y)
{
    auto g0 = numrec(shot(), kGoldBox);
    // 原实现：两段拖拽（先上移 120px 拿起，再拖到右下角出售区），松手后比对资金
    touch_event(static_cast<int>(InputEvent::Type::TOUCH_DOWN), x, y);
    std::this_thread::sleep_for(std::chrono::milliseconds(DragDownHoldMs));
    drag_path(x, y, x, y - 120, 6);
    drag_path(x, y - 120, kSellPos.first, kSellPos.second, 10);
    sleep_sec(0.4);
    touch_event(static_cast<int>(InputEvent::Type::TOUCH_UP), kSellPos.first, kSellPos.second);
    sleep_sec(1.0);
    auto g1 = numrec(shot(), kGoldBox);
    return g0.has_value() && g1.has_value() && *g1 > *g0;
}

bool SPAFarmTaskPlugin::in_facing_mode(const cv::Mat* img) const
{
    cv::Mat cur = (img != nullptr) ? *img : shot();
    return find(ocr(cur, std::array<int, 4> { 300, 0, 1700, 820 }), { "点击取消" }).has_value();
}

bool SPAFarmTaskPlugin::set_facing(int tx, int ty, int tries)
{
    for (int k = 0; k < tries; ++k) {
        int dist = 300 + 60 * k;
        // 原实现：慢速横向拖拽设置朝向，松手后确认「点击取消」消失
        touch_event(static_cast<int>(InputEvent::Type::TOUCH_DOWN), tx, ty);
        std::this_thread::sleep_for(std::chrono::milliseconds(DragDownHoldMs));
        drag_path(tx, ty, tx + dist, ty, 10);
        sleep_sec(0.3);
        touch_event(static_cast<int>(InputEvent::Type::TOUCH_UP), tx + dist, ty);
        sleep_sec(1.2);
        if (!in_facing_mode()) {
            return true;
        }
    }
    return false;
}

std::optional<int> SPAFarmTaskPlugin::slots_left(const cv::Mat& img) const
{
    if (auto n = numrec(img, { 1862, 566, 1897, 607 })) {
        return *n % 10;
    }
    return std::nullopt;
}

std::optional<std::pair<int, int>> SPAFarmTaskPlugin::new_bench_slot(
    const cv::Mat& before,
    const cv::Mat& after)
{
    cv::Mat d;
    cv::absdiff(
        before(cv::Rect(200, 560, 1300, 210)),
        after(cv::Rect(200, 560, 1300, 210)),
        d);
    cv::Mat g, m;
    cv::cvtColor(d, g, cv::COLOR_BGR2GRAY);
    cv::threshold(g, m, 40, 255, cv::THRESH_BINARY);
    cv::morphologyEx(m, m, cv::MORPH_CLOSE, cv::Mat::ones(15, 15, CV_8U));
    cv::Mat labels, stats, centroids;
    int n = cv::connectedComponentsWithStats(m, labels, stats, centroids);
    if (n <= 1) {
        return std::nullopt;
    }
    int best_k = 1;
    int best_area = stats.at<int>(1, cv::CC_STAT_AREA);
    for (int k = 2; k < n; ++k) {
        int area = stats.at<int>(k, cv::CC_STAT_AREA);
        if (area > best_area) {
            best_area = area;
            best_k = k;
        }
    }
    return std::pair<int, int> {
        static_cast<int>(centroids.at<double>(best_k, 0)) + 200,
        static_cast<int>(centroids.at<double>(best_k, 1)) + 560
    };
}

bool SPAFarmTaskPlugin::buy_and_deploy(Board& board, int i, int tier, std::optional<int> cost)
{
    cv::Mat before = shot();
    if (!buy(i, cost)) {
        return false;
    }
    cv::Mat after = shot();
    auto slot = new_bench_slot(before, after);
    if (!slot.has_value()) {
        handle_popups();
        board.pending = true;
        return true;
    }
    if (auto pos = deploy_from_bench(*slot, false, &board, tier >= 5)) {
        board.ops.push_back(BoardOp {
            pos->first,
            pos->second,
            std::max(tier, m_last_tier),
            m_last_tile.value_or(*pos) });
    }
    else {
        board.pending = true;
    }
    return true;
}

void SPAFarmTaskPlugin::deploy_bench(Board& board)
{
    for (int pass = 0; pass < 6; ++pass) {
        bool placed = false;
        for (const auto& row : kBenchRows) {
            int empty = 0;
            for (auto xy : row) {
                if (auto pos = deploy_from_bench(xy, true, &board, false)) {
                    board.ops.push_back(BoardOp {
                        pos->first,
                        pos->second,
                        m_last_tier,
                        m_last_tile.value_or(*pos) });
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

bool SPAFarmTaskPlugin::handle_popups()
{
    cv::Mat img = shot();
    auto items = ocr(img);
    if (find(items, { "确认准备", "资金将在" }).has_value()) {
        tap(860, 602, 0.5);
        tap(1410, 728, 1.5);
        return true;
    }
    return false;
}

std::optional<int> SPAFarmTaskPlugin::read_gold(const cv::Mat* img, int tries)
{
    for (int k = 0; k < tries; ++k) {
        cv::Mat im = (k == 0 && img != nullptr) ? *img : shot();
        auto g = numrec(im, kGoldBox);
        if (!g.has_value()) {
            g = num(im, { 1760, 760, 1880, 825 });
        }
        if (g.has_value() && *g >= 0 && *g <= 99) {
            return g;
        }
        sleep_sec(0.4);
    }
    return std::nullopt;
}

std::optional<int> SPAFarmTaskPlugin::gold_now()
{
    auto g = read_gold();
    if (!g.has_value()) {
        ++m_gold_fail;
    }
    return g;
}

void SPAFarmTaskPlugin::prep_phase(Board& board, const cv::Mat& img)
{
    auto [gold, up_cost, maxed, rnd_opt] = read_shop(img);
    int rnd = 0;
    if (!rnd_opt.has_value()) {
        if (board.prep_done) {
            return;
        }
        rnd = std::max(1, board.round_done + 1);
    }
    else {
        rnd = *rnd_opt;
    }
    if (rnd == board.round_done) {
        return;
    }

    if (board.round_done < 0 && rnd == 1) {
        int bought = 0;
        for (const auto& c : shop_cards(img)) {
            auto g = gold_now();
            if (bought >= 2) {
                break;
            }
            if (c.op && c.cost.has_value() && (!g.has_value() || *c.cost <= *g)) {
                if (is_healer(c.name) && board.healers >= 1) {
                    continue;
                }
                if (buy_and_deploy(board, c.i, c.hi ? 5 : 1, c.cost)) {
                    ++bought;
                    if (is_healer(c.name)) {
                        ++board.healers;
                    }
                }
            }
        }
    }

    if (!maxed) {
        for (int u = 0; u < 4; ++u) {
            cv::Mat cur = shot();
            auto [g_cur, c_cur, m_cur, _] = read_shop(cur);
            maxed = m_cur;
            if (maxed || (g_cur.has_value() && c_cur.has_value() && *g_cur < *c_cur)) {
                break;
            }
            int ux0 = m_upx;
            tap(m_upx, 920, 0.6);
            tap(m_upx, 920, 1.0);
            auto [g2, c2, m2, __] = read_shop(shot());
            maxed = m2;
            bool ok = maxed
                || (m_upx != ux0)
                || (g_cur.has_value() && g2.has_value() && *g2 < *g_cur)
                || (c_cur.has_value() && c2.has_value() && *c2 != *c_cur);
            if (!ok) {
                break;
            }
        }

        if (!maxed) {
            for (const auto& c : shop_cards(shot())) {
                cv::Mat im = shot();
                auto g = read_gold(&im);
                auto sl = slots_left(im);
                if ((g.has_value() && *g == 0) || (sl.has_value() && *sl == 0)) {
                    break;
                }
                if (c.op && c.cost.has_value() && (!g.has_value() || *c.cost <= *g)) {
                    if (is_healer(c.name) && board.healers >= 1) {
                        continue;
                    }
                    if (buy_and_deploy(board, c.i, c.hi ? 5 : 1, c.cost)) {
                        if (is_healer(c.name)) {
                            ++board.healers;
                        }
                    }
                }
            }
        }
    }

    if (maxed) {
        int refreshes = 0;
        int same_count = 0;
        std::vector<std::pair<std::string, std::optional<int>>> last_sig;
        bool has_last_sig = false;

        for (int step = 0; step < 40 && !need_exit(); ++step) {
            cv::Mat cur = shot();
            auto g = read_gold(&cur);
            bool got = false;
            auto allc = shop_cards(cur);

            std::vector<std::pair<std::string, std::optional<int>>> sig;
            for (const auto& c : allc) {
                sig.emplace_back(c.name, c.cost);
            }
            if (has_last_sig) {
                if (sig == last_sig) {
                    ++same_count;
                    if (same_count >= 2) {
                        break;
                    }
                }
                else {
                    same_count = 0;
                }
            }
            last_sig = sig;
            has_last_sig = true;

            int gold_v = g.value_or(99);
            std::vector<ShopCard> free_cards;
            std::vector<ShopCard> cards;
            for (const auto& c : allc) {
                if (c.op && c.cost.has_value() && *c.cost == 0) {
                    free_cards.push_back(c);
                }
                if (c.hi && c.op && c.cost.has_value() && *c.cost <= gold_v) {
                    cards.push_back(c);
                }
            }

            auto is_owned = [&](const ShopCard& c) {
                for (const auto& [k, cnt] : board.owned) {
                    if (!k.empty() && !c.name.empty()
                        && (k.find(c.name) != std::string::npos || c.name.find(k) != std::string::npos)) {
                        return true;
                    }
                }
                return false;
            };

            std::vector<ShopCard> prio;
            for (const auto& c : cards) {
                if (prio_index(c.name).has_value()) {
                    prio.push_back(c);
                }
            }
            std::sort(prio.begin(), prio.end(), [&](const ShopCard& a, const ShopCard& b) {
                return *prio_index(a.name) < *prio_index(b.name);
            });

            std::vector<ShopCard> pick;
            if (!free_cards.empty()) {
                std::sort(free_cards.begin(), free_cards.end(), [&](const ShopCard& a, const ShopCard& b) {
                    return rank(a.name, a.hi ? 5 : 1) > rank(b.name, b.hi ? 5 : 1);
                });
                pick.push_back(free_cards.front());
            }
            else if (!prio.empty()) {
                pick = prio;
            }
            else {
                for (const auto& c : cards) {
                    if (is_owned(c)) {
                        pick.push_back(c);
                    }
                }
                if (pick.empty()) {
                    for (const auto& c : cards) {
                        if (c.dps) {
                            pick.push_back(c);
                        }
                    }
                }
                if (pick.empty() && g.has_value() && (*g - 1) < 4) {
                    pick = cards;
                }
            }

            bool failed = false;
            for (const auto& c : pick) {
                if (buy_and_deploy(board, c.i, c.hi ? 5 : 1, c.cost)) {
                    board.owned[c.name] += 1;
                    got = true;
                    break;
                }
                else {
                    failed = true;
                    break;
                }
            }
            if (failed) {
                break;
            }
            if (got) {
                has_last_sig = false;
                continue;
            }
            if (g.has_value() && *g < 4) {
                break;
            }
            ++refreshes;
            tap(kRefreshPos.first, kRefreshPos.second, 1.0);
        }
    }

    if (board.pending) {
        deploy_bench(board);
        board.pending = false;
    }
    board.round_done = rnd;
    board.prep_done = true;
    sleep_sec(1.5);
    tap(kReadyPos.first, kReadyPos.second, 1.5);
    board.last_ready = now_sec();
    handle_popups();
}

bool SPAFarmTaskPlugin::_run()
{
    LogTraceFunction;
    apply_profile(m_profile);
    load_layout();

    bool ok_start = false;
    for (int i = 0; i < 3 && !need_exit(); ++i) {
        std::string all_txt;
        for (const auto& it : ocr(shot())) {
            all_txt += it.text;
        }
        if (all_txt.find("极速模拟") != std::string::npos || all_txt.find("断线重连") != std::string::npos) {
            ok_start = true;
            break;
        }
        sleep_sec(2.0);
    }
    if (!ok_start) {
        log_info("当前不是卫戍协议极速模拟界面，任务停止");
        return false;
    }

    std::set<std::string> done;
    Board board;
    double last_seen = now_sec();
    double last_progress = now_sec();
    int last_round = -999;
    int runs = 0;
    int attempts = 0;
    std::optional<std::string> current;

    while (runs < m_runs && !need_exit()) {
        cv::Mat img = shot();
        auto items = ocr(img);
        std::string txt;
        for (const auto& it : items) {
            txt += it.text;
        }
        std::string txt_upper = to_upper_ascii(txt);

        if (now_sec() - last_progress > kStallTimeoutSec) {
            log_info("超过 10 分钟无进展，判定卡死，停止运行");
            break;
        }

        bool seen_garrison = false;
        for (const auto& k : kGarrisonKeywords) {
            if (txt_upper.find(k) != std::string::npos) {
                seen_garrison = true;
                break;
            }
        }
        if (seen_garrison) {
            last_seen = now_sec();
        }
        else if (now_sec() - last_seen > kLeaveTimeoutSec) {
            log_info("连续 30 秒未检测到卫戍协议界面，停止运行");
            break;
        }

        if (txt.find("点击取消") != std::string::npos && txt.find("准备就绪") != std::string::npos) {
            if (m_last_tile.has_value()) {
                set_facing(m_last_tile->first, m_last_tile->second);
            }
            else if (auto hit = find(items, { "点击取消" })) {
                tap(hit->cx, hit->cy, 1.0);
            }
            continue;
        }

        if (txt.find("返回主页") != std::string::npos) {
            bool ok = (txt.find("模拟完成") != std::string::npos);
            if (ok && current.has_value()) {
                done.insert(*current);
            }
            if (ok) {
                ++runs;
            }
            ++attempts;
            log_info(
                std::string(ok ? "本局通关" : "本局失败")
                + "，已通关 " + std::to_string(runs) + "/" + std::to_string(m_runs)
                + "，总尝试 " + std::to_string(attempts));
            current.reset();
            last_progress = now_sec();
            if (auto hit = find(items, { "返回主页" })) {
                tap(hit->cx, hit->cy, 4.0);
            }
            board = Board();
            if (runs >= m_runs || (m_attempts > 0 && attempts >= m_attempts)) {
                break;
            }
        }
        else if (txt.find("请选择模拟难度") != std::string::npos
                 || (txt.find("开始模拟") != std::string::npos
                     && txt.find("极速模拟") != std::string::npos
                     && txt.find("回合") == std::string::npos)) {
            auto hit = find(items, { "开始模拟" });
            tap(hit ? hit->cx : 1700.0, hit ? hit->cy : 986.0, 3.0);
            board = Board();
        }
        else if (txt.find("确认准备") != std::string::npos || txt.find("资金将在") != std::string::npos) {
            tap(860, 602, 0.5);
            tap(1410, 728, 2.0);
        }
        else if (txt.find("断线重连") != std::string::npos) {
            auto hit = find(items, { "断线重连" });
            tap(hit ? hit->cx : 1660.0, hit ? hit->cy : 970.0, 5.0);
            board = Board();
        }
        else if (txt.find("独立模拟") != std::string::npos && txt.find("极速模拟") != std::string::npos) {
            auto hit = find(items, { "极速模拟" });
            tap(hit ? hit->cx : 1280.0, hit ? hit->cy : 1000.0, 3.0);
        }
        else if (txt.find("确认本局信息") != std::string::npos) {
            tap(1686, 986, 3.0);
        }
        else if (txt.find("选择策略") != std::string::npos) {
            last_progress = now_sec();
            current = pick_strategy(done);
            if (!current.has_value()) {
                break;
            }
            tap(1680, 990, 3.0);
        }
        else if (txt.find("确定自选编队") != std::string::npos) {
            pick_squad();
        }
        else if (txt.find("回合") != std::string::npos
                 && txt.find("准备就绪") != std::string::npos
                 && (txt.find("刷新") != std::string::npos
                     || txt.find("升级") != std::string::npos
                     || txt.find("满级") != std::string::npos)) {
            prep_phase(board, img);
            if (board.prep_done && (now_sec() - board.last_ready) > 10.0) {
                tap(kReadyPos.first, kReadyPos.second, 1.5);
                handle_popups();
                board.last_ready = now_sec();
            }
            if (board.round_done != last_round) {
                last_round = board.round_done;
                last_progress = now_sec();
            }
            continue;
        }
        else if (txt.find("回合") != std::string::npos
                 || (txt.find("查看") != std::string::npos && txt.find("极速模拟") != std::string::npos)
                 || txt_upper.find("COUNTDOWN") != std::string::npos) {
            board.prep_done = false;
            sleep_sec(4.0);
            continue;
        }
        else {
            if (seen_garrison) {
                for (const auto& gkw : kGenericKeywords) {
                    if (auto hit = find(items, { gkw })) {
                        tap(hit->cx, hit->cy, 2.0);
                        break;
                    }
                }
            }
            sleep_sec(2.0);
            continue;
        }
        sleep_sec(1.0);
    }

    return true;
}