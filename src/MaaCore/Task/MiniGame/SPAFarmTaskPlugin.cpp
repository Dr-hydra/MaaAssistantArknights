#include "SPAFarmTaskPlugin.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <numeric>
#include <queue>
#include <random>
#include <sstream>
#include <thread>

#include "Config/GeneralConfig.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Config/Miscellaneous/OcrPack.h"
#include "Config/ResourceLoader.h"
#include "Controller/Controller.h"
#include "Status.h"
#include "Utils/Logger.hpp"

namespace asst
{

namespace
{

constexpr double kLeaveTimeoutSec = 30.0;
constexpr double kStallTimeoutSec = 600.0;

// Authoritative medal stackers (`ht` in service.pretty.mjs lines 7855-7869) + quick stackers
const std::vector<std::string> kQuickStackers = {
    "伺夜", "空弦", "黑键", "雪猎", "旅骨", "霍尔海雅",
    "迷迭香", "鸿雪", "Miss.Christine", "溯光星源", "逻各斯", "夕",
    "巡林者", "克洛丝", "安德切尔", "蓝毒", "守林人", "寒芒克洛丝",
    "史都华德", "远山", "天火", "夜烟", "莫斯提马", "特米米"
};

// Authoritative medal finishers (`er` in service.pretty.mjs lines 7870-7875) + quick finishers
const std::vector<std::string> kQuickFinishers = {
    "蕾缪安", "塔露拉", "艾雅法拉", "维伊",
    "维什戴尔", "逻各斯", "迷迭香", "澄闪"
};

const std::vector<std::string> kAuxiliaryNames = {
    "幻影", "Touch", "预备干员医疗", "机动盾牌", "工程蓄水炮", "樱桃三号", "结构性原理"
};

const std::vector<std::pair<std::string, std::string>> kPriorityFull = {
    { "塔露拉", "塔露拉" },     { "维什戴尔", "维什戴尔" }, { "星熊", "斩业星熊" },
    { "安洁莉娜", "予愿安洁莉娜" }, { "能天使", "新约能天使" }, { "凯尔希", "凯尔希·思衡托" },
    { "望", "望" },             { "初雪", "圣聆初雪" },     { "逻各斯", "逻各斯" },
    { "蕾缪安", "蕾缪安" },     { "贝洛内", "贝洛内" },     { "冻河", "冻河" },
    { "明霄陈", "赤刃明霄陈" }, { "结城理", "结城理" }
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

// Exact 1:1 port of `pr` in service.pretty.mjs lines 7883-7912
struct GoalTaskSpec
{
    std::string id;
    std::vector<std::string> bonds;
    int layers = 100;
    std::vector<std::string> strategies;
};

const std::vector<GoalTaskSpec> kGoalTasks = {
    { "task-1", { "炎", "莱塔尼亚", "奥术" }, 100, { "杰西卡" } },
    { "task-2", { "阿戈尔", "卡西米尔", "突袭" }, 100, { "杰西卡" } },
    { "task-3", { "叙拉古", "萨尔贡", "精准" }, 100, { "杰西卡" } },
    { "task-4", { "维多利亚", "谢拉格", "灵巧" }, 100, { "杰西卡" } },
    { "task-5", { "乌萨斯", "伊比利亚", "聚合" }, 100, { "杰西卡" } },
    { "task-6", { "奇迹", "远见", "投资人" }, 50, { "杰西卡" } },
    { "task-7", { "迅捷", "不屈", "坚守", "助力" }, 50, { "杰西卡" } }
};

double now_sec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::vector<std::string> split_utf8_chars(std::string_view s)
{
    std::vector<std::string> out;
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
        out.emplace_back(s.substr(i, len));
        i += len;
    }
    return out;
}

bool is_cjk_or_alnum_cp(std::string_view ch)
{
    if (ch.size() == 1) {
        unsigned char c = static_cast<unsigned char>(ch[0]);
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }
    if (ch.size() == 3) {
        unsigned char b0 = static_cast<unsigned char>(ch[0]);
        unsigned char b1 = static_cast<unsigned char>(ch[1]);
        unsigned char b2 = static_cast<unsigned char>(ch[2]);
        uint32_t cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
        return cp >= 0x4E00 && cp <= 0x9FFF;
    }
    return false;
}

std::string normalize_variants(std::string_view s)
{
    static const std::vector<std::pair<std::string_view, std::string_view>> kVariants = {
        { "衛", "卫" }, { "戍", "戍" }, { "協", "协" }, { "議", "议" }, { "極", "极" },
        { "擬", "拟" }, { "獨", "独" }, { "選", "选" }, { "擇", "择" }, { "略", "略" },
        { "確", "确" }, { "認", "认" }, { "準", "准" }, { "備", "备" }, { "編", "编" },
        { "隊", "队" }, { "級", "级" }, { "滿", "满" }, { "購", "购" }, { "買", "买" },
        { "賣", "卖" }, { "資", "资" }, { "金", "金" }, { "刷", "刷" }, { "新", "新" },
        { "凍", "冻" }, { "結", "结" }, { "返", "返" }, { "頁", "页" }, { "斷", "断" },
        { "線", "线" }, { "連", "连" }, { "繼", "继" }, { "續", "续" }, { "離", "离" },
        { "開", "开" }, { "關", "关" }, { "閉", "闭" }, { "決", "决" }, { "策", "策" },
        { "戰", "战" }, { "術", "术" }, { "獎", "奖" }, { "勵", "励" }, { "獲", "获" },
        { "裝", "装" }, { "穿", "穿" }, { "戴", "戴" }, { "領", "领" }, { "袖", "袖" },
        { "敵", "敌" }, { "訓", "训" }, { "維", "维" }, { "爾", "尔" }, { "邏", "逻" },
        { "絲", "丝" }, { "藍", "蓝" }, { "遠", "远" }, { "煙", "烟" }, { "閃", "闪" },
        { "潔", "洁" }, { "麗", "丽" }, { "凱", "凯" }, { "聖", "圣" }, { "聆", "聆" },
        { "蕾", "蕾" }, { "繆", "缪" }, { "貝", "贝" }, { "陳", "陈" }, { "鋒", "锋" },
        { "術", "术" }, { "醫", "医" }, { "療", "疗" }, { "護", "护" }, { "堅", "坚" },
        { "強", "强" }, { "精", "精" }, { "奧", "奥" }, { "靈", "灵" }, { "跡", "迹" },
        { "辭", "辞" }, { "歲", "岁" }, { "榮", "荣" }, { "薩", "萨" }, { "貢", "贡" },
        { "謝", "谢" }, { "格", "格" }, { "蘭", "兰" }
    };
    std::string out(s);
    for (const auto& [from, to] : kVariants) {
        size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    return out;
}

std::string extract_digits_mapped(std::string_view s)
{
    std::string out;
    auto chars = split_utf8_chars(s);
    for (const auto& ch : chars) {
        if (ch.size() == 1) {
            char c = ch[0];
            if (c >= '0' && c <= '9') {
                out.push_back(c);
            }
            else if (c == 'O' || c == 'o') {
                out.push_back('0');
            }
        }
        else if (ch == "Ｏ" || ch == "〇") {
            out.push_back('0');
        }
    }
    return out;
}

} // namespace

// ============================================================================
// Public Entry & Configuration
// ============================================================================

bool SPAFarmTaskPlugin::verify(AsstMsg msg, const json::value& details) const
{
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }
    const std::string& task = details.get("details", "task", std::string());
    return task.ends_with("MiniGame@SPA@BadgeFarm");
}

bool SPAFarmTaskPlugin::set_params(const json::value& params)
{
    if (auto p = params.find<std::string>("profile"); p) {
        m_profile = (*p == "low") ? "low" : "full";
    }
    else if (auto lp = params.find<bool>("low_profile"); lp) {
        m_profile = *lp ? "low" : "full";
    }
    if (auto g = params.find<std::string>("goal"); g && !g->empty()) {
        m_goal = *g;
    }
    int target_val = params.get("target", params.get("target_clears", 3));
    m_target = (target_val >= 6) ? 6 : 3;
    m_runs = std::max(1, params.get("runs", params.get("max_runs", 20)));
    m_attempts = std::max(0, params.get("attempts", 0));
    return true;
}

void SPAFarmTaskPlugin::set_profile(bool low_profile)
{
    m_profile = low_profile ? "low" : "full";
}

void SPAFarmTaskPlugin::set_target_clears(int target_clears)
{
    m_target = (target_clears >= 6) ? 6 : 3;
}

void SPAFarmTaskPlugin::set_max_runs(int max_runs)
{
    m_runs = std::max(1, max_runs);
}

void SPAFarmTaskPlugin::reset_match_state()
{
    m_match_info_read = false;
    m_stage_id = "act3autochess_m01";
    m_stage_source = "default";
    m_stage_vote = { "", 0 };
    m_current_round = 0;
    m_last_prep_round = -1;
    m_current_shop_level = 1;
    m_current_gold = 0;
    m_deploy_cap = 8;
    m_upgrade_x_Ref.reset();
    m_units.clear();
    m_shop.clear();
    m_remembered_units.clear();
    m_panel_verified_slots.clear();
    m_op_hist_bank.clear();
    m_empty_cell_refs.clear();
    m_recent_bought_names.clear();
    m_bought_items.clear();
    m_current_boss.clear();
    m_equip_slots.clear();
    m_unit_equipped_bond.clear();
    m_unit_equipped_count.clear();
    m_mob_used_this_round = false;
    m_pager_used_round = -1;
    m_active_core_bonds = { "精准", "奥术" };
}

// ============================================================================
// Low-level Controller & Timing
// ============================================================================

Point SPAFarmTaskPlugin::to_720p(double x_1080, double y_1080)
{
    int x = static_cast<int>(std::lround(std::clamp(x_1080, 0.0, 1919.0) * 1280.0 / 1920.0));
    int y = static_cast<int>(std::lround(std::clamp(y_1080, 0.0, 1079.0) * 720.0 / 1080.0));
    return Point(x, y);
}

cv::Mat SPAFarmTaskPlugin::shot() const
{
    for (int i = 0; i < 5; ++i) {
        cv::Mat raw = ctrler()->get_image();
        if (raw.empty()) {
            continue;
        }
        cv::Mat bgr;
        if (raw.channels() == 4) {
            cv::cvtColor(raw, bgr, cv::COLOR_BGRA2BGR);
        }
        else if (raw.channels() == 1) {
            cv::cvtColor(raw, bgr, cv::COLOR_GRAY2BGR);
        }
        else {
            bgr = raw;
        }
        if (bgr.cols != 1920 || bgr.rows != 1080) {
            cv::Mat resized;
            cv::resize(bgr, resized, cv::Size(1920, 1080), 0.0, 0.0, cv::INTER_CUBIC);
            return resized;
        }
        return bgr.clone();
    }
    return {};
}

bool SPAFarmTaskPlugin::sleep_ms(int ms)
{
    if (ms <= 0) {
        return !need_exit();
    }
    return sleep(static_cast<unsigned int>(ms));
}

bool SPAFarmTaskPlugin::sleep_sec(double sec)
{
    if (sec <= 0.0) {
        return !need_exit();
    }
    return sleep(static_cast<unsigned int>(std::lround(sec * 1000.0)));
}

void SPAFarmTaskPlugin::tap(double x, double y, int wait_ms)
{
    ctrler()->click(to_720p(x, y));
    sleep_ms(wait_ms);
}

void SPAFarmTaskPlugin::swipe(double x1, double y1, double x2, double y2, int duration_ms, int wait_ms)
{
    Point p1 = to_720p(x1, y1);
    Point p2 = to_720p(x2, y2);
    ctrler()->swipe(Rect(p1.x, p1.y, 1, 1), Rect(p2.x, p2.y, 1, 1), duration_ms);
    sleep_ms(wait_ms);
}

void SPAFarmTaskPlugin::touch_event(int type, double x, double y) const
{
    InputEvent ev;
    ev.type = static_cast<InputEvent::Type>(type);
    ev.pointerId = 0;
    ev.point = to_720p(x, y);
    ctrler()->inject_input_event(ev);
}

void SPAFarmTaskPlugin::drag_segments(
    double x1,
    double y1,
    double x2,
    double y2,
    int steps,
    int step_ms,
    int hold_ms)
{
    steps = std::max(1, steps);
    touch_event(static_cast<int>(InputEvent::Type::TOUCH_DOWN), x1, y1);
    sleep_ms(static_cast<int>(std::lround(50.0 * m_step_scale)));
    for (int i = 1; i <= steps; ++i) {
        double r = static_cast<double>(i) / static_cast<double>(steps);
        touch_event(
            static_cast<int>(InputEvent::Type::TOUCH_MOVE),
            x1 + (x2 - x1) * r,
            y1 + (y2 - y1) * r);
        sleep_ms(static_cast<int>(std::lround(step_ms * m_step_scale)));
    }
    sleep_ms(static_cast<int>(std::lround(hold_ms * m_step_scale)));
    touch_event(static_cast<int>(InputEvent::Type::TOUCH_UP), x2, y2);
}

cv::Mat SPAFarmTaskPlugin::drag_board_unit(
    double x1,
    double y1,
    double x2,
    double y2,
    bool capture_before_up)
{
    // Exact 1:1 port of `we(I, k, D, B, ...)` in service.pretty.mjs lines 9085-9123:
    // Stage 1: touch_down(x1, y1), hold 220ms, lift vertically to mid_y = min(y1 - 140, y2) in 8 steps (25ms each), hold 60ms
    // Stage 2: move from (x1, mid_y) to (x2, y2) in 10 steps (25ms each), hold 120ms, optional screenshot, touch_up(x2, y2)
    const double mid_y = std::min(y1 - 140.0, y2);
    touch_event(static_cast<int>(InputEvent::Type::TOUCH_DOWN), x1, y1);
    sleep_ms(static_cast<int>(std::lround(220.0 * m_step_scale)));
    for (int i = 1; i <= 8; ++i) {
        double r = static_cast<double>(i) / 8.0;
        touch_event(
            static_cast<int>(InputEvent::Type::TOUCH_MOVE),
            x1,
            y1 + (mid_y - y1) * r);
        sleep_ms(static_cast<int>(std::lround(25.0 * m_step_scale)));
    }
    sleep_ms(static_cast<int>(std::lround(60.0 * m_step_scale)));
    for (int i = 1; i <= 10; ++i) {
        double r = static_cast<double>(i) / 10.0;
        touch_event(
            static_cast<int>(InputEvent::Type::TOUCH_MOVE),
            x1 + (x2 - x1) * r,
            mid_y + (y2 - mid_y) * r);
        sleep_ms(static_cast<int>(std::lround(25.0 * m_step_scale)));
    }
    sleep_ms(static_cast<int>(std::lround(120.0 * m_step_scale)));
    cv::Mat captured;
    if (capture_before_up) {
        captured = shot();
    }
    touch_event(static_cast<int>(InputEvent::Type::TOUCH_UP), x2, y2);
    return captured;
}

// ============================================================================
// Logging & MAA GUI Task Completion / Status Reporting
// ============================================================================

int SPAFarmTaskPlugin::total_available_strategies() const
{
    int cnt = 0;
    for (const auto& s : m_strategies) {
        if (s.available) {
            ++cnt;
        }
    }
    return cnt > 0 ? cnt : 62;
}

void SPAFarmTaskPlugin::log_info(const std::string& msg)
{
    Log.info("SPAFarm |", msg);
    json::value info = basic_info();
    info["what"] = "SPAFarmStatus";
    info["details"] = json::object {
        { "message", msg },
        { "goal", m_goal },
        { "profile", m_profile },
        { "target", m_target },
        { "runs", m_runs_completed },
        { "max_runs", m_runs },
        { "attempts", m_attempts_done },
        { "max_attempts", m_attempts },
        { "medals_earned", m_medals_earned },
        { "completed_strategies", static_cast<int>(m_done_strategies.size()) },
        { "total_strategies", total_available_strategies() },
        { "current_strategy", m_current_strategy },
        { "strategy_id", m_current_strategy_id },
        { "round", m_current_round },
        { "shop_level", m_current_shop_level },
        { "gold", m_current_gold }
    };
    if (status()) {
        status()->set_number("SPAFarm.RunsCompleted", m_runs_completed);
        status()->set_number("SPAFarm.AttemptsDone", m_attempts_done);
        status()->set_number("SPAFarm.MedalsEarned", m_medals_earned);
        status()->set_number("SPAFarm.CompletedStrategies", static_cast<int64_t>(m_done_strategies.size()));
        status()->set_str("SPAFarm.LastMessage", msg);
    }
    callback(AsstMsg::SubTaskExtraInfo, info);
}

void SPAFarmTaskPlugin::report_gui_progress(const std::string& event_type, const std::string& message)
{
    log_info(message);
    json::value info = basic_info_with_what(event_type);
    json::array strat_arr;
    for (const auto& [name, clears] : m_strategy_clears) {
        strat_arr.emplace_back(json::object {
            { "name", name },
            { "clears", clears },
            { "target", m_target },
            { "completed", clears >= m_target }
        });
    }
    info["details"] = json::object {
        { "message", message },
        { "goal", m_goal },
        { "profile", m_profile },
        { "target", m_target },
        { "runs", m_runs_completed },
        { "max_runs", m_runs },
        { "attempts", m_attempts_done },
        { "max_attempts", m_attempts },
        { "medals_earned", m_medals_earned },
        { "completed_strategies", static_cast<int>(m_done_strategies.size()) },
        { "total_strategies", total_available_strategies() },
        { "current_strategy", m_current_strategy },
        { "strategy_id", m_current_strategy_id },
        { "round", m_current_round },
        { "shop_level", m_current_shop_level },
        { "gold", m_current_gold },
        { "strategies", std::move(strat_arr) }
    };
    callback(AsstMsg::SubTaskExtraInfo, info);
}

void SPAFarmTaskPlugin::report_match_finished(bool won, int round_reached)
{
    ++m_attempts_done;
    int cur_clears = 0;
    bool newly_earned_medal = false;
    std::string strat_label = m_current_strategy.empty() ? "当前策略" : m_current_strategy;

    if (!m_current_strategy.empty()) {
        cur_clears = m_strategy_clears[m_current_strategy];
    }
    if (won) {
        ++m_runs_completed;
        if (!m_current_strategy.empty()) {
            bool was_done = m_done_strategies.count(m_current_strategy) > 0 || cur_clears >= m_target;
            cur_clears = ++m_strategy_clears[m_current_strategy];
            for (auto& s : m_strategies) {
                if (s.name == m_current_strategy) {
                    s.passed = cur_clears;
                    break;
                }
            }
            if (cur_clears >= m_target) {
                m_done_strategies.insert(m_current_strategy);
                if (!was_done) {
                    ++m_medals_earned;
                    newly_earned_medal = true;
                }
            }
        }
    }

    std::ostringstream oss;
    oss << "【对局结算】第 " << m_attempts_done << " 场：" << (won ? "模拟完成(胜利)" : "模拟中断(失败)")
        << " | 策略：" << strat_label;
    if (!m_current_strategy.empty()) {
        oss << " (通关 " << cur_clears << "/" << m_target << ")";
    }
    oss << " | 到达回合：" << std::max(1, round_reached)
        << " | 累计通关：" << m_runs_completed << "/" << m_runs
        << " | 策略刻章完成：" << m_done_strategies.size() << "/" << total_available_strategies();
    report_gui_progress("SPAFarmMatchResult", oss.str());

    if (newly_earned_medal) {
        std::ostringstream medal_oss;
        medal_oss << "【刻章达成】策略「" << strat_label << "」已达成 " << m_target
                  << " 次通关刻章！(本次运行新获刻章：" << m_medals_earned
                  << "，总完成策略：" << m_done_strategies.size() << "/" << total_available_strategies() << ")";
        report_gui_progress("SPAFarmMedalEarned", medal_oss.str());
    }
}

void SPAFarmTaskPlugin::report_task_summary(const std::string& stop_reason, bool completed_ok)
{
    std::ostringstream oss;
    oss << "【任务完成情况汇总】状态：" << (completed_ok ? "已完成" : "已停止")
        << " | 原因：" << stop_reason
        << " | 累计通关：" << m_runs_completed << "/" << m_runs << " 局"
        << " | 总尝试：" << m_attempts_done << " 场"
        << " | 本次新获刻章：" << m_medals_earned << " 个"
        << " | 已达标策略：" << m_done_strategies.size() << "/" << total_available_strategies();
    if (!m_strategy_clears.empty()) {
        oss << " | 策略明细：";
        int shown = 0;
        for (const auto& [name, clears] : m_strategy_clears) {
            if (shown > 0) {
                oss << "、";
            }
            oss << name << "(" << clears << "/" << m_target << ")";
            if (++shown >= 8 && m_strategy_clears.size() > 8) {
                oss << " 等共" << m_strategy_clears.size() << "项";
                break;
            }
        }
    }
    const std::string summary_msg = oss.str();
    report_gui_progress("SPAFarmTaskCompleted", summary_msg);

    json::value chain_info = basic_info_with_what("SPAFarmTaskCompleted");
    chain_info["details"]["message"] = summary_msg;
    chain_info["details"]["reason"] = stop_reason;
    chain_info["details"]["completed"] = completed_ok;
    chain_info["details"]["runs"] = m_runs_completed;
    chain_info["details"]["max_runs"] = m_runs;
    chain_info["details"]["attempts"] = m_attempts_done;
    chain_info["details"]["medals_earned"] = m_medals_earned;
    chain_info["details"]["completed_strategies"] = static_cast<int>(m_done_strategies.size());
    chain_info["details"]["total_strategies"] = total_available_strategies();
    callback(AsstMsg::TaskChainExtraInfo, chain_info);
}

// ============================================================================
// OCR & Dictionary Fuzzy Matching (`rt`, `Fi`, `vi`, `Jr`, `xr`, `yi`)
// ============================================================================

std::vector<SPAFarmTaskPlugin::OcrItem> SPAFarmTaskPlugin::ocr(
    const cv::Mat& img,
    const std::optional<std::array<int, 4>>& roi_xywh,
    bool pad_replicate) const
{
    if (img.empty()) {
        return {};
    }
    cv::Mat work;
    int ox = 0;
    int oy = 0;

    if (roi_xywh.has_value()) {
        auto [rx, ry, rw, rh] = *roi_xywh;
        int x1 = std::clamp(rx, 0, img.cols);
        int y1 = std::clamp(ry, 0, img.rows);
        int x2 = std::clamp(rx + rw, 0, img.cols);
        int y2 = std::clamp(ry + rh, 0, img.rows);
        if (x2 <= x1 || y2 <= y1) {
            return {};
        }
        cv::Mat crop = img(cv::Rect(x1, y1, x2 - x1, y2 - y1));
        if (pad_replicate) {
            constexpr int P = 40;
            cv::copyMakeBorder(crop, work, P, P, P, P, cv::BORDER_REPLICATE);
            ox = x1 - P;
            oy = y1 - P;
        }
        else {
            work = crop.clone();
            ox = x1;
            oy = y1;
        }
    }
    else {
        work = img;
    }

    auto raw_res = WordOcr::get_instance().recognize(work, false);
    std::vector<OcrItem> out;
    out.reserve(raw_res.size());
    for (const auto& it : raw_res) {
        std::string txt = normalize_variants(it.text);
        int bx = ox + it.rect.x;
        int by = oy + it.rect.y;
        double cx = bx + it.rect.width * 0.5;
        double cy = by + it.rect.height * 0.5;
        out.push_back(OcrItem { cx, cy, bx, by, it.rect.width, it.rect.height, std::move(txt), it.score });
    }
    return out;
}

std::vector<SPAFarmTaskPlugin::OcrItem> SPAFarmTaskPlugin::ocr_rec_only(
    const cv::Mat& img,
    const std::array<int, 4>& roi_xywh,
    int upscale,
    int black_border) const
{
    if (img.empty()) {
        return {};
    }
    auto [rx, ry, rw, rh] = roi_xywh;
    int x1 = std::clamp(rx, 0, img.cols);
    int y1 = std::clamp(ry, 0, img.rows);
    int x2 = std::clamp(rx + rw, 0, img.cols);
    int y2 = std::clamp(ry + rh, 0, img.rows);
    if (x2 <= x1 || y2 <= y1) {
        return {};
    }
    cv::Mat crop = img(cv::Rect(x1, y1, x2 - x1, y2 - y1)).clone();
    if (upscale > 1) {
        cv::resize(crop, crop, cv::Size((x2 - x1) * upscale, (y2 - y1) * upscale), 0.0, 0.0, cv::INTER_CUBIC);
    }
    if (black_border > 0) {
        cv::copyMakeBorder(
            crop,
            crop,
            black_border,
            black_border,
            black_border,
            black_border,
            cv::BORDER_CONSTANT,
            cv::Scalar(0, 0, 0));
    }
    auto raw_res = WordOcr::get_instance().recognize(crop, true);
    std::vector<OcrItem> out;
    out.reserve(raw_res.size());
    for (const auto& it : raw_res) {
        std::string txt = normalize_variants(it.text);
        out.push_back(OcrItem {
            x1 + (x2 - x1) * 0.5,
            y1 + (y2 - y1) * 0.5,
            x1,
            y1,
            x2 - x1,
            y2 - y1,
            std::move(txt),
            it.score });
    }
    return out;
}

// Exact 1:1 port of `Te = (a, s) => a.texts.find(e => e.score >= .55 && s.test(e.text.replace(/\s/g, "")))`
// Outer loop iterates `keywords` in caller-specified priority order so high-priority button texts
// are matched before fallback keywords, and enforces `it.score >= 0.55`.
std::optional<SPAFarmTaskPlugin::OcrItem> SPAFarmTaskPlugin::find_ocr(
    const std::vector<OcrItem>& items,
    std::initializer_list<std::string_view> keywords)
{
    for (std::string_view kw : keywords) {
        if (kw.empty()) {
            continue;
        }
        for (const auto& it : items) {
            if (it.score >= 0.55 && it.text.find(kw) != std::string::npos) {
                return it;
            }
        }
    }
    return std::nullopt;
}

std::optional<SPAFarmTaskPlugin::OcrItem> SPAFarmTaskPlugin::find_ocr_exact(
    const std::vector<OcrItem>& items,
    std::initializer_list<std::string_view> exact_keywords)
{
    for (std::string_view kw : exact_keywords) {
        std::string target = norm_text(kw);
        if (target.empty()) {
            continue;
        }
        for (const auto& it : items) {
            if (it.score >= 0.55 && norm_text(it.text) == target) {
                return it;
            }
        }
    }
    return std::nullopt;
}

std::optional<SPAFarmTaskPlugin::OcrItem> SPAFarmTaskPlugin::find_ocr_in_roi(
    const std::vector<OcrItem>& items,
    std::initializer_list<std::string_view> keywords,
    int x0,
    int y0,
    int x1,
    int y1)
{
    for (std::string_view kw : keywords) {
        if (kw.empty()) {
            continue;
        }
        for (const auto& it : items) {
            if (it.score < 0.55 || it.cx < x0 || it.cx > x1 || it.cy < y0 || it.cy > y1) {
                continue;
            }
            if (it.text.find(kw) != std::string::npos) {
                return it;
            }
        }
    }
    return std::nullopt;
}

bool SPAFarmTaskPlugin::has_ocr(
    const std::vector<OcrItem>& items,
    std::initializer_list<std::string_view> keywords)
{
    return find_ocr(items, keywords).has_value();
}

bool SPAFarmTaskPlugin::has_ocr_exact(
    const std::vector<OcrItem>& items,
    std::initializer_list<std::string_view> exact_keywords)
{
    return find_ocr_exact(items, exact_keywords).has_value();
}

std::string SPAFarmTaskPlugin::norm_text(std::string_view sv)
{
    std::string norm_v = normalize_variants(sv);
    std::string out;
    for (const auto& ch : split_utf8_chars(norm_v)) {
        if (is_cjk_or_alnum_cp(ch)) {
            out += ch;
        }
    }
    return out;
}

std::unordered_map<std::string, int> SPAFarmTaskPlugin::char_bag(std::string_view norm_sv)
{
    std::unordered_map<std::string, int> bag;
    for (const auto& ch : split_utf8_chars(norm_sv)) {
        bag[ch]++;
    }
    return bag;
}

double SPAFarmTaskPlugin::bag_similarity(
    const std::unordered_map<std::string, int>& a,
    int len_a,
    const std::unordered_map<std::string, int>& b,
    int len_b)
{
    if (len_a <= 0 || len_b <= 0) {
        return 0.0;
    }
    int common = 0;
    for (const auto& [ch, cnt_a] : a) {
        auto it = b.find(ch);
        if (it != b.end()) {
            common += std::min(cnt_a, it->second);
        }
    }
    return static_cast<double>(common) / static_cast<double>(std::max(len_a, len_b));
}

std::vector<SPAFarmTaskPlugin::DictEntry> SPAFarmTaskPlugin::build_dict(const std::vector<std::string>& names)
{
    std::set<std::string> uniq(names.begin(), names.end());
    std::vector<DictEntry> dict;
    dict.reserve(uniq.size());
    for (const auto& name : uniq) {
        std::string n = norm_text(name);
        if (n.empty()) {
            continue;
        }
        auto chars = split_utf8_chars(n);
        dict.push_back(DictEntry {
            name,
            n,
            static_cast<int>(chars.size()),
            char_bag(n) });
    }
    std::sort(dict.begin(), dict.end(), [](const DictEntry& a, const DictEntry& b) {
        return a.len > b.len;
    });
    return dict;
}

std::vector<SPAFarmTaskPlugin::DictMatch> SPAFarmTaskPlugin::match_dict(
    std::string_view text,
    const std::vector<DictEntry>& dict)
{
    // Exact 1:1 port of `xr(a, s, e = .66)` in service.pretty.mjs lines 5058-5133
    std::string norm = norm_text(text);
    auto chars = split_utf8_chars(norm);
    const int total_len = static_cast<int>(chars.size());
    if (total_len == 0) {
        return {};
    }

    auto slice_chars = [&](int start, int end) {
        std::string s;
        for (int i = start; i < end && i < total_len; ++i) {
            s += chars[i];
        }
        return s;
    };

    std::vector<DictMatch> candidates;
    for (const auto& d : dict) {
        // 1. Exact substring search
        for (int pos = 0; pos + d.len <= total_len;) {
            if (slice_chars(pos, pos + d.len) == d.norm) {
                candidates.push_back(DictMatch { d.name, d.len, pos, pos + d.len, 1.0, true });
                pos += d.len;
            }
            else {
                ++pos;
            }
        }
        if (d.len < 2) {
            continue;
        }
        // 2. Fuzzy character-bag window search (`vi >= 0.66`)
        DictMatch best_fuzzy;
        bool has_fuzzy = false;
        const int min_w = std::max(2, d.len - 1);
        const int max_w = std::min(total_len, d.len + 1);
        for (int w = min_w; w <= max_w; ++w) {
            for (int pos = 0; pos + w <= total_len; ++pos) {
                std::string sub = slice_chars(pos, pos + w);
                if (sub == d.norm) {
                    continue;
                }
                double sim = bag_similarity(char_bag(sub), w, d.bag, d.len);
                if (sim >= 0.66 && (!has_fuzzy || sim > best_fuzzy.score)) {
                    best_fuzzy = DictMatch { d.name, d.len, pos, pos + w, sim, false };
                    has_fuzzy = true;
                }
            }
        }
        if (has_fuzzy) {
            candidates.push_back(best_fuzzy);
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const DictMatch& a, const DictMatch& b) {
        if (a.exact != b.exact) {
            return a.exact > b.exact;
        }
        if (std::abs(a.score - b.score) > 1e-6) {
            return a.score > b.score;
        }
        if (a.len != b.len) {
            return a.len > b.len;
        }
        return a.start < b.start;
    });

    std::vector<uint8_t> used(total_len, 0);
    std::vector<DictMatch> selected;
    for (const auto& m : candidates) {
        bool overlap = false;
        for (int i = m.start; i < m.end; ++i) {
            if (used[i]) {
                overlap = true;
                break;
            }
        }
        if (!overlap) {
            for (int i = m.start; i < m.end; ++i) {
                used[i] = 1;
            }
            selected.push_back(m);
        }
    }

    std::sort(selected.begin(), selected.end(), [](const DictMatch& a, const DictMatch& b) {
        return a.start < b.start;
    });
    return selected;
}

double SPAFarmTaskPlugin::match_coverage(std::string_view text, const std::vector<DictMatch>& matches)
{
    auto chars = split_utf8_chars(norm_text(text));
    if (chars.empty()) {
        return 0.0;
    }
    int covered = 0;
    for (const auto& m : matches) {
        covered += (m.end - m.start);
    }
    return static_cast<double>(covered) / static_cast<double>(chars.size());
}

// ============================================================================
// Data Loading (`oa`, `hr`/`pr`, `Er`)
// ============================================================================

SPAFarmTaskPlugin::StackTraitInfo SPAFarmTaskPlugin::parse_stack_trait(std::string_view trait_text)
{
    // Exact 1:1 port of `hr(a)` in service.pretty.mjs lines 5817-5904
    StackTraitInfo info;
    if (trait_text.empty()) {
        return info;
    }
    std::string text(trait_text);
    if (text.find("叠层") == std::string::npos && text.find("层数") == std::string::npos) {
        return info;
    }

    auto extract_bond = [](std::string_view clause) -> std::string {
        static const std::vector<std::string> kBondNames = {
            "迅捷", "坚城", "强攻", "精准", "奥术", "守望", "突袭", "堡垒", "不朽",
            "刚毅", "远见", "奇迹", "灵巧", "锐利", "源石", "辞岁", "不赦", "荣光",
            "萨尔贡", "谢拉格", "拉特兰", "炎", "极昼", "辅助", "医疗", "近卫"
        };
        for (const auto& b : kBondNames) {
            if (clause.find("[" + b + "]") != std::string::npos || clause.find(b) != std::string::npos) {
                return b;
            }
        }
        return "";
    };

    std::string nb = extract_bond(text);
    bool other_only = (text.find("其他") != std::string::npos);

    if (text.find("前方一格") != std::string::npos) {
        info.rules.push_back(StackGeomRule { "front1", nb, true, false });
    }
    if (text.find("身后一格") != std::string::npos) {
        info.rules.push_back(StackGeomRule { "back1", nb, true, false });
    }
    if (text.find("同一行") != std::string::npos) {
        info.rules.push_back(StackGeomRule { "sameRow", nb, other_only, false });
    }
    if (text.find("周围四格") != std::string::npos) {
        info.rules.push_back(StackGeomRule { "around4", nb, true, false });
    }
    if (text.find("周围八格") != std::string::npos) {
        info.rules.push_back(StackGeomRule { "around8", nb, true, false });
    }
    if (text.find("周围十二格") != std::string::npos) {
        info.rules.push_back(StackGeomRule { "around12", nb, true, false });
    }
    if (text.find("攻击范围内") != std::string::npos && text.find("自身不处于") == std::string::npos) {
        info.rules.push_back(StackGeomRule { "inRange", nb, other_only, false });
    }

    info.on_skill = (text.find("技能") != std::string::npos && text.find("时") != std::string::npos);
    info.on_deploy = (text.find("部署") != std::string::npos);
    info.end_of_prep = (text.find("休整阶段结束") != std::string::npos);
    info.infects_tile = (text.find("格变为感染") != std::string::npos);
    info.uses_infected_tile = (text.find("位于感染格") != std::string::npos);
    return info;
}

std::vector<std::pair<int, int>> SPAFarmTaskPlugin::default_range_for_subprof(
    std::string_view sub_prof,
    std::string_view pos)
{
    // Exact 1:1 port of `Er` in service.pretty.mjs lines 5684-5728
    auto rect_range = [](int r0, int r1, int c0, int c1) {
        std::vector<std::pair<int, int>> out;
        for (int r = r0; r <= r1; ++r) {
            for (int c = c0; c <= c1; ++c) {
                out.emplace_back(r, c);
            }
        }
        return out;
    };

    if (sub_prof == "aoesniper" || sub_prof == " artilleryman") {
        return rect_range(-1, 1, 0, 4);
    }
    if (sub_prof == "reaperrange" || sub_prof == " Longrange") {
        return rect_range(-2, 2, 0, 4);
    }
    if (sub_prof == "fastshot" || sub_prof == "closerange" || sub_prof == "corecaster"
        || sub_prof == "splashcaster" || sub_prof == "funnel" || sub_prof == "chain"
        || sub_prof == "slower" || sub_prof == "summoner" || sub_prof == "ritualist"
        || sub_prof == "alchemist" || sub_prof == "medebuff") {
        return rect_range(-1, 1, 0, 3);
    }
    if (sub_prof == "physician" || sub_prof == "ringhealer" || sub_prof == "healer") {
        return rect_range(-1, 1, 0, 3);
    }
    if (sub_prof == "wandermedic" || sub_prof == "bard" || sub_prof == "blastcaster") {
        return rect_range(-2, 2, -2, 2);
    }
    if (sub_prof == "lord") {
        auto r = rect_range(-1, 1, 0, 2);
        r.emplace_back(0, 3);
        return r;
    }
    if (sub_prof == "instructor" || sub_prof == "charger") {
        return rect_range(0, 0, 0, 2);
    }
    if (sub_prof == "protector" || sub_prof == "guardian" || sub_prof == "unyield"
        || sub_prof == "artsprotector" || sub_prof == "duelist") {
        return { { 0, 0 } };
    }
    if (pos == "RANGED") {
        return rect_range(-1, 1, 0, 3);
    }
    return rect_range(0, 0, 0, 1);
}

std::pair<int, int> SPAFarmTaskPlugin::rotate_offset(std::pair<int, int> dr_dc, std::string_view facing)
{
    // Exact 1:1 port of `Br([a, s], e)` in service.pretty.mjs lines 5661-5673
    const int a = dr_dc.first;
    const int s = dr_dc.second;
    const int t = -a;
    if (facing == "RIGHT") {
        return { t, s };
    }
    if (facing == "LEFT") {
        return { -t, -s };
    }
    if (facing == "UP") {
        return { -s, -t };
    }
    if (facing == "DOWN") {
        return { s, t };
    }
    return { t, s };
}

void SPAFarmTaskPlugin::init_builtin_fallbacks()
{
    m_collapsed_board_H = solve_homography({
        { 4, 1, 790, 308 },
        { 9, 1, 1380, 308 },
        { 4, 2, 785, 400 },
        { 9, 2, 1395, 400 },
        { 4, 5, 760, 712 },
        { 9, 5, 1450, 712 },
        { 4, 6, 748, 840 },
        { 9, 6, 1465, 840 }
    });

    if (m_calib_expanded.anchors.empty()) {
        m_calib_expanded.camera = "expanded";
        m_calib_expanded.anchors = {
            { 1, 1, 469, 174 }, { 12, 1, 1659, 174 },
            { 1, 2, 439, 272 }, { 12, 2, 1690, 272 },
            { 1, 3, 409, 376 }, { 12, 3, 1720, 376 },
            { 1, 4, 376, 488 }, { 12, 4, 1752, 488 }
        };
        m_calib_expanded.marker_E = { 597.5, 496.5 };
        m_calib_expanded.marker_S1 = { 1398.5, 211.0 };
        m_calib_expanded.marker_S2 = { 1554.0, 412.5 };
        m_calib_expanded.ref_n_E = 899;
        m_calib_expanded.ref_n_S1 = 964;
        m_calib_expanded.ref_n_S2 = 965;
        m_calib_expanded.base_H = solve_homography(m_calib_expanded.anchors);
    }

    if (m_calib_collapsed.anchors.empty()) {
        m_calib_collapsed.camera = "collapsed";
        m_calib_collapsed.anchors = {
            { 1, 1, 432, 308 }, { 12, 1, 1689, 308 },
            { 1, 2, 402, 400 }, { 12, 2, 1718, 400 },
            { 1, 3, 370, 500 }, { 12, 3, 1750, 500 },
            { 1, 4, 336, 610 }, { 12, 4, 1784, 610 }
        };
        m_calib_collapsed.marker_E = { 555.5, 651.5 };
        m_calib_collapsed.marker_S1 = { 1383.5, 344.5 };
        m_calib_collapsed.marker_S2 = { 1546.5, 560.5 };
        m_calib_collapsed.ref_n_E = 885;
        m_calib_collapsed.ref_n_S1 = 948;
        m_calib_collapsed.ref_n_S2 = 955;
        m_calib_collapsed.base_H = solve_homography(m_calib_collapsed.anchors);
    }

    if (m_stages.empty()) {
        StageInfo st;
        st.stage_id = "act3autochess_m01";
        st.name = "卫戍协议默认地图";
        st.width = 13;
        st.height = 7;
        st.rows = {
            "xxxxxxxxxxxxx",
            "xxxhhaahSxxxx",
            "xxxhaabbbxxxx",
            "xxxhbbhhbSxxx",
            "xxEbbbbbbxxxx",
            "xxxxQQQQQQxxx",
            "xxxHHHHHHHHHx"
        };
        m_stages.push_back(std::move(st));
    }
}

bool SPAFarmTaskPlugin::load_all_data()
{
    if (m_data_loaded) {
        return true;
    }

    init_builtin_fallbacks();

    std::vector<std::filesystem::path> candidate_dirs = {
        std::filesystem::current_path() / "resource" / "SPAFarm",
        std::filesystem::path("resource") / "SPAFarm",
        std::filesystem::path("..") / "resource" / "SPAFarm",
        std::filesystem::path("data")
    };

    std::filesystem::path data_dir;
    for (const auto& dir : candidate_dirs) {
        std::error_code ec;
        if (std::filesystem::exists(dir / "chess.json", ec)) {
            data_dir = dir;
            break;
        }
    }

    auto read_json_file = [](const std::filesystem::path& p) -> std::optional<json::value> {
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) {
            return std::nullopt;
        }
        return json::open(p);
    };

    if (!data_dir.empty()) {
        // 1. Load bonds.json
        if (auto j_bonds = read_json_file(data_dir / "bonds.json"); j_bonds && j_bonds->is_array()) {
            m_bonds.clear();
            m_bond_by_norm.clear();
            for (const auto& jb : j_bonds->as_array()) {
                BondInfo b;
                b.bond_id = jb.get("bondId", std::string());
                b.name = jb.get("name", std::string());
                b.norm_name = norm_text(b.name);
                b.is_core = jb.get("isCore", false);
                b.count_mode = jb.get("countMode", std::string("FIELD_ONLY"));
                b.max_Active = jb.get("activeCount", 6);
                b.desc = jb.get("desc", std::string());
                if (auto th = jb.find<json::array>("thresholds"); th) {
                    for (const auto& v : *th) {
                        if (v.is_number()) {
                            b.thresholds.push_back(v.as_integer());
                        }
                    }
                }
                if (b.thresholds.empty()) {
                    b.thresholds.push_back(std::max(2, b.max_Active));
                }
                if (auto ops = jb.find<json::array>("operatorNames"); ops) {
                    for (const auto& v : *ops) {
                        if (v.is_string()) {
                            b.operator_names.push_back(v.as_string());
                        }
                    }
                }
                if (!b.norm_name.empty()) {
                    m_bond_by_norm[b.norm_name] = m_bonds.size();
                    m_bonds.push_back(std::move(b));
                }
            }
        }

        // 2. Load chess.json (non-golden base entries + elite stats)
        if (auto j_chess = read_json_file(data_dir / "chess.json"); j_chess && j_chess->is_array()) {
            m_operators.clear();
            m_op_by_norm.clear();
            for (const auto& jc : j_chess->as_array()) {
                if (jc.get("golden", false)) {
                    continue;
                }
                OperatorInfo op;
                op.chess_id = jc.get("chessId", std::string());
                op.char_id = jc.get("charId", std::string());
                op.name = jc.get("name", std::string());
                op.norm_name = norm_text(op.name);
                op.tier = std::clamp(jc.get("tier", 1), 1, 6);
                op.elite = false;
                op.golden = false;
                op.profession = jc.get("profession", std::string());
                op.sub_profession_id = jc.get("subProfessionId", std::string());
                op.position = jc.get("position", std::string("MELEE"));
                op.purchase_price = jc.get("purchasePrice", 2);
                op.sell_price = jc.get("sellPrice", 1);
                op.trait_text = jc.get("traitText", std::string());
                if (auto st = jc.find<json::object>("stats"); st) {
                    op.atk = st->get("atk", 0);
                    op.def = st->get("def", 0);
                    op.max_hp = st->get("maxHp", 0);
                    op.block_cnt = st->get("blockCnt", 1);
                }
                if (auto bnames = jc.find<json::array>("bondNames"); bnames) {
                    for (const auto& v : *bnames) {
                        if (v.is_string()) {
                            op.bond_names.push_back(v.as_string());
                        }
                    }
                }
                op.stack_info = parse_stack_trait(op.trait_text);
                op.base_range = default_range_for_subprof(op.sub_profession_id, op.position);
                op.can_block_air = (op.name == "琉璃");
                op.is_healer = (op.profession == "MEDIC" || op.sub_profession_id == "bard" || op.sub_profession_id == "guardian");
                op.facing_independent = (
                    op.sub_profession_id == "bard" || op.sub_profession_id == "wandermedic"
                    || op.sub_profession_id == "summoner" || op.sub_profession_id == "ritualist"
                    || op.sub_profession_id == "stalker" || op.sub_profession_id == "alchemist"
                    || op.sub_profession_id == "craftsman");

                bool has_jiancheng = std::find(op.bond_names.begin(), op.bond_names.end(), "坚城") != op.bond_names.end();
                bool has_qiangdun = std::find(op.bond_names.begin(), op.bond_names.end(), "强盾") != op.bond_names.end();
                op.taunt_level = has_jiancheng ? 3 : (has_qiangdun || op.profession == "TANK") ? 2
                    : (op.profession == "PIONEER" || op.profession == "WARRIOR") ? 1 : 0;

                static constexpr double kTierWeight[7] = { 0.0, 0.7, 0.85, 1.0, 1.2, 1.45, 1.75 };
                op.dps_weight = kTierWeight[op.tier];
                if (op.profession == "MEDIC") {
                    op.dps_weight *= 0.25;
                }
                else if (op.profession == "TANK" && op.sub_profession_id != "artsprotector") {
                    op.dps_weight *= 0.45;
                }

                std::string skills_str = op.trait_text + jc.get("skill", std::string()) + jc.get("talents", std::string());
                op.is_summoner = (
                    skills_str.find("召唤") != std::string::npos
                    || skills_str.find("装置") != std::string::npos
                    || skills_str.find("无人机") != std::string::npos);

                if (!op.norm_name.empty() && m_op_by_norm.find(op.norm_name) == m_op_by_norm.end()) {
                    m_op_by_norm[op.norm_name] = m_operators.size();
                    m_operators.push_back(std::move(op));
                }
            }
        }

        // 3. Load items.json
        if (auto j_items = read_json_file(data_dir / "items.json"); j_items && j_items->is_array()) {
            m_items.clear();
            m_item_by_norm.clear();
            for (const auto& ji : j_items->as_array()) {
                if (ji.get("golden", false)) {
                    continue;
                }
                ItemInfo it;
                it.id = ji.get("id", std::string());
                it.name = ji.get("name", std::string());
                it.norm_name = norm_text(it.name);
                it.tier = ji.get("tier", 1);
                it.price = ji.get("price", 2);
                it.effect = ji.get("effect", std::string());
                for (const auto& b : m_bonds) {
                    if (it.effect.find("[" + b.name + "]") != std::string::npos
                        && it.effect.find("获得") != std::string::npos) {
                        it.granted_bond_name = b.name;
                        it.granted_bond_id = b.bond_id;
                        break;
                    }
                }
                it.category = classify_item_category(it.name, it.effect);
                it.is_debuff = (it.category == "debuff")
                    || (it.effect.find("晕眩") != std::string::npos && it.effect.find("携带者") != std::string::npos);
                if (!it.norm_name.empty() && m_item_by_norm.find(it.norm_name) == m_item_by_norm.end()) {
                    m_item_by_norm[it.norm_name] = m_items.size();
                    m_items.push_back(std::move(it));
                }
            }
        }

        // 4. Load farm-rules.json
        if (auto j_rules = read_json_file(data_dir / "farm-rules.json"); j_rules && j_rules->is_object()) {
            if (auto strats = j_rules->find<json::array>("strategies"); strats) {
                m_strategies.clear();
                for (const auto& js : *strats) {
                    StrategyRule sr;
                    sr.id = js.get("id", std::string());
                    sr.name = js.get("name", std::string());
                    sr.norm_name = norm_text(sr.name);
                    sr.owner = js.get("owner", std::string());
                    sr.available = js.get("available", true);
                    if (!sr.name.empty()) {
                        m_strategies.push_back(std::move(sr));
                    }
                }
            }
        }

        // 5. Load stages.json
        if (auto j_stages = read_json_file(data_dir / "stages.json"); j_stages && j_stages->is_array()) {
            m_stages.clear();
            for (const auto& jst : j_stages->as_array()) {
                StageInfo st;
                st.stage_id = jst.get("stageId", std::string());
                st.name = jst.get("name", std::string());
                st.width = jst.get("width", 13);
                st.height = jst.get("height", 7);
                if (auto r_arr = jst.find<json::array>("rows"); r_arr) {
                    for (const auto& rv : *r_arr) {
                        if (rv.is_string()) {
                            st.rows.push_back(rv.as_string());
                        }
                    }
                }
                // Apply `wr` tile overrides for act3autochess_m01 (service.pretty.mjs lines 7529-7558)
                if (st.stage_id == "act3autochess_m01" && st.rows.size() >= 5) {
                    if (st.rows[4].size() > 9) {
                        st.rows[4][9] = 'a';
                        st.rows[4][8] = 'a';
                    }
                    if (st.rows[3].size() > 10) {
                        st.rows[3][10] = 'c';
                    }
                    if (st.rows[2].size() > 10) {
                        st.rows[2][10] = 'c';
                    }
                }
                if (!st.stage_id.empty() && !st.rows.empty()) {
                    m_stages.push_back(std::move(st));
                }
            }
        }

        // 6. Load board-refs.json
        if (auto j_brefs = read_json_file(data_dir / "board-refs.json"); j_brefs && j_brefs->is_array()) {
            m_board_refs.clear();
            for (const auto& jr : j_brefs->as_array()) {
                BoardRefEntry entry;
                entry.stage_id = jr.get("stageId", std::string());
                entry.camera = jr.get("camera", std::string());
                if (auto cells_obj = jr.find<json::object>("cells"); cells_obj) {
                    for (const auto& [cell_key, arr_val] : *cells_obj) {
                        if (!arr_val.is_array()) {
                            continue;
                        }
                        std::vector<double> vals;
                        for (const auto& elem : arr_val.as_array()) {
                            if (elem.is_number()) {
                                vals.push_back(elem.as_double());
                            }
                            else {
                                vals.push_back(std::numeric_limits<double>::quiet_NaN());
                            }
                        }
                        entry.cells[cell_key] = std::move(vals);
                    }
                }
                if (!entry.stage_id.empty()) {
                    m_board_refs.push_back(std::move(entry));
                }
            }
        }

        // 7. Load bosses.json & enemy-types.json
        if (auto j_bosses = read_json_file(data_dir / "bosses.json"); j_bosses && j_bosses->is_array()) {
            m_bosses.clear();
            for (const auto& jb : j_bosses->as_array()) {
                BossInfo bi;
                bi.enemy_id = jb.get("enemyId", std::string());
                bi.name = jb.get("name", std::string());
                bi.norm_name = norm_text(bi.name);
                bi.description = jb.get("description", std::string());
                if (!bi.name.empty()) {
                    m_bosses.push_back(std::move(bi));
                }
            }
        }
        if (auto j_etypes = read_json_file(data_dir / "enemy-types.json"); j_etypes && j_etypes->is_array()) {
            m_enemy_types.clear();
            for (const auto& je : j_etypes->as_array()) {
                EnemyTypeInfo ei;
                ei.code = je.get("code", std::string());
                ei.name = je.get("name", std::string());
                if (!ei.name.empty()) {
                    m_enemy_types.push_back(std::move(ei));
                }
            }
        }

        // 8. Load calib/m01-expanded.json & calib/m01-collapsed.json
        auto load_calib = [&](const std::filesystem::path& p, CameraCalibData& dst) {
            auto jc = read_json_file(p);
            if (!jc || !jc->is_object()) {
                return;
            }
            dst.camera = jc->get("camera", dst.camera);
            if (auto anc = jc->find<json::array>("anchors"); anc) {
                dst.anchors.clear();
                for (const auto& a : *anc) {
                    if (a.is_array() && a.as_array().size() >= 4) {
                        const auto& arr = a.as_array();
                        dst.anchors.push_back({
                            arr[0].as_double(),
                            arr[1].as_double(),
                            arr[2].as_double(),
                            arr[3].as_double() });
                    }
                }
            }
            if (auto mref = jc->find<json::object>("markerRef"); mref) {
                auto read_pt = [&](const char* k, std::array<double, 2>& pt) {
                    if (auto arr = mref->find<json::array>(k); arr && arr->size() >= 2) {
                        pt = { (*arr)[0].as_double(), (*arr)[1].as_double() };
                    }
                };
                read_pt("E", dst.marker_E);
                read_pt("S1", dst.marker_S1);
                read_pt("S2", dst.marker_S2);
            }
            if (auto nref = jc->find<json::object>("markerRefN"); nref) {
                dst.ref_n_E = nref->get("E", dst.ref_n_E);
                dst.ref_n_S1 = nref->get("S1", dst.ref_n_S1);
                dst.ref_n_S2 = nref->get("S2", dst.ref_n_S2);
            }
            if (dst.anchors.size() >= 4) {
                dst.base_H = solve_homography(dst.anchors);
            }
        };
        load_calib(data_dir / "calib" / "m01-expanded.json", m_calib_expanded);
        load_calib(data_dir / "calib" / "m01-collapsed.json", m_calib_collapsed);
    }

    // Build fuzzy lookup dictionaries (`Jr`)
    std::vector<std::string> op_names;
    for (const auto& op : m_operators) {
        op_names.push_back(op.name);
    }
    m_chess_dict = build_dict(op_names);

    std::vector<std::string> item_names;
    for (const auto& it : m_items) {
        item_names.push_back(it.name);
    }
    m_item_dict = build_dict(item_names);

    std::vector<std::string> bond_names;
    for (const auto& b : m_bonds) {
        bond_names.push_back(b.name);
    }
    m_bond_dict = build_dict(bond_names);

    std::vector<std::string> boss_names;
    for (const auto& b : m_bosses) {
        boss_names.push_back(b.name);
    }
    m_boss_dict = build_dict(boss_names);

    std::vector<std::string> strat_names;
    for (const auto& s : m_strategies) {
        strat_names.push_back(s.name);
    }
    m_strategy_dict = build_dict(strat_names);

    m_data_loaded = true;
    return true;
}

const SPAFarmTaskPlugin::OperatorInfo* SPAFarmTaskPlugin::find_operator(std::string_view name) const
{
    std::string n = norm_text(name);
    auto it = m_op_by_norm.find(n);
    if (it != m_op_by_norm.end() && it->second < m_operators.size()) {
        return &m_operators[it->second];
    }
    return nullptr;
}

const SPAFarmTaskPlugin::ItemInfo* SPAFarmTaskPlugin::find_item(std::string_view name) const
{
    std::string n = norm_text(name);
    auto it = m_item_by_norm.find(n);
    if (it != m_item_by_norm.end() && it->second < m_items.size()) {
        return &m_items[it->second];
    }
    return nullptr;
}

const SPAFarmTaskPlugin::BondInfo* SPAFarmTaskPlugin::find_bond(std::string_view name) const
{
    std::string n = norm_text(name);
    auto it = m_bond_by_norm.find(n);
    if (it != m_bond_by_norm.end() && it->second < m_bonds.size()) {
        return &m_bonds[it->second];
    }
    return nullptr;
}

const SPAFarmTaskPlugin::StageInfo* SPAFarmTaskPlugin::find_stage(std::string_view stage_id) const
{
    for (const auto& st : m_stages) {
        if (st.stage_id == stage_id) {
            return &st;
        }
    }
    return m_stages.empty() ? nullptr : &m_stages.front();
}

// ============================================================================
// Projective Homography & Dual-Camera Calibration (`kr`, `ft`, `os`, `Un`, `pn`, `wn`, `Hn`, `Sr`)
// ============================================================================

std::array<double, 9> SPAFarmTaskPlugin::solve_homography(const std::vector<std::array<double, 4>>& anchors)
{
    // Exact 1:1 port of `kr(a)` in service.pretty.mjs lines 4274-4298 (8-DOF projective homography via normal equations + Gaussian elimination)
    if (anchors.size() < 4) {
        return { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    }
    double A[8][9] = {};
    for (const auto& anc : anchors) {
        const double gx = anc[0];
        const double gy = anc[1];
        const double px = anc[2];
        const double py = anc[3];
        const double r1[9] = { gx, gy, 1.0, 0.0, 0.0, 0.0, -gx * px, -gy * px, px };
        const double r2[9] = { 0.0, 0.0, 0.0, gx, gy, 1.0, -gx * py, -gy * py, py };
        for (int i = 0; i < 8; ++i) {
            for (int j = 0; j < 9; ++j) {
                A[i][j] += r1[i] * r1[j] + r2[i] * r2[j];
            }
        }
    }

    for (int i = 0; i < 8; ++i) {
        int piv = i;
        for (int r = i + 1; r < 8; ++r) {
            if (std::abs(A[r][i]) > std::abs(A[piv][i])) {
                piv = r;
            }
        }
        if (std::abs(A[piv][i]) < 1e-12) {
            continue;
        }
        if (piv != i) {
            for (int c = i; c <= 8; ++c) {
                std::swap(A[i][c], A[piv][c]);
            }
        }
        for (int r = i + 1; r < 8; ++r) {
            const double f = A[r][i] / A[i][i];
            for (int c = i; c <= 8; ++c) {
                A[r][c] -= f * A[i][c];
            }
        }
    }

    std::array<double, 9> h = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    for (int i = 7; i >= 0; --i) {
        double s = A[i][8];
        for (int c = i + 1; c < 8; ++c) {
            s -= A[i][c] * h[c];
        }
        h[i] = (std::abs(A[i][i]) > 1e-12) ? (s / A[i][i]) : 0.0;
    }
    h[8] = 1.0;
    return h;
}

std::pair<double, double> SPAFarmTaskPlugin::project_point(const std::array<double, 9>& H, double gx, double gy)
{
    // Exact 1:1 port of `ft(a, s, e)` in service.pretty.mjs lines 4300-4303
    const double w = H[6] * gx + H[7] * gy + H[8];
    if (std::abs(w) < 1e-12) {
        return { 0.0, 0.0 };
    }
    return {
        (H[0] * gx + H[1] * gy + H[2]) / w,
        (H[3] * gx + H[4] * gy + H[5]) / w
    };
}

std::array<double, 9> SPAFarmTaskPlugin::invert_homography(const std::array<double, 9>& H)
{
    // Exact 1:1 port of `os(a)` in service.pretty.mjs lines 6486-6489
    const double s = H[0], e = H[1], t = H[2];
    const double f = H[3], w = H[4], i = H[5];
    const double m = H[6], u = H[7], c = H[8];
    const double l = w * c - i * u;
    const double E = -(f * c - i * m);
    const double d = f * u - w * m;
    const double g = s * l + e * E + t * d;
    if (std::abs(g) < 1e-15) {
        return { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    }
    return {
        l / g, -(e * c - t * u) / g, (e * i - t * w) / g,
        E / g, (s * c - t * m) / g, -(s * i - t * f) / g,
        d / g, -(s * u - e * m) / g, (s * w - e * f) / g
    };
}

std::array<double, 9> SPAFarmTaskPlugin::adjust_homography(
    const std::array<double, 9>& H,
    const SimilarityFit& fit)
{
    // Exact 1:1 port of `wn(a, s)` in service.pretty.mjs lines 4551-4560
    const double a = fit.scale * fit.cos_a;
    const double b = -fit.scale * fit.sin_a;
    const double c = fit.tx;
    const double d = fit.scale * fit.sin_a;
    const double e = fit.scale * fit.cos_a;
    const double f = fit.ty;
    return {
        a * H[0] + b * H[3] + c * H[6],
        a * H[1] + b * H[4] + c * H[7],
        a * H[2] + b * H[5] + c * H[8],
        d * H[0] + e * H[3] + f * H[6],
        d * H[1] + e * H[4] + f * H[7],
        d * H[2] + e * H[5] + f * H[8],
        H[6],
        H[7],
        H[8]
    };
}

std::optional<std::tuple<double, double, int>> SPAFarmTaskPlugin::detect_marker_centroid(
    const cv::Mat& img,
    const std::array<double, 2>& ref_pt,
    int top_offset,
    std::string_view color_kind)
{
    // 1:1 port of `Un(a, s, e, t, f = 110)` in service.pretty.mjs L4444-4478.
    //
    // 旧实现错用了 goal-only 兜底分支的固定 ROI（[450,415,740,610] 等）并自拟了颜色阈值。
    // 1920x1080 实机实测：蓝点 E 只统计到 170 px（门限 0.5*2568=1284），必然被丢弃，
    // 只剩两个红门参与拟合，角度偏差 -8.8° 被判失败 -> 整段部署被静默跳过。
    // 此处按原实现重写：
    //   ROI    = 参考质心 +/- 110（E 的上边界再收 60）
    //   排除区 = di（顶部信息条、右侧面板）
    //   颜色   = qn / hi
    if (img.empty()) {
        return std::nullopt;
    }
    constexpr int kHalf = 110;
    static constexpr std::array<std::array<int, 4>, 2> kExcludeZones = {
        std::array<int, 4> { 0, 0, 1920, 110 },
        std::array<int, 4> { 1680, 0, 1920, 1080 }
    };

    const int x0 = std::clamp(static_cast<int>(std::lround(ref_pt[0] - kHalf)), 0, img.cols);
    const int x1 = std::clamp(static_cast<int>(std::lround(ref_pt[0] + kHalf)), 0, img.cols);
    const int y0 = std::clamp(static_cast<int>(std::lround(ref_pt[1] - top_offset)), 0, img.rows);
    const int y1 = std::clamp(static_cast<int>(std::lround(ref_pt[1] + kHalf)), 0, img.rows);

    const bool is_blue = (color_kind == "blue");
    double sx = 0.0;
    double sy = 0.0;
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        const auto* row = img.ptr<cv::Vec3b>(y);
        for (int x = x0; x < x1; ++x) {
            bool excluded = false;
            for (const auto& zone : kExcludeZones) {
                if (x >= zone[0] && y >= zone[1] && x < zone[2] && y < zone[3]) {
                    excluded = true;
                    break;
                }
            }
            if (excluded) {
                continue;
            }
            const int B = row[x][0];
            const int G = row[x][1];
            const int R = row[x][2];
            // `qn` / `hi` in service.pretty.mjs L4441-4442
            const bool match = is_blue
                ? (B > 220 && G > 140 && R < 150 && (B - R) > 90)
                : (R > 200 && G < 110 && B < 110 && (R - G) > 120);
            if (match) {
                ++n;
                sx += x;
                sy += y;
            }
        }
    }
    if (n == 0) {
        return std::nullopt;
    }
    return std::make_tuple(sx / n, sy / n, n);
}

SPAFarmTaskPlugin::SimilarityFit SPAFarmTaskPlugin::fit_similarity(
    const std::vector<std::pair<double, double>>& src_pts,
    const std::vector<std::pair<double, double>>& dst_pts)
{
    // Exact 1:1 port of `pn(a, s)` in service.pretty.mjs lines 4509-4549
    SimilarityFit res;
    const size_t n = std::min(src_pts.size(), dst_pts.size());
    if (n < 2) {
        return res;
    }
    double mx = 0.0, my = 0.0, nx = 0.0, ny = 0.0;
    for (size_t i = 0; i < n; ++i) {
        mx += src_pts[i].first;
        my += src_pts[i].second;
        nx += dst_pts[i].first;
        ny += dst_pts[i].second;
    }
    mx /= static_cast<double>(n);
    my /= static_cast<double>(n);
    nx /= static_cast<double>(n);
    ny /= static_cast<double>(n);

    double M = 0.0, C = 0.0, T = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double dx = src_pts[i].first - mx;
        const double dy = src_pts[i].second - my;
        const double ux = dst_pts[i].first - nx;
        const double uy = dst_pts[i].second - ny;
        M += dx * dx + dy * dy;
        C += dx * ux + dy * uy;
        T += dx * uy - dy * ux;
    }
    if (M < 1e-6) {
        return res;
    }
    const double scale = std::hypot(C, T) / M;
    if (scale < 1e-6) {
        return res;
    }
    const double cos_a = C / (M * scale);
    const double sin_a = T / (M * scale);
    const double tx = nx - scale * (cos_a * mx - sin_a * my);
    const double ty = ny - scale * (sin_a * mx + cos_a * my);

    double max_err = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double px = scale * (cos_a * src_pts[i].first - sin_a * src_pts[i].second) + tx;
        const double py = scale * (sin_a * src_pts[i].first + cos_a * src_pts[i].second) + ty;
        max_err = std::max(max_err, std::hypot(px - dst_pts[i].first, py - dst_pts[i].second));
    }

    res.valid = true;
    res.matched_markers = static_cast<int>(n);
    res.scale = scale;
    res.cos_a = cos_a;
    res.sin_a = sin_a;
    res.tx = tx;
    res.ty = ty;
    res.err = max_err;
    res.angle_deg = std::atan2(sin_a, cos_a) * 180.0 / 3.14159265358979323846;
    return res;
}

SPAFarmTaskPlugin::CameraDetectResult SPAFarmTaskPlugin::detect_camera(
    const cv::Mat& img,
    bool allow_goal_only) const
{
    // 1:1 port of `Hn` (service.pretty.mjs L4526-4557) + `Sr` (L4584-4684).
    //
    // 旧实现与原实现的三处偏离，均已改正：
    //   1. marker 可用条件只有 `n >= max(150, 0.5 * markerRefN)`；没有 [0.35, 2.5] 区间，
    //      也没有 allow_goal_only 时把 E 放宽到 0.2 的特例；
    //   2. 三个 marker 都可用且残差 > 15 时，剔除残差最大的一个再重拟合；
    //   3. 接受条件是 `|scale - 1| <= 0.025` 且 `|平移| <= 45px`
    //      （不是 scale in [0.85,1.15] + |angle| <= 8 度 + 残差 <= 10）；
    //      两个镜头都评估，取 |scale - 1| 最小的那个。
    struct MarkerSlot
    {
        std::string_view id;
        std::array<double, 2> ref = { 0.0, 0.0 };
        int ref_n = 1;
        int top = 110;
        bool blue = false;
    };

    auto eval_calib = [&](const CameraCalibData& calib) -> CameraDetectResult {
        CameraDetectResult out;
        out.camera = calib.camera;
        const MarkerSlot slots[3] = {
            { "E", calib.marker_E, calib.ref_n_E, 60, true },
            { "S1", calib.marker_S1, calib.ref_n_S1, 110, false },
            { "S2", calib.marker_S2, calib.ref_n_S2, 110, false }
        };

        std::vector<std::pair<double, double>> src_pts;
        std::vector<std::pair<double, double>> dst_pts;
        std::string detail = calib.camera + "[";

        for (const auto& slot : slots) {
            auto det = detect_marker_centroid(img, slot.ref, slot.top, slot.blue ? "blue" : "red");
            const int n = det ? std::get<2>(*det) : 0;
            const double need = std::max(150.0, 0.5 * static_cast<double>(slot.ref_n));
            const bool usable = det.has_value() && static_cast<double>(n) >= need;
            detail += std::string(slot.id) + "=";
            if (det) {
                detail += "(" + std::to_string(static_cast<int>(std::lround(std::get<0>(*det)))) + ","
                        + std::to_string(static_cast<int>(std::lround(std::get<1>(*det)))) + ")n"
                        + std::to_string(n) + "/" + std::to_string(static_cast<int>(std::lround(need)));
            }
            else {
                detail += "n0/" + std::to_string(static_cast<int>(std::lround(need)));
            }
            detail += usable ? " OK " : " XX ";
            if (usable) {
                src_pts.emplace_back(slot.ref[0], slot.ref[1]);
                dst_pts.emplace_back(std::get<0>(*det), std::get<1>(*det));
            }
        }

        if (src_pts.size() < 2) {
            out.detail = detail + "] 可用标记不足(" + std::to_string(src_pts.size()) + ")";
            return out;
        }

        SimilarityFit fit = fit_similarity(src_pts, dst_pts);
        if (src_pts.size() >= 3 && fit.err > 15.0) {
            size_t worst = 0;
            double worst_err = -1.0;
            for (size_t i = 0; i < src_pts.size(); ++i) {
                const double px = fit.scale * (fit.cos_a * src_pts[i].first - fit.sin_a * src_pts[i].second) + fit.tx;
                const double py = fit.scale * (fit.sin_a * src_pts[i].first + fit.cos_a * src_pts[i].second) + fit.ty;
                const double err_i = std::hypot(px - dst_pts[i].first, py - dst_pts[i].second);
                if (err_i > worst_err) {
                    worst_err = err_i;
                    worst = i;
                }
            }
            const auto at = static_cast<std::ptrdiff_t>(worst);
            src_pts.erase(src_pts.begin() + at);
            dst_pts.erase(dst_pts.begin() + at);
            fit = fit_similarity(src_pts, dst_pts);
            detail += "剔除残差点 ";
        }

        if (!fit.valid) {
            out.detail = detail + "] 拟合失败";
            return out;
        }

        out.fit = fit;
        out.H = adjust_homography(calib.base_H, fit);
        out.detail = detail + "] 用" + std::to_string(fit.matched_markers) + "点 scale="
            + std::to_string(fit.scale) + " 平移="
            + std::to_string(static_cast<int>(std::lround(std::hypot(fit.tx, fit.ty)))) + " 残差="
            + std::to_string(static_cast<int>(std::lround(fit.err)));
        return out;
    };

    CameraDetectResult exp_res = eval_calib(m_calib_expanded);
    CameraDetectResult col_res = eval_calib(m_calib_collapsed);

    const CameraDetectResult* best = nullptr;
    for (const auto* cand : { &exp_res, &col_res }) {
        if (!cand->fit.valid) {
            continue;
        }
        if (best == nullptr || std::abs(cand->fit.scale - 1.0) < std::abs(best->fit.scale - 1.0)) {
            best = cand;
        }
    }

    CameraDetectResult out;
    const std::string all = exp_res.detail + " || " + col_res.detail;

    // `Sr` 的 goal-only 兜底分支（service.pretty.mjs L4591-4664）：
    // 主路径失败时，用蓝色出生点的连通域像素块重做一次相似变换。
    if (allow_goal_only) {
        const bool need_fallback = (best == nullptr)
            || std::abs(best->fit.scale - 1.0) > 0.025
            || std::hypot(best->fit.tx, best->fit.ty) > 45.0;
        if (need_fallback) {
            if (auto fb = detect_goal_only_fallback(img); fb.has_value()) {
                fb->detail = all + " -> goal-only " + fb->camera;
                return *fb;
            }
        }
    }

    if (best == nullptr) {
        out.detail = all + " -> 找不到蓝点/红门标记";
        return out;
    }

    const double dev = std::abs(best->fit.scale - 1.0);
    const double shift = std::hypot(best->fit.tx, best->fit.ty);
    if (dev > 0.025 || shift > 45.0) {
        out.detail = all + " -> 与所有已标定镜头都不符";
        return out;
    }

    out.camera = best->camera;
    out.H = best->H;
    out.fit = best->fit;
    out.detail = all + " -> " + best->camera;
    if (allow_goal_only) {
        out.detail += " (goal-only)";
    }
    return out;
}

std::optional<SPAFarmTaskPlugin::CameraDetectResult> SPAFarmTaskPlugin::detect_goal_only_fallback(
    const cv::Mat& img) const
{
    // 1:1 port of the `allowGoalOnly` branch inside `Sr` (service.pretty.mjs L4591-4664).
    //
    // 只在「Boss 回合 / 最后一回合」且本局地图不是默认图时启用：此时画面可能只剩出生点
    // 附近的棋盘，红门不在视野内。做法是在固定 ROI 里取蓝色像素，按 5x5 邻域做连通域标记，
    // 丢掉碎块，用保留像素的 1% / 99% 分位点围成的矩形与标定 box 拟合相似变换。
    struct GoalOnlySpec
    {
        std::string_view camera;
        std::array<int, 4> roi;
        std::array<double, 4> box;
    };
    static constexpr GoalOnlySpec kSpecs[2] = {
        { "expanded",
          std::array<int, 4> { 450, 415, 740, 610 },
          std::array<double, 4> { 493.8044, 430.6554, 621.3322, 560.1759 } },
        { "collapsed",
          std::array<int, 4> { 410, 520, 690, 770 },
          std::array<double, 4> { 459.5028, 544.5004, 615.0266, 700.0242 } }
    };

    if (img.empty()) {
        return std::nullopt;
    }

    for (const auto& spec : kSpecs) {
        const int roi_x0 = spec.roi[0];
        const int roi_y0 = spec.roi[1];
        const int roi_w = spec.roi[2] - spec.roi[0];
        const int roi_h = spec.roi[3] - spec.roi[1];
        if (roi_w <= 0 || roi_h <= 0
            || spec.roi[3] > img.rows || spec.roi[2] > img.cols) {
            continue;
        }

        const size_t total = static_cast<size_t>(roi_w) * static_cast<size_t>(roi_h);
        std::vector<unsigned char> mask(total, 0);
        int blue_count = 0;
        for (int row = 0; row < roi_h; ++row) {
            const auto* src = img.ptr<cv::Vec3b>(roi_y0 + row);
            for (int col = 0; col < roi_w; ++col) {
                const int B = src[roi_x0 + col][0];
                const int G = src[roi_x0 + col][1];
                const int R = src[roi_x0 + col][2];
                if (B > 220 && G > 140 && R < 150 && (B - R) > 90) {
                    mask[static_cast<size_t>(row) * static_cast<size_t>(roi_w) + static_cast<size_t>(col)] = 1;
                    ++blue_count;
                }
            }
        }
        if (blue_count == 0) {
            continue;
        }

        std::vector<int> labels(total, 0);
        std::vector<unsigned char> keep; // 按 label 索引，0 = 背景
        keep.push_back(0);
        int next_label = 0;
        std::vector<int> stack;
        for (size_t seed = 0; seed < total; ++seed) {
            if (mask[seed] == 0 || labels[seed] != 0) {
                continue;
            }
            ++next_label;
            keep.push_back(0);
            labels[seed] = next_label;
            stack.clear();
            stack.push_back(static_cast<int>(seed));
            int min_c = roi_w;
            int max_c = 0;
            int min_r = roi_h;
            int max_r = 0;
            while (!stack.empty()) {
                const int cur = stack.back();
                stack.pop_back();
                const int col = cur % roi_w;
                const int row = (cur - col) / roi_w;
                min_c = std::min(min_c, col);
                max_c = std::max(max_c, col);
                min_r = std::min(min_r, row);
                max_r = std::max(max_r, row);
                for (int dr = -2; dr <= 2; ++dr) {
                    for (int dc = -2; dc <= 2; ++dc) {
                        const int nc = col + dc;
                        const int nr = row + dr;
                        if (nc < 0 || nr < 0 || nc >= roi_w || nr >= roi_h) {
                            continue;
                        }
                        const int ni = nr * roi_w + nc;
                        if (mask[ni] != 0 && labels[ni] == 0) {
                            labels[ni] = next_label;
                            stack.push_back(ni);
                        }
                    }
                }
            }
            keep[static_cast<size_t>(next_label)] =
                (max_c - min_c > 48 || max_r - min_r > 48) ? 1 : 0;
        }

        std::vector<int> xs;
        std::vector<int> ys;
        for (int row = 0; row < roi_h; ++row) {
            for (int col = 0; col < roi_w; ++col) {
                const size_t idx = static_cast<size_t>(row) * static_cast<size_t>(roi_w)
                    + static_cast<size_t>(col);
                const int lb = labels[idx];
                if (lb > 0 && keep[static_cast<size_t>(lb)] != 0) {
                    xs.push_back(col + roi_x0);
                    ys.push_back(row + roi_y0);
                }
            }
        }
        if (xs.size() < 1000 || xs.size() > 7000) {
            continue;
        }
        std::sort(xs.begin(), xs.end());
        std::sort(ys.begin(), ys.end());

        const size_t lo_x = static_cast<size_t>(static_cast<double>(xs.size()) * 0.01);
        const size_t hi_x = static_cast<size_t>(static_cast<double>(xs.size()) * 0.99);
        const size_t lo_y = static_cast<size_t>(static_cast<double>(ys.size()) * 0.01);
        const size_t hi_y = static_cast<size_t>(static_cast<double>(ys.size()) * 0.99);
        const std::array<double, 4> box_pts = {
            static_cast<double>(xs[lo_x]),
            static_cast<double>(ys[lo_y]),
            static_cast<double>(xs[hi_x]),
            static_cast<double>(ys[hi_y])
        };

        const double sx = (box_pts[2] - box_pts[0]) / (spec.box[2] - spec.box[0]);
        const double sy = (box_pts[3] - box_pts[1]) / (spec.box[3] - spec.box[1]);
        if (std::abs(sx - 1.0) > 0.04 || std::abs(sy - 1.0) > 0.04 || std::abs(sx - sy) > 0.04) {
            continue;
        }

        const std::vector<std::pair<double, double>> src_pts = {
            { spec.box[0], spec.box[1] },
            { spec.box[2], spec.box[3] }
        };
        const std::vector<std::pair<double, double>> dst_pts = {
            { box_pts[0], box_pts[1] },
            { box_pts[2], box_pts[3] }
        };
        const SimilarityFit fit = fit_similarity(src_pts, dst_pts);
        if (!fit.valid) {
            continue;
        }
        if (std::abs(fit.scale - 1.0) <= 0.025 && std::abs(fit.tx) <= 85.0
            && std::abs(fit.ty) <= 45.0 && fit.err <= 4.0) {
            const CameraCalibData& base =
                (spec.camera == "collapsed") ? m_calib_collapsed : m_calib_expanded;
            CameraDetectResult out;
            out.camera = std::string(spec.camera);
            out.fit = fit;
            out.fit.matched_markers = 0;
            out.H = adjust_homography(base.base_H, fit);
            return out;
        }
    }
    return std::nullopt;
}

bool SPAFarmTaskPlugin::goal_only_allowed() const
{
    // `(!!info.boss || round === lastRound) && !!stageSource && stageSource !== "default"`
    // `turns` 表共 9 回合，第 9 回合为 Boss 回合（resource/SPAFarm/farm-rules.json）。
    constexpr int kLastRound = 9;
    const bool boss_round = !m_current_boss.empty() || m_current_round >= kLastRound;
    return boss_round && !m_stage_source.empty() && m_stage_source != "default";
}

std::array<double, 9> SPAFarmTaskPlugin::active_board_homography(const CameraDetectResult& cam) const
{
    if (cam.camera == "collapsed") {
        return adjust_homography(m_collapsed_board_H, cam.fit);
    }
    return cam.H;
}

std::optional<SPAFarmTaskPlugin::StableBoardState> SPAFarmTaskPlugin::wait_stable_board(
    int timeout_ms,
    const std::optional<std::array<double, 4>>& prev_pts)
{
    // Exact 1:1 port of `Dt(j = 5e3, re = null)` in service.pretty.mjs lines 7363-7385:
    // Loops taking screenshots until `!is_detail_panel_open(Me)`, detects camera `Sr`,
    // projects board corners `(2.5, 1.5)` and `(9.5, 6.5)`, and requires 2 consecutive frames
    // to have all 4 coordinates within `< 4.0` px displacement. Returns `std::nullopt` on timeout.
    const auto t0 = std::chrono::steady_clock::now();
    std::optional<std::array<double, 4>> ce = prev_pts;
    while (!need_exit()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        if (elapsed >= timeout_ms) {
            break;
        }
        cv::Mat Me = shot();
        if (!Me.empty() && !is_detail_panel_open(Me)) {
            const bool allow_goal_only = goal_only_allowed();
            CameraDetectResult we = detect_camera(Me, allow_goal_only);
            if (!we.camera.empty()) {
                auto [x1, y1] = project_point(we.H, 2.5, 1.5);
                auto [x2, y2] = project_point(we.H, 9.5, 6.5);
                std::array<double, 4> je = { x1, y1, x2, y2 };
                if (ce.has_value()) {
                    bool stable = true;
                    for (size_t i = 0; i < 4; ++i) {
                        if (std::abs(je[i] - (*ce)[i]) >= 4.0) {
                            stable = false;
                            break;
                        }
                    }
                    if (stable) {
                        return StableBoardState {
                            active_board_homography(we),
                            we.H,
                            we.camera,
                            we.fit,
                            je,
                            Me
                        };
                    }
                }
                ce = je;
            }
            else {
                ce.reset();
            }
        }
        sleep_ms(static_cast<int>(std::lround(100.0 * m_step_scale)));
    }
    return std::nullopt;
}

// ============================================================================
// Stage Identification & Info Screen (`Ri`, `ji`, `Ni`, `ki`, `lr`, `F`)
// ============================================================================

std::unordered_map<std::string, std::vector<double>> SPAFarmTaskPlugin::sample_board_cells(
    const cv::Mat& img,
    const std::array<double, 9>& H) const
{
    // Exact 1:1 port of `Ri(a, s)` in service.pretty.mjs lines 5539-5568
    static constexpr double kOffsets[8][2] = {
        { -0.3, -0.3 }, { 0.0, -0.3 }, { 0.3, -0.3 },
        { -0.3, 0.0 },                 { 0.3, 0.0 },
        { -0.3, 0.3 },  { 0.0, 0.3 },  { 0.3, 0.3 }
    };
    std::unordered_map<std::string, std::vector<double>> out;
    if (img.empty()) {
        return out;
    }
    for (int r = 1; r <= 4; ++r) {
        for (int c = 0; c < 13; ++c) {
            std::vector<double> vec;
            vec.reserve(24);
            for (const auto& off : kOffsets) {
                auto [px, py] = project_point(H, c + 0.5 + off[0], r + 0.5 + off[1]);
                double sr = 0.0, sg = 0.0, sb = 0.0;
                int cnt = 0;
                for (int dy = -3; dy <= 3; ++dy) {
                    for (int dx = -3; dx <= 3; ++dx) {
                        int x = static_cast<int>(std::lround(px + dx));
                        int y = static_cast<int>(std::lround(py + dy));
                        if (x < 0 || y < 0 || x >= img.cols || y >= img.rows) {
                            continue;
                        }
                        const auto& p = img.at<cv::Vec3b>(y, x);
                        const int B = p[0], G = p[1], R = p[2];
                        if ((R > 200 && G < 80 && B > 200)
                            || (R < 80 && G > 200 && B > 200)
                            || (R + G + B < 30)) {
                            continue;
                        }
                        sr += R;
                        sg += G;
                        sb += B;
                        ++cnt;
                    }
                }
                if (cnt > 0) {
                    vec.push_back(sr / cnt);
                    vec.push_back(sg / cnt);
                    vec.push_back(sb / cnt);
                }
                else {
                    vec.push_back(std::numeric_limits<double>::quiet_NaN());
                    vec.push_back(std::numeric_limits<double>::quiet_NaN());
                    vec.push_back(std::numeric_limits<double>::quiet_NaN());
                }
            }
            out[std::to_string(r) + "," + std::to_string(c)] = std::move(vec);
        }
    }
    return out;
}

void SPAFarmTaskPlugin::update_stage_from_board(
    const cv::Mat& img,
    const std::array<double, 9>& H,
    std::string_view camera,
    const std::set<std::string>& occupied_cells)
{
    // Exact 1:1 port of `ji` + `Ni` + `Tt` in service.pretty.mjs lines 5570-5596 & 7097-7128
    if (m_stage_source == "thumbnail" || m_board_refs.empty()) {
        return;
    }
    auto sampled = sample_board_cells(img, H);
    struct CandScore
    {
        std::string stage_id;
        double score = 1e9;
    };
    std::vector<CandScore> scores;
    for (const auto& entry : m_board_refs) {
        if (entry.camera != camera) {
            continue;
        }
        std::vector<double> cell_dists;
        for (const auto& [cell_key, sample_vec] : sampled) {
            if (occupied_cells.count(cell_key) > 0) {
                continue;
            }
            auto it = entry.cells.find(cell_key);
            if (it == entry.cells.end()) {
                continue;
            }
            const auto& ref_vec = it->second;
            std::vector<double> pt_dists;
            for (size_t i = 0; i + 2 < sample_vec.size() && i + 2 < ref_vec.size(); i += 3) {
                if (std::isnan(sample_vec[i]) || std::isnan(ref_vec[i])) {
                    continue;
                }
                const double dr = sample_vec[i] - ref_vec[i];
                const double dg = sample_vec[i + 1] - ref_vec[i + 1];
                const double db = sample_vec[i + 2] - ref_vec[i + 2];
                pt_dists.push_back(std::sqrt(dr * dr + dg * dg + db * db));
            }
            if (!pt_dists.empty()) {
                std::sort(pt_dists.begin(), pt_dists.end());
                cell_dists.push_back(pt_dists[pt_dists.size() / 2]);
            }
        }
        if (!cell_dists.empty()) {
            std::sort(cell_dists.begin(), cell_dists.end());
            scores.push_back(CandScore { entry.stage_id, cell_dists[cell_dists.size() / 2] });
        }
    }
    if (scores.empty()) {
        return;
    }
    std::sort(scores.begin(), scores.end(), [](const CandScore& a, const CandScore& b) {
        return a.score < b.score;
    });
    const bool confident = (scores[0].score < 20.0)
        && (scores.size() < 2 || (scores[1].score - scores[0].score) >= 6.0);
    if (!confident) {
        m_stage_vote = { "", 0 };
        return;
    }
    if (m_stage_vote.first == scores[0].stage_id) {
        m_stage_vote.second++;
    }
    else {
        m_stage_vote = { scores[0].stage_id, 1 };
    }
    if (scores[0].stage_id == m_stage_id) {
        if (m_stage_source == "default") {
            m_stage_source = "board";
        }
        return;
    }
    if (m_stage_vote.second >= 2) {
        m_stage_id = scores[0].stage_id;
        m_stage_source = "board";
        m_empty_cell_refs.clear();
        log_info("识别到当前关卡地图: " + m_stage_id);
    }
}

std::optional<SPAFarmTaskPlugin::InfoScreenData> SPAFarmTaskPlugin::read_info_screen(const cv::Mat& img) const
{
    // Exact 1:1 port of `ki` in service.pretty.mjs lines 5404-5492
    if (img.empty()) {
        return std::nullopt;
    }
    auto left_items = ocr(img, std::array<int, 4> { 0, 160, 1350, 860 });
    auto right_items = ocr(img, std::array<int, 4> { 1340, 400, 580, 460 });

    InfoScreenData out;
    for (const auto& it : left_items) {
        auto matches = match_dict(it.text, m_bond_dict);
        if (matches.empty() || match_coverage(it.text, matches) < 0.6) {
            continue;
        }
        for (const auto& m : matches) {
            double mid_x = it.x + it.w * (m.start + m.end) * 0.5 / std::max(1, m.len);
            double gr = green_hp_bar_ratio(img, mid_x, it.y, it.h);
            bool exists = false;
            for (const auto& b : out.bonds) {
                if (b.name == m.name) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                const auto* binfo = find_bond(m.name);
                out.bonds.push_back(InfoScreenData::BondStatus {
                    m.name,
                    gr >= 0.15,
                    gr,
                    binfo ? binfo->is_core : false });
            }
        }
    }

    auto leader_hit = find_ocr(right_items, { "敌方领袖" });
    if (leader_hit) {
        std::string boss_concat;
        for (const auto& it : right_items) {
            if (it.y > leader_hit->y + leader_hit->h * 0.5 && it.x >= leader_hit->x - 120) {
                if (it.text.find("特训") != std::string::npos) {
                    for (const auto& et : m_enemy_types) {
                        if (it.text.find(et.name) != std::string::npos) {
                            out.enemy_types.push_back(et.name);
                        }
                    }
                }
                else if (out.enemy_types.empty()) {
                    boss_concat += it.text;
                }
            }
        }
        auto boss_matches = match_dict(boss_concat, m_boss_dict);
        if (!boss_matches.empty()) {
            out.boss = boss_matches.front().name;
        }
    }

    if (out.bonds.empty() && !leader_hit.has_value()) {
        return std::nullopt;
    }
    return out;
}

std::pair<std::optional<int>, bool> SPAFarmTaskPlugin::read_round_and_prep(
    const cv::Mat& img,
    const std::vector<OcrItem>& full_items) const
{
    // Exact 1:1 port of `lr(j)` in service.pretty.mjs lines 7214-7264
    std::optional<int> round_num;
    auto parse_round_from = [&](const std::vector<OcrItem>& items) -> std::optional<int> {
        for (const auto& it : items) {
            auto pos = it.text.find("回合");
            if (pos != std::string::npos) {
                std::string d = extract_digits_mapped(it.text.substr(pos));
                if (!d.empty()) {
                    int v = std::stoi(d);
                    if (v >= 1 && v <= 30) {
                        return v;
                    }
                }
            }
        }
        for (const auto& it : items) {
            std::string d = extract_digits_mapped(it.text);
            if (!d.empty() && d.size() <= 2 && it.score >= 0.7) {
                int v = std::stoi(d);
                if (v >= 1 && v <= 30) {
                    return v;
                }
            }
        }
        return std::nullopt;
    };

    auto r1 = ocr(img, std::array<int, 4> { 690, 15, 130, 75 });
    round_num = parse_round_from(r1);
    if (!round_num) {
        auto r2 = ocr_rec_only(img, { 690, 15, 130, 75 }, 3, 0);
        round_num = parse_round_from(r2);
    }
    if (!round_num) {
        round_num = parse_round_from(full_items);
    }

    auto phase_items = ocr(img, std::array<int, 4> { 900, 15, 210, 75 });
    bool is_prep = has_ocr(phase_items, { "休息", "休整" })
        || (has_ocr(full_items, { "准备就绪" })
            && has_ocr(full_items, { "刷新", "升级", "已满级", "满级", "收起", "展开" }));
    return { round_num, is_prep };
}

std::optional<int> SPAFarmTaskPlugin::read_remaining_deploy(const cv::Mat& img) const
{
    // Exact 1:1 port of `F(j)` in service.pretty.mjs lines 7265-7278
    auto items = ocr(img, std::array<int, 4> { 750, 130, 420, 55 });
    std::string joined;
    for (const auto& it : items) {
        joined += it.text;
    }
    auto pos = joined.find("剩余可部署");
    if (pos != std::string::npos) {
        std::string d = extract_digits_mapped(joined.substr(pos));
        if (!d.empty()) {
            return d[0] - '0';
        }
    }
    return std::nullopt;
}

// ============================================================================
// Shop Reader (`Kr`, `Kn`, `Rt`, `Bi`, `Ci`, `Di`, `Mt`)
// ============================================================================

std::optional<int> SPAFarmTaskPlugin::read_shop_digits_4x(
    const cv::Mat& img,
    const std::array<int, 4>& roi_xywh) const
{
    // Exact 1:1 port of `Kr(a, [s, e, t, f])` in service.pretty.mjs lines 4815-4824
    auto items = ocr_rec_only(img, roi_xywh, 4, 0);
    std::string digits;
    for (const auto& it : items) {
        digits += extract_digits_mapped(it.text);
    }
    if (digits.empty()) {
        return std::nullopt;
    }
    return std::stoi(digits);
}

std::optional<int> SPAFarmTaskPlugin::recognize_shop_level_roman_cc(const cv::Mat& img, int up_x)
{
    // Exact 1:1 port of `Kn(a, s)` in service.pretty.mjs lines 4945-5033:
    // Extracts [left: round(up_x - 55), top: 845, width: 110, height: 100] in grayscale,
    // computes Otsu threshold, finds 4-connected bright components, and classifies each component
    // into 'V' (if top 30% rows have >= 2 horizontal foreground runs in >= 3 rows) or 'I'.
    if (img.empty()) {
        return std::nullopt;
    }
    const int W = 110;
    const int H = 100;
    const int left = std::clamp(static_cast<int>(std::lround(up_x - 55)), 0, std::max(0, img.cols - W));
    const int top = 845;
    if (left + W > img.cols || top + H > img.rows) {
        return std::nullopt;
    }

    cv::Mat crop_bgr = img(cv::Rect(left, top, W, H));
    cv::Mat gray;
    cv::cvtColor(crop_bgr, gray, cv::COLOR_BGR2GRAY);

    // Otsu threshold (`Kn` lines 4955-4972)
    int hist[256] = {};
    double sum_all = 0.0;
    const int total_px = W * H;
    for (int y = 0; y < H; ++y) {
        const uint8_t* row = gray.ptr<uint8_t>(y);
        for (int x = 0; x < W; ++x) {
            hist[row[x]]++;
            sum_all += row[x];
        }
    }
    int wB = 0;
    double sumB = 0.0;
    double max_var = 0.0;
    int otsu_t = 180;
    for (int t = 0; t < 256; ++t) {
        wB += hist[t];
        if (wB == 0) {
            continue;
        }
        int wF = total_px - wB;
        if (wF == 0) {
            break;
        }
        sumB += static_cast<double>(t) * hist[t];
        const double mB = sumB / wB;
        const double mF = (sum_all - sumB) / wF;
        const double var_between = static_cast<double>(wB) * static_cast<double>(wF) * (mB - mF) * (mB - mF);
        if (var_between > max_var) {
            max_var = var_between;
            otsu_t = t;
        }
    }
    if (otsu_t < 100) {
        return std::nullopt;
    }

    std::vector<uint8_t> bin(total_px, 0);
    for (int y = 0; y < H; ++y) {
        const uint8_t* row = gray.ptr<uint8_t>(y);
        for (int x = 0; x < W; ++x) {
            if (row[x] > otsu_t) {
                bin[y * W + x] = 1;
            }
        }
    }

    struct Comp
    {
        int x0 = W;
        int y0 = H;
        int x1 = 0;
        int y1 = 0;
        int n = 0;
    };

    std::vector<uint8_t> vis(total_px, 0);
    std::vector<Comp> raw_comps;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int idx = y * W + x;
            if (!bin[idx] || vis[idx]) {
                continue;
            }
            Comp c;
            std::vector<int> stack = { idx };
            vis[idx] = 1;
            while (!stack.empty()) {
                int cur = stack.back();
                stack.pop_back();
                int cx = cur % W;
                int cy = cur / W;
                c.x0 = std::min(c.x0, cx);
                c.x1 = std::max(c.x1, cx);
                c.y0 = std::min(c.y0, cy);
                c.y1 = std::max(c.y1, cy);
                c.n++;
                const int nbrs[4][2] = { { cx - 1, cy }, { cx + 1, cy }, { cx, cy - 1 }, { cx, cy + 1 } };
                for (const auto& nb : nbrs) {
                    int nx = nb[0], ny = nb[1];
                    if (nx >= 0 && nx < W && ny >= 0 && ny < H) {
                        int nidx = ny * W + nx;
                        if (bin[nidx] && !vis[nidx]) {
                            vis[nidx] = 1;
                            stack.push_back(nidx);
                        }
                    }
                }
            }
            raw_comps.push_back(c);
        }
    }

    auto filter_comps = [&](bool strict_h) {
        std::vector<Comp> out;
        for (const auto& c : raw_comps) {
            if (c.x0 > 1 && c.y0 > 1 && c.x1 < W - 2 && c.y1 < H - 2
                && (c.y1 - c.y0) >= (strict_h ? 28 : 20) && c.n >= 80) {
                out.push_back(c);
            }
        }
        std::sort(out.begin(), out.end(), [](const Comp& a, const Comp& b) {
            return a.x0 < b.x0;
        });
        return out;
    };

    auto comps = filter_comps(true);
    if (comps.empty()) {
        comps = filter_comps(false);
    }
    if (comps.empty() || comps.size() > 3) {
        return std::nullopt;
    }

    int min_y0 = H, max_y1 = 0;
    for (const auto& c : comps) {
        min_y0 = std::min(min_y0, c.y0);
        max_y1 = std::max(max_y1, c.y1);
    }
    if (max_y1 - min_y0 > 50) {
        return std::nullopt;
    }

    std::string roman;
    for (const auto& c : comps) {
        int top_band_end = c.y0 + std::max(4, static_cast<int>(std::floor((c.y1 - c.y0) * 0.3)));
        int two_stroke_rows = 0;
        for (int y = c.y0; y <= top_band_end && y <= c.y1; ++y) {
            int runs = 0;
            bool in_run = false;
            for (int x = c.x0; x <= c.x1; ++x) {
                bool fg = bin[y * W + x] != 0;
                if (fg && !in_run) {
                    ++runs;
                }
                in_run = fg;
            }
            if (runs >= 2) {
                ++two_stroke_rows;
            }
        }
        roman += (two_stroke_rows >= 3) ? "V" : "I";
    }

    static const std::unordered_map<std::string, int> kRomanMap = {
        { "I", 1 }, { "II", 2 }, { "III", 3 }, { "IV", 4 }, { "V", 5 }, { "VI", 6 }
    };
    auto it = kRomanMap.find(roman);
    if (it != kRomanMap.end()) {
        return it->second;
    }
    return std::nullopt;
}

SPAFarmTaskPlugin::ShopHeaderState SPAFarmTaskPlugin::read_shop_header(
    const cv::Mat& img,
    const std::vector<OcrItem>& full_items) const
{
    // Exact 1:1 port of `Rt(a)` in service.pretty.mjs lines 4826-4890
    ShopHeaderState st;
    if (img.empty()) {
        return st;
    }

    st.gold = read_shop_digits_4x(img, { 1767, 776, 66, 32 });
    if (!st.gold) {
        st.gold = read_shop_digits_4x(img, { 1782, 777, 36, 30 });
    }

    for (const auto& it : full_items) {
        if (it.text == "刷新" || (it.text.find("刷新") != std::string::npos && it.x > 1600 && it.y > 600)) {
            st.refresh_btn = { static_cast<int>(std::lround(it.cx)), static_cast<int>(std::lround(it.cy)) };
            break;
        }
    }

    std::optional<OcrItem> up_item;
    for (const auto& it : full_items) {
        if (it.text == "升级" || (it.text.find("升级") != std::string::npos && it.y > 750 && it.x < 800)) {
            up_item = it;
            break;
        }
    }

    const bool has_max_level = has_ocr(full_items, { "已满级", "满级" });
    if (!up_item && has_ocr(full_items, { "收起" })) {
        for (int probe_x : { 337, 580 }) {
            if (auto lv = recognize_shop_level_roman_cc(img, probe_x)) {
                st.level = lv;
                return st;
            }
        }
        if (has_max_level) {
            st.level = 6;
        }
        return st;
    }

    if (!up_item) {
        if (has_max_level) {
            st.level = 6;
        }
        return st;
    }

    const int up_x = static_cast<int>(std::lround(up_item->cx));
    const int up_y = static_cast<int>(std::lround(up_item->cy));
    st.upgrade_btn = { up_x, up_y };
    st.upgrade_cost = read_shop_digits_4x(img, { up_x - 13, 786, 28, 30 });
    st.level = recognize_shop_level_roman_cc(img, up_x);

    // Fallback OCR with 3x cubic resize + 60px black border (`Rt` lines 4862-4880)
    if (!st.level) {
        auto rec_items = ocr_rec_only(img, { std::max(0, up_x - 60), 870, 120, 80 }, 3, 60);
        for (const auto& r : rec_items) {
            if (r.text.find("IV") != std::string::npos || r.text.find("Ⅳ") != std::string::npos) {
                st.level = 4;
                break;
            }
            if (r.text.find("VI") != std::string::npos || r.text.find("Ⅵ") != std::string::npos) {
                st.level = 6;
                break;
            }
            if (r.text.find("V") != std::string::npos || r.text.find("Ⅴ") != std::string::npos) {
                st.level = 5;
                break;
            }
            if (r.text.find("III") != std::string::npos || r.text.find("Ⅲ") != std::string::npos) {
                st.level = 3;
                break;
            }
            if (r.text.find("II") != std::string::npos || r.text.find("Ⅱ") != std::string::npos) {
                st.level = 2;
                break;
            }
            if (r.text.find("I") != std::string::npos || r.text.find("Ⅰ") != std::string::npos) {
                st.level = 1;
                break;
            }
        }
    }
    return st;
}

std::vector<int> SPAFarmTaskPlugin::detect_card_gaps(const cv::Mat& img)
{
    // Exact 1:1 port of `Ci(a)` + `Di(a)` in service.pretty.mjs lines 5336-5389
    if (img.empty() || img.cols < 1700 || img.rows < 1040) {
        return {};
    }
    const int W = img.cols;
    std::vector<double> col_luma(W, 0.0);
    for (int x = 250; x < 1700; ++x) {
        double s = 0.0;
        for (int y = 800; y < 1040; ++y) {
            const auto& p = img.at<cv::Vec3b>(y, x);
            s += (p[2] + p[1] + p[0]) / 3.0;
        }
        col_luma[x] = s / 240.0;
    }
    std::vector<double> smooth(W, 0.0);
    for (int x = 255; x < 1695; ++x) {
        double s = 0.0;
        for (int d = -3; d <= 3; ++d) {
            s += col_luma[x + d];
        }
        smooth[x] = s / 7.0;
    }
    std::vector<int> minima;
    for (int x = 270; x < 1680; ++x) {
        double m = 0.0;
        for (int d = -25; d <= 25; ++d) {
            m += smooth[x + d];
        }
        m /= 51.0;
        if (smooth[x] < m - 18.0 && smooth[x] <= smooth[x - 1] && smooth[x] <= smooth[x + 1]) {
            if (minima.empty() || x - minima.back() > 15) {
                minima.push_back(x);
            }
            else if (smooth[x] < smooth[minima.back()]) {
                minima.back() = x;
            }
        }
    }

    int best_p = 243;
    int best_f = 0;
    int best_votes = 0;
    double best_score = 1e9;
    for (int p = 205; p <= 250; ++p) {
        for (int f : minima) {
            if (f < 330) {
                continue;
            }
            int votes = 0;
            double err = 0.0;
            for (int k = 0; k < 6; ++k) {
                int target = f + k * p;
                if (target > 1680) {
                    break;
                }
                int nearest = -1;
                int dist = 9999;
                for (int m : minima) {
                    if (std::abs(m - target) < dist) {
                        dist = std::abs(m - target);
                        nearest = m;
                    }
                }
                if (nearest >= 0 && dist <= 8) {
                    ++votes;
                    err += dist;
                }
            }
            if (votes > best_votes || (votes == best_votes && err < best_score)) {
                best_votes = votes;
                best_p = p;
                best_f = f;
                best_score = err;
            }
        }
    }
    if (best_votes < 2) {
        return {};
    }
    while (best_f - best_p >= 330) {
        best_f -= best_p;
    }
    std::vector<int> gaps;
    for (int g = best_f; g <= 1650; g += best_p) {
        gaps.push_back(g);
    }
    return gaps;
}

std::vector<SPAFarmTaskPlugin::ShopCard> SPAFarmTaskPlugin::read_shop_cards(const cv::Mat& img)
{
    // Exact 1:1 port of `Bi` in service.pretty.mjs lines 5155-5334
    if (img.empty()) {
        return {};
    }
    const cv::Rect jt(330, 925, 1330, 135);
    cv::Mat orig_crop = img(jt).clone();

    // Red-channel enhanced crop (`5 * R - 1.3 * 255`)
    cv::Mat red_crop(jt.height, jt.width, CV_8UC3);
    for (int y = 0; y < jt.height; ++y) {
        const auto* src_row = orig_crop.ptr<cv::Vec3b>(y);
        auto* dst_row = red_crop.ptr<cv::Vec3b>(y);
        for (int x = 0; x < jt.width; ++x) {
            int R = src_row[x][2];
            int v = std::clamp(static_cast<int>(std::lround(5.0 * R - 1.3 * 255.0)), 0, 255);
            dst_row[x] = cv::Vec3b(static_cast<uint8_t>(v), static_cast<uint8_t>(v), static_cast<uint8_t>(v));
        }
    }

    auto orig_ocr = ocr(orig_crop, std::nullopt, true);
    auto red_ocr = ocr(red_crop, std::nullopt, true);

    struct TaggedOcr
    {
        OcrItem item;
        std::string src;
    };
    std::vector<TaggedOcr> all_ocr;
    for (const auto& it : orig_ocr) {
        all_ocr.push_back({ it, "orig" });
    }
    for (const auto& it : red_ocr) {
        all_ocr.push_back({ it, "red" });
    }

    struct RawHit
    {
        int x = 0;
        std::string kind; // "operator", "item", "bond"
        std::string name;
        double score = 0.0;
        bool exact = false;
        std::string src;
    };
    std::vector<RawHit> hits;

    for (const auto& tagged : all_ocr) {
        const auto& it = tagged.item;
        const int card_x = 330 + it.x;
        const int rel_y = it.y;

        if (rel_y < 65) {
            auto bm = match_dict(it.text, m_bond_dict);
            if (!bm.empty() && match_coverage(it.text, bm) >= 0.5) {
                for (const auto& m : bm) {
                    int sub_x = 330 + it.x + static_cast<int>(std::lround(static_cast<double>(it.w) * m.start / std::max(1, m.len)));
                    hits.push_back(RawHit { sub_x, "bond", m.name, m.score, m.exact, tagged.src });
                }
            }
            continue;
        }

        auto cm = match_dict(it.text, m_chess_dict);
        auto im = match_dict(it.text, m_item_dict);
        double cov_c = match_coverage(it.text, cm);
        double cov_i = match_coverage(it.text, im);

        bool pick_item = (!im.empty() && (cm.empty() || cov_i > cov_c || (im[0].exact && !cm[0].exact)));
        const auto& chosen = pick_item ? im : cm;
        const std::string chosen_kind = pick_item ? "item" : "operator";
        if (!chosen.empty() && std::max(cov_c, cov_i) >= 0.5) {
            for (const auto& m : chosen) {
                int sub_x = (chosen.size() == 1)
                    ? card_x
                    : (330 + it.x + static_cast<int>(std::lround(static_cast<double>(it.w) * m.start / std::max(1, m.len))));
                hits.push_back(RawHit { sub_x, chosen_kind, m.name, m.score, m.exact, tagged.src });
            }
        }
    }

    // Targeted gap slot fallback (`Bi` lines 5243-5281)
    auto gaps = detect_card_gaps(img);
    for (int gap_x : gaps) {
        if (gap_x + 180 > 1660) {
            continue;
        }
        bool covered = false;
        for (const auto& h : hits) {
            if (h.kind != "bond" && h.x >= gap_x - 20 && h.x < gap_x + 240) {
                covered = true;
                break;
            }
        }
        if (covered) {
            continue;
        }
        auto slot_rec = ocr_rec_only(img, { gap_x + 5, 925 + 80, 175, 48 }, 2, 0);
        for (const auto& r : slot_rec) {
            auto cm = match_dict(r.text, m_chess_dict);
            auto im = match_dict(r.text, m_item_dict);
            bool pick_item = (!im.empty() && (cm.empty() || im[0].score > cm[0].score));
            const auto& chosen = pick_item ? im : cm;
            if (!chosen.empty()) {
                hits.push_back(RawHit {
                    gap_x + 30,
                    pick_item ? "item" : "operator",
                    chosen[0].name,
                    chosen[0].score,
                    chosen[0].exact,
                    "slot" });
                break;
            }
        }
    }

    std::sort(hits.begin(), hits.end(), [](const RawHit& a, const RawHit& b) {
        return a.x < b.x;
    });

    struct Cluster
    {
        int x = 0;
        std::string name;
        std::string kind;
        std::string source;
        double score = 0.0;
        bool exact = false;
        std::vector<std::string> bonds;
    };
    std::vector<Cluster> clusters;

    for (const auto& h : hits) {
        Cluster* target = nullptr;
        for (auto& cl : clusters) {
            if (std::abs(h.x - cl.x) < 90) {
                target = &cl;
                break;
            }
        }
        if (!target) {
            clusters.push_back(Cluster { h.x, "", "", "", 0.0, false, {} });
            target = &clusters.back();
        }
        if (h.kind == "bond") {
            if (std::find(target->bonds.begin(), target->bonds.end(), h.name) == target->bonds.end()) {
                target->bonds.push_back(h.name);
            }
        }
        else {
            bool better = target->name.empty()
                || (h.exact && !target->exact)
                || (h.exact == target->exact && h.score > target->score);
            if (better) {
                target->x = h.x;
                target->name = h.name;
                target->kind = h.kind;
                target->source = h.src;
                target->score = h.score;
                target->exact = h.exact;
            }
        }
    }

    std::vector<ShopCard> cards;
    int idx = 0;
    for (auto& cl : clusters) {
        if (cl.name.empty() && !cl.bonds.empty()) {
            // Resolve operator uniquely from bonds (`Bi` lines 5309-5321)
            std::vector<std::string> cands;
            for (const auto& op : m_operators) {
                bool all_match = true;
                for (const auto& b : cl.bonds) {
                    if (std::find(op.bond_names.begin(), op.bond_names.end(), b) == op.bond_names.end()) {
                        all_match = false;
                        break;
                    }
                }
                if (all_match) {
                    cands.push_back(op.name);
                }
            }
            if (cands.size() == 1) {
                cl.name = cands.front();
                cl.kind = "operator";
                cl.source = "bonds";
            }
        }
        if (cl.name.empty() && cl.bonds.empty()) {
            continue;
        }
        ShopCard sc;
        sc.index = idx++;
        sc.x = cl.x;
        sc.name = cl.name;
        sc.kind = cl.kind;
        sc.source = cl.source;
        sc.bonds = std::move(cl.bonds);
        if (sc.kind == "operator") {
            if (const auto* op = find_operator(sc.name)) {
                sc.cost = op->purchase_price;
                sc.tier = op->tier;
                if (sc.bonds.empty()) {
                    sc.bonds = op->bond_names;
                }
            }
        }
        else if (sc.kind == "item") {
            if (const auto* it = find_item(sc.name)) {
                sc.cost = it->price;
                sc.tier = it->tier;
            }
        }
        cards.push_back(std::move(sc));
    }

    detect_free_cards(img, cards);
    return cards;
}

void SPAFarmTaskPlugin::detect_free_cards(const cv::Mat& img, std::vector<ShopCard>& cards) const
{
    // Exact 1:1 port of `Mt(I, k)` in service.pretty.mjs lines 8908-8929
    for (auto& card : cards) {
        if (card.name.empty()) {
            continue;
        }
        int left = std::clamp(card.x + 40, 0, std::max(0, img.cols - 130));
        auto price_ocr = ocr(img, std::array<int, 4> { left, 760, 130, 40 }, true);
        for (const auto& it : price_ocr) {
            if (it.score >= 0.6 && (it.text == "0" || it.text == "O" || it.text == "o" || it.text == "Ｏ" || it.text == "〇")) {
                card.free_card = true;
                card.cost = 0;
                break;
            }
        }
    }
}

// ============================================================================
// Board Unit Detection, Sprite Histogram & Detail Panel (`he`, `yn`, `ta`, `Oi`, `Gt`, `Ce`, `Pt`, `zr`)
// ============================================================================

cv::Rect SPAFarmTaskPlugin::unit_sprite_box(
    const std::array<double, 9>& H,
    int r,
    int c,
    std::string_view camera)
{
    // Exact 1:1 port of `ta(a, s, e, t)` in service.pretty.mjs lines 5598-5606
    auto [fx, fy] = project_point(H, c + 0.5, r + 0.5);
    auto [ix, iy] = project_point(H, c, r + 0.5);
    auto [mx, my] = project_point(H, c + 1.0, r + 0.5);
    (void)iy;
    (void)my;
    const double u = std::max(24.0, mx - ix);
    const double top_y = project_point(H, c + 0.5, r + 0.5 + (camera == "expanded" ? -1.3 : -0.95)).second;
    int rx = static_cast<int>(std::lround(fx - 0.45 * u));
    int ry = static_cast<int>(std::lround(top_y));
    int rw = std::max(12, static_cast<int>(std::lround(0.9 * u)));
    int rh = std::max(12, static_cast<int>(std::lround(fy + 0.1 * u - top_y)));
    return cv::Rect(rx, ry, rw, rh);
}

double SPAFarmTaskPlugin::green_hp_bar_ratio(
    const cv::Mat& img,
    double cx,
    double top_y,
    double box_h)
{
    // Exact 1:1 port of `Si(a, s, e, t)` in service.pretty.mjs lines 5506-5523
    if (img.empty()) {
        return 0.0;
    }
    const double f = std::max(16.0, box_h);
    const int y1 = std::clamp(static_cast<int>(std::lround(top_y - f * 5.0)), 0, img.rows);
    const int y2 = std::clamp(static_cast<int>(std::lround(top_y - f * 0.2)), 0, img.rows);
    const int x1 = std::clamp(static_cast<int>(std::lround(cx - 36.0)), 0, img.cols);
    const int x2 = std::clamp(static_cast<int>(std::lround(cx + 36.0)), 0, img.cols);
    int total = 0;
    int green = 0;
    for (int y = y1; y < y2; ++y) {
        const auto* row = img.ptr<cv::Vec3b>(y);
        for (int x = x1; x < x2; ++x) {
            const int B = row[x][0];
            const int G = row[x][1];
            const int R = row[x][2];
            ++total;
            if (G > 120 && G > R + 40 && G > B + 10) {
                ++green;
            }
        }
    }
    return total > 0 ? static_cast<double>(green) / static_cast<double>(total) : 0.0;
}

std::vector<double> SPAFarmTaskPlugin::compute_hsv_hist(
    const cv::Mat& img,
    const cv::Rect& roi,
    const cv::Mat* bg_ref)
{
    // Exact 1:1 port of `Li` + `Oi` in service.pretty.mjs lines 5626-5650 (108-bin HSV histogram with background subtraction)
    if (img.empty()) {
        return {};
    }
    cv::Rect clipped = roi & cv::Rect(0, 0, img.cols, img.rows);
    if (clipped.width <= 4 || clipped.height <= 4) {
        return {};
    }
    cv::Mat patch = img(clipped);
    const bool has_bg = (bg_ref != nullptr && !bg_ref->empty()
                         && bg_ref->cols == patch.cols && bg_ref->rows == patch.rows);
    std::vector<double> hist(108, 0.0);
    int fg_count = 0;
    const int total_px = patch.cols * patch.rows;
    for (int y = 0; y < patch.rows; ++y) {
        const auto* row = patch.ptr<cv::Vec3b>(y);
        const auto* bg_row = has_bg ? bg_ref->ptr<cv::Vec3b>(y) : nullptr;
        for (int x = 0; x < patch.cols; ++x) {
            const int B = row[x][0];
            const int G = row[x][1];
            const int R = row[x][2];
            if (has_bg) {
                const int d = std::abs(R - bg_row[x][2]) + std::abs(G - bg_row[x][1]) + std::abs(B - bg_row[x][0]);
                if (d < 60) {
                    continue;
                }
            }
            const int mx = std::max({ R, G, B });
            const int mn = std::min({ R, G, B });
            const int diff = mx - mn;
            const double val = mx / 255.0;
            const double sat = (mx > 0) ? (static_cast<double>(diff) / mx) : 0.0;
            double hue = 0.0;
            if (diff > 0) {
                if (mx == R) {
                    hue = std::fmod(static_cast<double>(G - B) / diff + 6.0, 6.0);
                }
                else if (mx == G) {
                    hue = static_cast<double>(B - R) / diff + 2.0;
                }
                else {
                    hue = static_cast<double>(R - G) / diff + 4.0;
                }
            }
            const int h_bin = (sat < 0.2) ? 0 : (static_cast<int>(std::floor(hue * 2.0)) % 12);
            const int s_bin = std::min(2, static_cast<int>(std::floor(sat * 3.0)));
            const int v_bin = std::min(2, static_cast<int>(std::floor(val * 3.0)));
            const int bin = h_bin * 9 + s_bin * 3 + v_bin;
            if (bin >= 0 && bin < 108) {
                hist[bin] += 1.0;
                ++fg_count;
            }
        }
    }
    if (fg_count < total_px * 0.08) {
        return {};
    }
    for (double& v : hist) {
        v /= static_cast<double>(fg_count);
    }
    return hist;
}

double SPAFarmTaskPlugin::hist_intersection(const std::vector<double>& a, const std::vector<double>& b)
{
    // Exact 1:1 port of `Ti(a, s)` in service.pretty.mjs lines 5652-5656
    if (a.size() != 108 || b.size() != 108) {
        return 0.0;
    }
    double s = 0.0;
    for (size_t i = 0; i < 108; ++i) {
        s += std::min(a[i], b[i]);
    }
    return s;
}

std::vector<std::pair<double, double>> SPAFarmTaskPlugin::detect_unit_badges(
    const cv::Mat& img,
    const std::array<double, 9>& H,
    std::string_view camera,
    const std::vector<std::string>& rows) const
{
    // Combines `he` + `yn` geometry from service.pretty.mjs lines 6491-6527 & 6773-6799:
    // For each deployable cell (r, c) in `abhH`, checks the badge location `[c + 0.23, r + 0.5 + w]`
    // (`w = camera == "expanded" ? -1.25 : -0.86`) for tier badge + HP bar + sprite foreground.
    std::vector<std::pair<double, double>> badges;
    if (img.empty() || rows.empty()) {
        return badges;
    }
    const double w_off = (camera == "expanded") ? -1.25 : -0.86;
    for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
        for (int c = 0; c < static_cast<int>(rows[r].size()); ++c) {
            char ch = rows[r][c];
            if (ch != 'a' && ch != 'b' && ch != 'h' && ch != 'H') {
                continue;
            }
            auto [bx, by] = project_point(H, c + 0.5 - 0.27, r + 0.5 + w_off);
            if (bx < 80 || bx > 1840 || by < 100 || by > 920) {
                continue;
            }

            // Check badge neighborhood around (bx, by) for bright Roman numeral glyph on dark badge disc + HP bar / sprite activity
            int x0 = std::clamp(static_cast<int>(std::lround(bx - 18)), 0, img.cols);
            int y0 = std::clamp(static_cast<int>(std::lround(by - 18)), 0, img.rows);
            int x1 = std::clamp(static_cast<int>(std::lround(bx + 18)), 0, img.cols);
            int y1 = std::clamp(static_cast<int>(std::lround(by + 18)), 0, img.rows);
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }

            int bright_glyph = 0;
            int dark_ring = 0;
            int gold_or_cyan_frame = 0;
            for (int y = y0; y < y1; ++y) {
                const auto* row = img.ptr<cv::Vec3b>(y);
                for (int x = x0; x < x1; ++x) {
                    int B = row[x][0], G = row[x][1], R = row[x][2];
                    int luma = (R + G + B) / 3;
                    if (luma > 205 && std::max({ R, G, B }) - std::min({ R, G, B }) < 45) {
                        ++bright_glyph;
                    }
                    else if (luma < 55) {
                        ++dark_ring;
                    }
                    if ((R > 165 && G > 115 && B < 95) || (G > 140 && G > R + 35)) {
                        ++gold_or_cyan_frame;
                    }
                }
            }

            // Also check HP bar or foreground diff on unit sprite box
            cv::Rect sp_box = unit_sprite_box(H, r, c, camera);
            double hp_ratio = green_hp_bar_ratio(img, bx + 26.0, by, 24.0);
            bool has_badge = (bright_glyph >= 8 && dark_ring >= 25 && (hp_ratio >= 0.04 || gold_or_cyan_frame >= 10));

            if (!has_badge && ch == 'H') {
                // Bench slot check: bench units sit on `H` row (r = 6) and have distinct sprite contrast even without combat HP bar
                std::string bg_key = std::string(camera) + ":" + std::to_string(r) + "," + std::to_string(c);
                auto bg_it = m_empty_cell_refs.find(bg_key);
                const cv::Mat* bg_ptr = (bg_it != m_empty_cell_refs.end()) ? &bg_it->second : nullptr;
                auto hvec = compute_hsv_hist(img, sp_box, bg_ptr);
                if (bright_glyph >= 6 && dark_ring >= 20 && !hvec.empty()) {
                    has_badge = true;
                }
            }

            if (has_badge) {
                badges.emplace_back(bx, by);
            }
        }
    }
    return badges;
}

std::vector<SPAFarmTaskPlugin::UnitSlot> SPAFarmTaskPlugin::map_badges_to_units(
    const std::vector<std::pair<double, double>>& badges,
    const std::array<double, 9>& H,
    std::string_view camera,
    const std::vector<std::string>& rows) const
{
    // Exact 1:1 port of `yn(a, s, e, t)` in service.pretty.mjs lines 6491-6527
    const double w = (camera == "expanded") ? -1.25 : -0.86;
    const auto inv_H = invert_homography(H);

    struct MatchCand
    {
        size_t b = 0;
        int r = 0;
        int c = 0;
        double d = 0.0;
    };
    std::vector<MatchCand> cands;
    for (size_t i = 0; i < badges.size(); ++i) {
        auto [gx, gy] = project_point(inv_H, badges[i].first, badges[i].second);
        const double y = gx - (-0.27);
        const double b = gy - w;
        for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
            for (int c = 0; c < static_cast<int>(rows[r].size()); ++c) {
                char ch = rows[r][c];
                if (ch != 'a' && ch != 'b' && ch != 'h' && ch != 'H') {
                    continue;
                }
                const double dist = std::hypot(y - (c + 0.5), b - (r + 0.5));
                if (dist < 0.8) {
                    cands.push_back(MatchCand { i, r, c, dist });
                }
            }
        }
    }
    std::sort(cands.begin(), cands.end(), [](const MatchCand& a, const MatchCand& b) {
        return a.d < b.d;
    });

    std::set<size_t> used_b;
    std::set<std::string> used_cell;
    std::vector<UnitSlot> units;
    for (const auto& m : cands) {
        std::string cell_key = std::to_string(m.r) + "," + std::to_string(m.c);
        if (used_b.count(m.b) > 0 || used_cell.count(cell_key) > 0) {
            continue;
        }
        used_b.insert(m.b);
        used_cell.insert(cell_key);
        const bool is_bench = (rows[m.r][m.c] == 'H');
        UnitSlot u;
        u.slot = is_bench
            ? ("bench:" + std::to_string(m.c + 1))
            : ("board:" + std::to_string(m.r) + "," + std::to_string(m.c));
        u.area = is_bench ? "bench" : "board";
        u.r = m.r;
        u.c = m.c;
        units.push_back(std::move(u));
    }
    return units;
}

bool SPAFarmTaskPlugin::is_detail_panel_open(const cv::Mat& img) const
{
    // Exact 1:1 port of `Ce(j)` in service.pretty.mjs lines 7303-7306
    if (img.empty()) {
        return false;
    }
    auto left_tabs = ocr(img, std::array<int, 4> { 20, 530, 640, 50 });
    if (has_ocr(left_tabs, { "特质", "技能", "特性", "天赋" })) {
        return true;
    }
    auto buy_bar = ocr(img, std::array<int, 4> { 250, 960, 1450, 110 });
    return has_ocr(buy_bar, { "确认购买" });
}

bool SPAFarmTaskPlugin::close_detail_panel()
{
    // Exact 1:1 port of `ge()` in service.pretty.mjs lines 7280-7292 (`vn = [1450, 300]`, fallback `[300, 950]`)
    for (int i = 0; i < 4 && !need_exit(); ++i) {
        tap(1450, 300, 180);
        if (!is_detail_panel_open(shot())) {
            return true;
        }
        tap(300, 950, 220);
        if (!is_detail_panel_open(shot())) {
            return true;
        }
    }
    return !is_detail_panel_open(shot());
}

std::pair<std::string, std::string> SPAFarmTaskPlugin::read_unit_detail_panel(const cv::Mat& img) const
{
    // Exact 1:1 port of `Pt(j)` + `us(a, s, e)` in service.pretty.mjs lines 6529-6559 & 7406-7445
    if (img.empty()) {
        return { "", "" };
    }
    auto panel_ocr = ocr(img, std::array<int, 4> { 20, 270, 560, 220 });
    std::string resolved_name;

    // 1. Top title line (`box[1] < 270 + 45`) exact/fuzzy match against operators & items
    std::string top_text;
    for (const auto& it : panel_ocr) {
        if (it.y < 315) {
            top_text += it.text;
            std::string nt = norm_text(it.text);
            if (const auto* op = find_operator(nt)) {
                resolved_name = op->name;
                break;
            }
            if (const auto* item = find_item(nt)) {
                resolved_name = item->name;
                break;
            }
        }
    }
    if (resolved_name.empty() && !top_text.empty()) {
        auto cm = match_dict(top_text, m_chess_dict);
        if (!cm.empty()) {
            resolved_name = cm.front().name;
        }
        else {
            auto im = match_dict(top_text, m_item_dict);
            if (!im.empty()) {
                resolved_name = im.front().name;
            }
        }
    }

    // 2. Bond + ATK/DEF disambiguation (`us` lines 6536-6558)
    if (resolved_name.empty()) {
        std::vector<std::string> seen_bonds;
        std::optional<int> atk_val;
        std::optional<int> def_val;
        for (const auto& it : panel_ocr) {
            for (const auto& b : m_bonds) {
                if (it.text.find(b.name) != std::string::npos) {
                    seen_bonds.push_back(b.name);
                }
            }
            auto pos_a = it.text.find("攻击");
            if (pos_a != std::string::npos) {
                std::string d = extract_digits_mapped(it.text.substr(pos_a));
                if (!d.empty()) {
                    atk_val = std::stoi(d);
                }
            }
            auto pos_d = it.text.find("防御");
            if (pos_d != std::string::npos) {
                std::string d = extract_digits_mapped(it.text.substr(pos_d));
                if (!d.empty()) {
                    def_val = std::stoi(d);
                }
            }
        }
        if (!seen_bonds.empty() || atk_val.has_value()) {
            std::vector<std::string> cands;
            for (const auto& op : m_operators) {
                bool bonds_ok = true;
                for (const auto& sb : seen_bonds) {
                    if (std::find(op.bond_names.begin(), op.bond_names.end(), sb) == op.bond_names.end()) {
                        bonds_ok = false;
                        break;
                    }
                }
                if (!bonds_ok) {
                    continue;
                }
                if (atk_val.has_value() && op.atk != *atk_val) {
                    continue;
                }
                if (def_val.has_value() && op.def != *def_val) {
                    continue;
                }
                cands.push_back(op.name);
            }
            if (cands.size() == 1) {
                resolved_name = cands.front();
            }
        }
    }

    // 3. Detect orange facing cone in `[640, 450, 960, 630]` (`Pt` lines 7416-7435)
    std::string facing;
    double sx = 0.0, sy = 0.0;
    int cone_n = 0;
    for (int y = 450; y < 1080 && y < img.rows; ++y) {
        const auto* row = img.ptr<cv::Vec3b>(y);
        for (int x = 640; x < 1600 && x < img.cols; ++x) {
            const int B = row[x][0], G = row[x][1], R = row[x][2];
            if (R > 220 && G > 110 && G < 175 && B < 70) {
                sx += (x - 640);
                sy += (y - 450);
                ++cone_n;
            }
        }
    }
    if (cone_n > 500) {
        const double cx = sx / cone_n;
        const double cy = sy / cone_n;
        double mx = 0.0, my = 0.0;
        int outer_n = 0;
        for (int y = 450; y < 1080 && y < img.rows; ++y) {
            const auto* row = img.ptr<cv::Vec3b>(y);
            for (int x = 640; x < 1600 && x < img.cols; ++x) {
                const int B = row[x][0], G = row[x][1], R = row[x][2];
                if (R > 220 && G > 110 && G < 175 && B < 70) {
                    const double dx = (x - 640) - cx;
                    const double dy = (y - 450) - cy;
                    if (dx * dx + dy * dy > 1600.0) {
                        mx += dx;
                        my += dy;
                        ++outer_n;
                    }
                }
            }
        }
        if (outer_n > 100) {
            if (std::abs(mx) > std::abs(my)) {
                facing = (mx > 0.0) ? "RIGHT" : "LEFT";
            }
            else {
                facing = (my > 0.0) ? "DOWN" : "UP";
            }
        }
    }

    return { resolved_name, facing };
}

void SPAFarmTaskPlugin::check_triple_merge(const std::string& op_name)
{
    // Exact 1:1 port of `mt(j, J)` in service.pretty.mjs lines 6829-6845
    std::vector<size_t> indices;
    for (size_t i = 0; i < m_units.size(); ++i) {
        if (m_units[i].name == op_name && !m_units[i].elite) {
            indices.push_back(i);
        }
    }
    if (indices.size() < 3) {
        return;
    }
    // Prefer keeping a board unit over a bench unit
    size_t keep_idx = indices.front();
    for (size_t idx : indices) {
        if (m_units[idx].area == "board" && !m_units[idx].ghost) {
            keep_idx = idx;
            break;
        }
    }
    m_units[keep_idx].elite = true;
    auto rem_it = m_remembered_units.find(m_units[keep_idx].slot);
    if (rem_it != m_remembered_units.end()) {
        rem_it->second.elite = true;
    }
    for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
        if (*it != keep_idx) {
            m_remembered_units.erase(m_units[*it].slot);
            m_units.erase(m_units.begin() + static_cast<std::ptrdiff_t>(*it));
        }
    }
    log_info("三合一升精: " + op_name + " -> 保留于 " + m_units[keep_idx].slot);
}

void SPAFarmTaskPlugin::refresh_board_snapshot(
    const cv::Mat& img,
    const CameraDetectResult& cam,
    bool force_shop)
{
    // Exact 1:1 port of `Tr` + `Gt` + `rr` in service.pretty.mjs lines 6888-7205
    if (img.empty() || cam.camera.empty()) {
        return;
    }
    const auto* st = find_stage(m_stage_id);
    if (!st) {
        return;
    }
    auto badges = detect_unit_badges(img, cam.H, cam.camera, st->rows);
    std::set<std::string> occ_cells;
    auto detected = map_badges_to_units(badges, cam.H, cam.camera, st->rows);
    for (const auto& u : detected) {
        occ_cells.insert(std::to_string(u.r) + "," + std::to_string(u.c));
        occ_cells.insert(std::to_string(u.r - 1) + "," + std::to_string(u.c));
    }
    update_stage_from_board(img, cam.H, cam.camera, occ_cells);

    // Record empty cell background references & compute unit sprite histograms (`rr` lines 7084-7095)
    for (int r = 1; r < static_cast<int>(st->rows.size()); ++r) {
        for (int c = 0; c < static_cast<int>(st->rows[r].size()); ++c) {
            char ch = st->rows[r][c];
            if (ch != 'a' && ch != 'b' && ch != 'h' && ch != 'H') {
                continue;
            }
            std::string ckey = std::to_string(r) + "," + std::to_string(c);
            std::string ckey_below = std::to_string(r + 1) + "," + std::to_string(c);
            if (occ_cells.count(ckey) == 0 && occ_cells.count(ckey_below) == 0) {
                cv::Rect box = unit_sprite_box(cam.H, r, c, cam.camera) & cv::Rect(0, 0, img.cols, img.rows);
                if (box.width > 4 && box.height > 4) {
                    m_empty_cell_refs[cam.camera + ":" + ckey] = img(box).clone();
                }
            }
        }
    }

    for (auto& u : detected) {
        std::string ckey = std::to_string(u.r) + "," + std::to_string(u.c);
        std::string ckey_above = std::to_string(u.r - 1) + "," + std::to_string(u.c);
        auto bg_it = m_empty_cell_refs.find(cam.camera + ":" + ckey);
        if (bg_it != m_empty_cell_refs.end() && occ_cells.count(ckey_above) == 0) {
            u.hist = compute_hsv_hist(img, unit_sprite_box(cam.H, u.r, u.c, cam.camera), &bg_it->second);
        }
    }

    // Reconcile identity from panel cache, previous units, histogram bank, and recent shop purchases (`Gt`)
    std::unordered_map<std::string, UnitSlot> prev_by_slot;
    for (const auto& u : m_units) {
        prev_by_slot[u.slot] = u;
    }

    for (auto& u : detected) {
        auto pv = m_panel_verified_slots.find(u.slot);
        if (pv != m_panel_verified_slots.end()) {
            u.name = pv->second;
            u.source = "panel";
            u.verified = true;
            u.confirmed = true;
            m_panel_verified_slots.erase(pv);
            continue;
        }
        auto prev_it = prev_by_slot.find(u.slot);
        if (prev_it != prev_by_slot.end() && !prev_it->second.name.empty()) {
            u.name = prev_it->second.name;
            u.source = "carried";
            u.facing = prev_it->second.facing;
            u.elite = prev_it->second.elite;
            u.verified = prev_it->second.verified;
            u.confirmed = prev_it->second.confirmed;
            u.auxiliary = prev_it->second.auxiliary;
            u.item = prev_it->second.item;
            continue;
        }
        auto rem_it = m_remembered_units.find(u.slot);
        if (rem_it != m_remembered_units.end() && !rem_it->second.name.empty()) {
            u.name = rem_it->second.name;
            u.source = "carried";
            u.facing = rem_it->second.facing;
            u.elite = rem_it->second.elite;
            u.verified = rem_it->second.verified;
        }
    }

    // Assign newly appeared bench units from `m_recent_bought_names` (right-to-left on bench)
    std::vector<size_t> unnamed_bench;
    for (size_t i = 0; i < detected.size(); ++i) {
        if (detected[i].area == "bench" && detected[i].name.empty()) {
            unnamed_bench.push_back(i);
        }
    }
    std::sort(unnamed_bench.begin(), unnamed_bench.end(), [&](size_t a, size_t b) {
        return detected[a].c > detected[b].c;
    });
    while (!unnamed_bench.empty() && !m_recent_bought_names.empty()) {
        size_t idx = unnamed_bench.front();
        unnamed_bench.erase(unnamed_bench.begin());
        detected[idx].name = m_recent_bought_names.front();
        detected[idx].source = "shop";
        detected[idx].verified = true;
        m_recent_bought_names.erase(m_recent_bought_names.begin());
    }

    // Mark auxiliary/summon/item units (`line 7187`)
    for (auto& u : detected) {
        if (u.name.empty()) {
            continue;
        }
        std::string nt = norm_text(u.name);
        for (const auto& aux : kAuxiliaryNames) {
            if (nt == norm_text(aux)) {
                u.auxiliary = true;
                break;
            }
        }
        if (!find_operator(u.name) && find_item(u.name)) {
            u.auxiliary = true;
            u.item = true;
        }
        m_remembered_units[u.slot] = u;
        if (!u.hist.empty() && (u.confirmed || u.source == "panel")) {
            auto& bank = m_op_hist_bank[u.name];
            bank.push_back(u.hist);
            if (bank.size() > 6) {
                bank.erase(bank.begin());
            }
        }
    }

    // Preserve remembered board units that may be occluded in the current frame (`ghost: true`, lines 7055-7070)
    for (const auto& [slot, rem] : m_remembered_units) {
        if (rem.area == "board" && !rem.auxiliary && !rem.name.empty()) {
            bool present = false;
            for (const auto& u : detected) {
                if (u.slot == slot) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                UnitSlot ghost_u = rem;
                ghost_u.ghost = true;
                detected.push_back(std::move(ghost_u));
            }
        }
    }

    m_units = std::move(detected);

    // Check triple merges
    std::set<std::string> unique_names;
    for (const auto& u : m_units) {
        if (!u.name.empty() && !u.elite) {
            unique_names.insert(u.name);
        }
    }
    for (const auto& nm : unique_names) {
        check_triple_merge(nm);
    }

    // Update deployCap from `剩余可部署 N` (`line 7189-7190`)
    if (auto rem = read_remaining_deploy(img)) {
        int board_cnt = 0;
        for (const auto& u : m_units) {
            if (u.area == "board" && !u.auxiliary) {
                ++board_cnt;
            }
        }
        m_deploy_cap = std::max(m_deploy_cap, board_cnt + *rem);
    }

    if (force_shop && cam.camera == "expanded") {
        m_shop = read_shop_cards(img);
    }
}

void SPAFarmTaskPlugin::inspect_unknown_units(const CameraDetectResult& cam, bool skip_bench_unknown)
{
    // Exact 1:1 port of `zr` in service.pretty.mjs lines 7447-7528 (including waitStableBoard L7463, L7464, L7476)
    (void)cam;
    std::optional<std::array<double, 4>> prev_pts;
    for (size_t idx = 0; idx < m_units.size(); ++idx) {
        if (need_exit()) {
            return;
        }
        auto& u = m_units[idx];
        if (skip_bench_unknown && u.area == "bench" && u.name.empty()) {
            continue;
        }
        const auto* op = u.name.empty() ? nullptr : find_operator(u.name);
        const bool need_facing = (u.area == "board" && u.facing.empty() && (!op || !op->facing_independent));
        const bool need_name = u.name.empty() || (!u.confirmed && u.source == "guess");
        if (!need_name && !need_facing) {
            continue;
        }

        auto st = wait_stable_board(2500, prev_pts);
        if (!st.has_value()) {
            close_detail_panel();
            st = wait_stable_board(4000, prev_pts);
        }
        if (!st.has_value()) {
            log_info("镜头一直没有停稳，已停止自动点选");
            break;
        }
        prev_pts = st->pts;

        auto [px, py] = project_point(st->H, u.c + 0.5, u.r + 0.5);
        tap(px, py, 240);
        cv::Mat panel_img = shot();
        if (!is_detail_panel_open(panel_img)) {
            continue;
        }
        auto [det_name, det_facing] = read_unit_detail_panel(panel_img);

        // Duplicate board operator name re-verify (`service.pretty.mjs` lines 7474-7483)
        if (!det_name.empty() && u.area == "board") {
            bool dup_on_board = false;
            for (size_t j = 0; j < m_units.size(); ++j) {
                if (j != idx && m_units[j].area == "board" && m_units[j].confirmed && m_units[j].name == det_name) {
                    dup_on_board = true;
                    break;
                }
            }
            if (dup_on_board) {
                close_detail_panel();
                if (auto retry_st = wait_stable_board(4000)) {
                    prev_pts = retry_st->pts;
                    auto [rx, ry] = project_point(retry_st->H, u.c + 0.5, u.r + 0.5);
                    tap(rx, ry, 280);
                    cv::Mat retry_img = shot();
                    if (is_detail_panel_open(retry_img)) {
                        std::tie(det_name, det_facing) = read_unit_detail_panel(retry_img);
                    }
                }
            }
        }

        if (!det_name.empty()) {
            u.name = det_name;
            u.source = "panel";
            u.verified = true;
            u.confirmed = true;
            m_panel_verified_slots[u.slot] = det_name;
        }
        if (!det_facing.empty()) {
            u.facing = det_facing;
        }
        m_remembered_units[u.slot] = u;
        close_detail_panel();
    }
}

// ============================================================================
// Stage Traffic & Simulated Annealing Placement Solver (`Rr`, `Nt`, `Zr`, `is`, `ss`, `cr`, `ws`)
// ============================================================================

SPAFarmTaskPlugin::StageTrafficMap SPAFarmTaskPlugin::build_stage_traffic(std::string_view stage_id) const
{
    // Exact 1:1 port of `Rr(a, s)` in service.pretty.mjs lines 5729-5847
    StageTrafficMap tm;
    const auto* st = find_stage(stage_id);
    if (!st || st->rows.empty()) {
        return tm;
    }
    tm.stage_id = st->stage_id;
    tm.grid = st->rows;
    tm.rows = static_cast<int>(tm.grid.size());
    tm.cols = static_cast<int>(tm.grid[0].size());

    // Apply `wr` overrides for `act3autochess_m01` (`service.pretty.mjs` lines 7529-7558)
    if (tm.stage_id == "act3autochess_m01" && tm.rows >= 5 && tm.cols >= 10) {
        tm.grid[1][4] = 'b';
        tm.grid[4][4] = 'b';
        tm.grid[4][9] = 'b';
    }

    for (int r = 0; r < tm.rows; ++r) {
        for (int c = 0; c < tm.cols; ++c) {
            if (tm.grid[r][c] == 'S') {
                tm.spawns.emplace_back(r, c);
            }
            else if (tm.grid[r][c] == 'E') {
                tm.goals.emplace_back(r, c);
            }
        }
    }

    auto is_road = [](char ch) {
        return ch == 'b' || ch == 'c' || ch == 'S' || ch == 'E' || ch == 'I' || ch == 'O';
    };

    const int INF = 1000000;
    auto bfs_dist = [&](bool ground_only) {
        std::vector<std::vector<int>> dist(tm.rows, std::vector<int>(tm.cols, INF));
        std::queue<std::pair<int, int>> q;
        for (const auto& g : tm.goals) {
            dist[g.first][g.second] = 0;
            q.push(g);
        }
        const int dirs[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
        while (!q.empty()) {
            auto [r, c] = q.front();
            q.pop();
            for (const auto& d : dirs) {
                int nr = r + d[0];
                int nc = c + d[1];
                if (nr >= 0 && nr < tm.rows && nc >= 0 && nc < tm.cols) {
                    if (ground_only && !is_road(tm.grid[nr][nc])) {
                        continue;
                    }
                    if (dist[nr][nc] > dist[r][c] + 1) {
                        dist[nr][nc] = dist[r][c] + 1;
                        q.push({ nr, nc });
                    }
                }
            }
        }
        return dist;
    };

    tm.ground_dist = bfs_dist(true);
    tm.air_dist = bfs_dist(false);
    tm.ground_traffic.assign(tm.rows, std::vector<double>(tm.cols, 0.0));
    tm.air_traffic.assign(tm.rows, std::vector<double>(tm.cols, 0.0));
    tm.early_ground_traffic.assign(tm.rows, std::vector<double>(tm.cols, 0.0));
    tm.ground_first_step.assign(tm.rows, std::vector<double>(tm.cols, 0.0));

    const int dirs[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    for (const auto& sp : tm.spawns) {
        SpawnPathInfo pinfo;
        pinfo.spawn = sp;
        if (tm.ground_dist[sp.first][sp.second] < INF) {
            std::vector<std::vector<double>> flow(tm.rows, std::vector<double>(tm.cols, 0.0));
            flow[sp.first][sp.second] = 1.0;
            std::vector<std::pair<int, int>> order;
            for (int r = 0; r < tm.rows; ++r) {
                for (int c = 0; c < tm.cols; ++c) {
                    if (tm.ground_dist[r][c] <= tm.ground_dist[sp.first][sp.second]) {
                        order.emplace_back(r, c);
                    }
                }
            }
            std::sort(order.begin(), order.end(), [&](const auto& a, const auto& b) {
                return tm.ground_dist[a.first][a.second] > tm.ground_dist[b.first][b.second];
            });
            const int start_d = tm.ground_dist[sp.first][sp.second];
            int road_tile_cnt = 0;
            for (const auto& [r, c] : order) {
                double f = flow[r][c];
                if (f <= 0.0) {
                    continue;
                }
                int step_from_spawn = start_d - tm.ground_dist[r][c];
                std::string key = std::to_string(r) + "," + std::to_string(c);
                pinfo.ground_flow[key] = f;
                pinfo.ground_progress[key] = step_from_spawn;
                if (tm.grid[r][c] != 'S' && tm.grid[r][c] != 'E') {
                    ++road_tile_cnt;
                    if (pinfo.first_tile.empty()) {
                        pinfo.first_tile = key;
                    }
                }
                tm.ground_traffic[r][c] += f;
                if (step_from_spawn == 1) {
                    tm.ground_first_step[r][c] += f;
                }
                if (step_from_spawn >= 1 && step_from_spawn <= 3) {
                    tm.early_ground_traffic[r][c] += f * (4.0 - step_from_spawn) / 3.0;
                }
                std::vector<std::pair<int, int>> nexts;
                for (const auto& d : dirs) {
                    int nr = r + d[0], nc = c + d[1];
                    if (nr >= 0 && nr < tm.rows && nc >= 0 && nc < tm.cols
                        && tm.ground_dist[nr][nc] == tm.ground_dist[r][c] - 1) {
                        nexts.emplace_back(nr, nc);
                    }
                }
                for (const auto& nx : nexts) {
                    flow[nx.first][nx.second] += f / static_cast<double>(nexts.size());
                }
            }
            pinfo.is_single_tile = (road_tile_cnt <= 1);
        }

        if (!tm.goals.empty()) {
            auto g = tm.goals.front();
            int steps = std::max(1, std::max(std::abs(g.first - sp.first), std::abs(g.second - sp.second)) * 2);
            for (int k = 0; k <= steps; ++k) {
                double t = static_cast<double>(k) / steps;
                int r = std::clamp(static_cast<int>(std::lround(sp.first + (g.first - sp.first) * t)), 0, tm.rows - 1);
                int c = std::clamp(static_cast<int>(std::lround(sp.second + (g.second - sp.second) * t)), 0, tm.cols - 1);
                tm.air_traffic[r][c] += 1.0 / (steps + 1);
            }
        }
        tm.paths.push_back(std::move(pinfo));
    }

    const int n_spawns = std::max(1, static_cast<int>(tm.spawns.size()));
    for (int r = 0; r < tm.rows; ++r) {
        for (int c = 0; c < tm.cols; ++c) {
            tm.ground_traffic[r][c] /= n_spawns;
            tm.air_traffic[r][c] /= n_spawns;
            tm.early_ground_traffic[r][c] /= n_spawns;
            tm.ground_first_step[r][c] /= n_spawns;
            tm.max_ground_traffic = std::max(tm.max_ground_traffic, tm.ground_traffic[r][c]);
            tm.max_air_traffic = std::max(tm.max_air_traffic, tm.air_traffic[r][c]);
            std::string key = std::to_string(r) + "," + std::to_string(c);
            if (tm.ground_traffic[r][c] >= 0.9 && tm.grid[r][c] == 'b') {
                tm.chokepoints.insert(key);
                tm.road_importance[key] = 1.5;
            }
            else if (tm.ground_traffic[r][c] > 0.0) {
                tm.road_importance[key] = 1.0;
            }
        }
    }
    return tm;
}

SPAFarmTaskPlugin::GrazeStats SPAFarmTaskPlugin::compute_cell_graze(
    const StageTrafficMap& traffic,
    const OperatorInfo& op,
    int r,
    int c,
    std::string_view facing) const
{
    GrazeStats gs;
    for (const auto& off : op.base_range) {
        auto [dr, dc] = rotate_offset(off, facing);
        int tr = r + dr;
        int tc = c + dc;
        if (tr >= 0 && tr < traffic.rows && tc >= 0 && tc < traffic.cols) {
            gs.ground += traffic.ground_traffic[tr][tc];
            if (op.position == "RANGED" || op.base_range.size() > 2) {
                gs.air += traffic.air_traffic[tr][tc];
            }
            gs.early_ground += traffic.early_ground_traffic[tr][tc];
            gs.first_step += traffic.ground_first_step[tr][tc];
        }
    }
    return gs;
}

SPAFarmTaskPlugin::PlacementEvalResult SPAFarmTaskPlugin::evaluate_placement_full(
    const std::vector<PlacementAssign>& placement,
    const std::vector<UnitSlot>& bench_units,
    const StageTrafficMap& traffic,
    const PlacementEvalOptions& opts) const
{
    // Exact 1:1 port of the FULL `Nt(a, s, e = {}, t = null)` in service.pretty.mjs lines 5862-6301:
    // Includes:
    // ① focusBonds (`focusBondMul`, `coreBondMul`, `Ue(op)` 2x stacking multiplier, `Yi=60`, `Zi=30`)
    // ② supportWeight.defense (`w`) & supportWeight.heal (`i`) weighting on road blocking, off-road tank penalty (`-80*w`), and healer coverage (`70*h*i`, `12*oe*ue*i`)
    // ③ advance mode exposed ranged penalty (`"输出位比保护它的近战更靠近出生点"`, `C += 0.5`), ranged-ahead-of-melee road penalty (`-110*ge - 20*Fe`), stronger-blocker-behind-weaker penalty (`-25*min(4,Ce)`), infection tile drain/damage (`sa={dmg:1.44,drain:70}, Ki=45`), unguarded route penalty (`-400*ge, -250`), first-contact tile `Ft` coverage (`-120` / `+15*min(3,ge)`), and `avoidRightCol` red-gate adjacency penalty (`-600, gate++`, `-200`).
    PlacementEvalResult res;
    if (placement.empty()) {
        return res;
    }

    static constexpr double kInfDmg = 1.44;
    static constexpr double kInfDrain = 70.0;
    static constexpr double kInfExtra = 45.0;
    static constexpr double kHealDpsMul = 0.25;
    static constexpr double kHealAllyBase = 12.0;
    static constexpr double kBondTierScore = 60.0;
    static constexpr double kStackBondTierScore = 30.0;
    static constexpr double kLeakByBlock[5] = { 1.0, 0.75, 0.5, 0.25, 0.1 };
    static constexpr double kRouteDpsCap = 1500.0;

    const double w = opts.support_defense_weight;
    const double heal_w = opts.support_heal_weight;

    auto has_bond = [](const OperatorInfo& op, std::string_view bname) {
        return std::find(op.bond_names.begin(), op.bond_names.end(), bname) != op.bond_names.end();
    };
    auto has_focus_bond = [&](const OperatorInfo& op) {
        for (const auto& b : op.bond_names) {
            if (opts.focus_bonds.count(b) > 0) {
                return true;
            }
        }
        return false;
    };
    auto is_ranged_fragile = [](const OperatorInfo& op) {
        return op.position == "RANGED" || op.profession == "SNIPER" || op.profession == "CASTER"
            || op.profession == "SUPPORT" || op.profession == "MEDIC";
    };
    auto is_tank_role = [&](const OperatorInfo& op) {
        return op.profession == "TANK" || has_bond(op, "坚城") || has_bond(op, "强盾");
    };
    auto role_taunt_rank = [&](const OperatorInfo& op) -> int {
        if (has_bond(op, "坚城")) {
            return 3;
        }
        if (has_bond(op, "强盾") || op.profession == "TANK") {
            return 2;
        }
        if (op.profession == "PIONEER" || op.profession == "WARRIOR") {
            return 1;
        }
        return 0;
    };
    auto block_strength = [&](const OperatorInfo& op) -> double {
        return op.block_cnt * 100.0 + role_taunt_rank(op) * 30.0 + op.max_hp / 500.0;
    };
    auto defense_factor = [&](const OperatorInfo& op) -> double {
        const double hp_def = std::min(2.0, (op.max_hp / 2200.0 + op.def / 450.0) * 0.5);
        return std::min(2.2, (op.block_cnt / 2.0) * (0.5 + 0.5 * hp_def) * (1.0 + 0.2 * role_taunt_rank(op)));
    };
    auto effective_atk = [&](const PlacementAssign& pa, const OperatorInfo& op) -> double {
        return op.atk * (pa.elite ? 1.4 : 1.0) * (op.profession == "MEDIC" ? kHealDpsMul : 1.0);
    };

    double M = 0.0; // defense
    double Z = 0.0; // stacking
    double C = 0.0; // exposed
    double T = 0.0; // advance_score
    double me = 0.0; // gate
    int unguarded = 0;

    // Precompute each placed unit's resolved OperatorInfo & world attack cells (`rangeSet`)
    struct PlacedUnitCtx
    {
        PlacementAssign pa;
        const OperatorInfo* op = nullptr;
        std::string cell_key;
        std::vector<std::pair<int, int>> range_cells;
        std::unordered_set<std::string> range_set;
    };
    std::vector<PlacedUnitCtx> units;
    units.reserve(placement.size());
    std::unordered_map<std::string, size_t> by_cell;

    for (const auto& pa : placement) {
        const auto* op = find_operator(pa.name);
        if (!op) {
            continue;
        }
        PlacedUnitCtx ctx;
        ctx.pa = pa;
        ctx.op = op;
        ctx.cell_key = std::to_string(pa.r) + "," + std::to_string(pa.c);
        for (const auto& off : op->base_range) {
            auto [dr, dc] = rotate_offset(off, pa.facing);
            int tr = pa.r + dr;
            int tc = pa.c + dc;
            if (tr >= 0 && tr < traffic.rows && tc >= 0 && tc < traffic.cols) {
                ctx.range_cells.emplace_back(tr, tc);
                ctx.range_set.insert(std::to_string(tr) + "," + std::to_string(tc));
            }
        }
        by_cell[ctx.cell_key] = units.size();
        units.push_back(std::move(ctx));
    }
    if (units.empty()) {
        return res;
    }

    // L5885-5897: ③ `advance` mode exposed ranged penalty ("输出位比保护它的近战更靠近出生点")
    if (opts.advance && opts.round >= 4 && !traffic.spawns.empty()) {
        std::vector<const PlacedUnitCtx*> melee_units;
        for (const auto& u : units) {
            if (!is_ranged_fragile(*u.op)) {
                melee_units.push_back(&u);
            }
        }
        if (!melee_units.empty()) {
            for (const auto& ue : units) {
                if (!is_ranged_fragile(*ue.op)) {
                    continue;
                }
                if (ue.pa.r >= 0 && ue.pa.r < traffic.rows && ue.pa.c >= 0 && ue.pa.c < traffic.cols
                    && traffic.grid[ue.pa.r][ue.pa.c] == 'h') {
                    continue;
                }
                std::pair<int, int> ge = traffic.spawns.front();
                int Fe = 999;
                for (const auto& sp : traffic.spawns) {
                    int d = std::abs(ue.pa.r - sp.first) + std::abs(ue.pa.c - sp.second);
                    if (d < Fe) {
                        Fe = d;
                        ge = sp;
                    }
                }
                bool protected_by_closer_melee = false;
                for (const auto* Ce : melee_units) {
                    if (std::abs(Ce->pa.r - ue.pa.r) <= 2
                        && (std::abs(Ce->pa.r - ge.first) + std::abs(Ce->pa.c - ge.second)) < Fe) {
                        protected_by_closer_melee = true;
                        break;
                    }
                }
                if (!protected_by_closer_melee) {
                    C += 0.5;
                }
            }
        }
    }

    // L5916-5924: Infection tiles (`tile_infection`) and incoming healer HPS per cell
    std::unordered_set<std::string> infected_cells;
    std::unordered_map<std::string, double> incoming_heal;
    for (const auto& u : units) {
        if (u.op->stack_info.infects_tile) {
            infected_cells.insert(u.cell_key);
        }
        if (u.op->is_healer) {
            const double hps = u.op->atk * (u.pa.elite ? 1.4 : 1.0) * (u.op->profession == "MEDIC" ? 0.35 : 0.15);
            for (const auto& rk : u.range_set) {
                incoming_heal[rk] += hps;
            }
        }
    }

    auto survive_on_cell = [&](const PlacedUnitCtx& u) -> std::pair<double, double> {
        if (infected_cells.count(u.cell_key) == 0) {
            return { 0.0, 1.0 };
        }
        const double net = incoming_heal[u.cell_key] - kInfDrain;
        const double factor = (net >= 0.0)
            ? 1.0
            : std::max(0.15, 1.0 + net / std::max(100.0, u.op->max_hp * 0.08));
        return { net, factor };
    };

    // L5954-5975: Per-path traffic attenuation across blockers (`N(r, c, unit)` & first-contact tiles `Ft`)
    std::vector<double> path_leak(traffic.paths.size(), 1.0);
    std::vector<bool> path_has_melee_blocker(traffic.paths.size(), false);
    std::vector<std::pair<const PlacedUnitCtx*, std::string>> first_contacts;
    std::unordered_map<std::string, double> road_blocker_traffic;

    auto effective_cell_traffic = [&](int r, int c, const PlacedUnitCtx* self_u) -> double {
        if (traffic.paths.empty()) {
            return (r >= 0 && r < traffic.rows && c >= 0 && c < traffic.cols) ? traffic.ground_traffic[r][c] : 0.0;
        }
        const std::string key = std::to_string(r) + "," + std::to_string(c);
        double sum = 0.0;
        for (size_t pi = 0; pi < traffic.paths.size(); ++pi) {
            const auto& p = traffic.paths[pi];
            auto fl_it = p.ground_flow.find(key);
            if (fl_it == p.ground_flow.end()) {
                continue;
            }
            double f = fl_it->second;
            int my_prog = p.ground_progress.at(key);
            for (const auto& other : units) {
                if (self_u && other.cell_key == self_u->cell_key) {
                    continue;
                }
                auto opr_it = p.ground_progress.find(other.cell_key);
                if (opr_it != p.ground_progress.end() && opr_it->second < my_prog) {
                    int blk = is_ranged_fragile(*other.op) ? 0 : std::clamp(other.op->block_cnt, 0, 4);
                    f *= kLeakByBlock[blk];
                }
            }
            sum += f;
        }
        return sum / std::max<size_t>(1, traffic.paths.size());
    };

    for (size_t pi = 0; pi < traffic.paths.size(); ++pi) {
        const auto& p = traffic.paths[pi];
        std::vector<std::pair<int, const PlacedUnitCtx*>> path_units;
        for (const auto& u : units) {
            auto it = p.ground_progress.find(u.cell_key);
            if (it != p.ground_progress.end()) {
                path_units.emplace_back(it->second, &u);
            }
        }
        std::sort(path_units.begin(), path_units.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });

        double leak = 1.0;
        const PlacedUnitCtx* first_melee = nullptr;
        int first_melee_prog = -1;
        for (const auto& [prog, u_ptr] : path_units) {
            if (!is_ranged_fragile(*u_ptr->op) && u_ptr->op->block_cnt >= 1) {
                path_has_melee_blocker[pi] = true;
                if (!first_melee) {
                    first_melee = u_ptr;
                    first_melee_prog = prog;
                }
                leak *= kLeakByBlock[std::clamp(u_ptr->op->block_cnt, 0, 4)];
            }
        }
        path_leak[pi] = leak;

        if (first_melee) {
            std::string prev_tile = first_melee->cell_key;
            for (const auto& [k, pr] : p.ground_progress) {
                if (pr == first_melee_prog - 1) {
                    prev_tile = k;
                    break;
                }
            }
            first_contacts.emplace_back(first_melee, prev_tile);
        }
    }

    // L5976-6088: Evaluate each placed unit (road blocking, supportWeight.defense/heal, infection, DPS graze, advance score)
    for (const auto& F : units) {
        const auto& op = *F.op;
        const double focus_mul = has_focus_bond(op) ? 2.0 : 1.0;
        const auto [net_heal, surv_factor] = survive_on_cell(F);
        const double inf_dmg = (infected_cells.count(F.cell_key) > 0) ? kInfDmg : 1.0;

        const bool on_ground_road = (F.pa.r >= 0 && F.pa.r < traffic.rows && F.pa.c >= 0 && F.pa.c < traffic.cols
                                     && traffic.ground_traffic[F.pa.r][F.pa.c] > 0.0);
        if (on_ground_road) {
            const int blk = is_ranged_fragile(op) ? 0 : std::clamp(op.block_cnt, 0, 4);
            const double ue = effective_cell_traffic(F.pa.r, F.pa.c, &F) * (1.0 - kLeakByBlock[blk]);
            road_blocker_traffic[F.cell_key] = ue;

            if (is_ranged_fragile(op)) {
                double ge = 0.0;
                double Fe = 0.0;
                for (const auto& Ce : traffic.paths) {
                    auto fl_it = Ce.ground_flow.find(F.cell_key);
                    if (fl_it == Ce.ground_flow.end()) {
                        continue;
                    }
                    const int my_prog = Ce.ground_progress.at(F.cell_key);
                    bool melee_ahead = false;
                    for (const auto& other : units) {
                        if (other.cell_key != F.cell_key && !is_ranged_fragile(*other.op) && other.op->block_cnt >= 1) {
                            auto o_it = Ce.ground_progress.find(other.cell_key);
                            if (o_it != Ce.ground_progress.end() && o_it->second < my_prog) {
                                melee_ahead = true;
                                break;
                            }
                        }
                    }
                    if (melee_ahead) {
                        Fe += fl_it->second;
                    }
                    else {
                        ge += fl_it->second;
                    }
                }
                const double n_paths = std::max<double>(1.0, static_cast<double>(traffic.paths.size()));
                ge /= n_paths;
                Fe /= n_paths;
                M += ue * 70.0 * 0.3 - 110.0 * ge - 20.0 * Fe;
                C += ge;
            }
            else {
                // ② supportWeight.defense (`w`) weighting on melee road blocker
                M += ue * 70.0 * std::min(1.8, 0.4 + 0.6 * defense_factor(op) * w) * surv_factor;

                // L6001-6011: Stronger blocker behind weaker blocker on same path penalty
                for (const auto& ke : traffic.paths) {
                    auto my_it = ke.ground_progress.find(F.cell_key);
                    if (my_it == ke.ground_progress.end()) {
                        continue;
                    }
                    for (const auto& ge : units) {
                        if (ge.cell_key == F.cell_key || is_ranged_fragile(*ge.op) || ge.op->block_cnt < 1) {
                            continue;
                        }
                        auto ge_it = ke.ground_progress.find(ge.cell_key);
                        if (ge_it != ke.ground_progress.end() && ge_it->second < my_it->second) {
                            if (role_taunt_rank(op) < 2 && !ge.op->can_block_air) {
                                const double diff = (block_strength(op) - block_strength(*ge.op)) / 100.0;
                                if (diff > 0.0) {
                                    M -= 25.0 * std::min(4.0, diff);
                                }
                            }
                        }
                    }
                }
            }
        }

        // L6022-6028: Infection tile bonus/penalty
        if (infected_cells.count(F.cell_key) > 0) {
            if (net_heal < 0.0) {
                M += std::max(-180.0, net_heal * 1.5);
            }
            else {
                M += 25.0;
            }
            if (op.stack_info.uses_infected_tile) {
                Z += kInfExtra * focus_mul;
            }
        }

        // L6030-6033: Off-road melee & tank penalties (`-60`, `-80 * w`)
        if (!is_ranged_fragile(op) && !on_ground_road && F.range_cells.size() <= 2) {
            M -= 60.0;
        }
        if (is_tank_role(op) && !on_ground_road) {
            M -= 80.0 * w;
        }

        // L6038-6046: Advance score `T` toward nearest spawn
        if (!traffic.spawns.empty()) {
            std::pair<int, int> ge = traffic.spawns.front();
            int best_d = 999;
            for (const auto& sp : traffic.spawns) {
                int d = std::abs(F.pa.r - sp.first) + std::abs(F.pa.c - sp.second);
                if (d < best_d) {
                    best_d = d;
                    ge = sp;
                }
            }
            auto [f_dr, f_dc] = rotate_offset({ 0, 1 }, F.pa.facing);
            const double ke = ge.first - F.pa.r;
            const double we = ge.second - F.pa.c;
            const double ot = std::abs(ke) + std::abs(we);
            T += (20.0 - ot) * 4.0 + ((ke * f_dr + we * f_dc) / std::max(1.0, ot)) * 6.0;
        }

        // L6050-6088: Attack / Heal range evaluation & `supportWeight.heal` (`heal_w`)
        double fe = 0.0;
        double he = 0.0;
        double h_road = 0.0;
        for (const auto& [tr, tc] : F.range_cells) {
            std::string rk = std::to_string(tr) + "," + std::to_string(tc);
            if (op.is_healer) {
                auto rb_it = road_blocker_traffic.find(rk);
                if (rb_it != road_blocker_traffic.end()) {
                    h_road += rb_it->second;
                }
                continue;
            }
            const double gt = effective_cell_traffic(tr, tc, nullptr);
            if (gt > 0.0) {
                double imp = 1.0;
                if (auto imp_it = traffic.road_importance.find(rk); imp_it != traffic.road_importance.end()) {
                    imp = imp_it->second;
                }
                fe += gt * (0.7 + 0.3 * imp);
            }
            if (is_ranged_fragile(op) || F.range_cells.size() > 2) {
                he += traffic.air_traffic[tr][tc];
            }
        }

        if (op.is_healer) {
            M += h_road * 70.0 * heal_w;
            int covered_allies = 0;
            for (const auto& other : units) {
                if (other.cell_key != F.cell_key && F.range_set.count(other.cell_key) > 0) {
                    ++covered_allies;
                }
            }
            const double oe = (op.profession == "MEDIC") ? 1.0 : 0.25;
            M += kHealAllyBase * oe * covered_allies * heal_w;
        }
        else {
            const double dps_unit = (effective_atk(F.pa, op) * inf_dmg * surv_factor) / 600.0;
            M += dps_unit * (fe * 55.0 + he * 18.0);
        }
    }

    // L6090-6101: Unguarded spawn routes penalty (`-400 * ge`, `-250`)
    for (size_t pi = 0; pi < traffic.paths.size(); ++pi) {
        const auto& p = traffic.paths[pi];
        double route_dps = 0.0;
        for (const auto& u : units) {
            if (u.op->is_healer) {
                continue;
            }
            double covered_flow = 0.0;
            for (const auto& rk : u.range_set) {
                if (auto fl_it = p.ground_flow.find(rk); fl_it != p.ground_flow.end()) {
                    covered_flow += fl_it->second;
                }
            }
            if (covered_flow > 0.0) {
                const auto [nh, sf] = survive_on_cell(u);
                (void)nh;
                const double idmg = (infected_cells.count(u.cell_key) > 0) ? kInfDmg : 1.0;
                route_dps += effective_atk(u.pa, *u.op) * idmg * sf * std::min(2.5, covered_flow);
            }
        }
        const double ge = path_leak[pi] * std::max(0.0, 1.0 - route_dps / kRouteDpsCap);
        M -= 400.0 * ge;
        if (traffic.paths.size() > 1 && !path_has_melee_blocker[pi]) {
            ++unguarded;
            M -= 250.0;
        }
    }

    // L6118-6135: ① `focusBonds` & core bonds scoring (`Yi=60`, `Zi=30`, `focusBondMul`, `coreBondMul`)
    std::unordered_map<std::string, std::set<std::string>> field_bond_ops;
    std::unordered_map<std::string, std::set<std::string>> all_bond_ops;
    for (const auto& u : units) {
        for (const auto& b : u.op->bond_names) {
            field_bond_ops[b].insert(u.op->name);
            all_bond_ops[b].insert(u.op->name);
        }
        if (auto eq_it = m_unit_equipped_bond.find(u.op->name); eq_it != m_unit_equipped_bond.end() && !eq_it->second.empty()) {
            field_bond_ops[eq_it->second].insert(u.op->name);
            all_bond_ops[eq_it->second].insert(u.op->name);
        }
    }
    for (const auto& bu : bench_units) {
        if (const auto* bop = find_operator(bu.name)) {
            for (const auto& b : bop->bond_names) {
                all_bond_ops[b].insert(bop->name);
            }
        }
    }

    std::unordered_set<std::string> active_bonds;
    for (const auto& b : m_bonds) {
        const auto& set_ref = (b.count_mode == "BOARD_AND_DECK" || b.count_mode == "FIELD_AND_BENCH")
            ? all_bond_ops[b.name]
            : field_bond_ops[b.name];
        const int G = static_cast<int>(set_ref.size());
        int oe = 0;
        for (int th : b.thresholds) {
            if (G >= th) {
                ++oe;
            }
        }
        if (oe <= 0) {
            continue;
        }
        active_bonds.insert(b.name);
        const double ue = (opts.focus_bonds.count(b.name) > 0) ? opts.focus_bond_mul : 1.0;
        const double ge = b.is_core ? opts.core_bond_mul : 1.0;
        M += oe * kBondTierScore * ge * ue;
        if (b.desc.find("层数") != std::string::npos || b.desc.find("叠加") != std::string::npos) {
            Z += oe * kStackBondTierScore * ge * ue;
        }
    }

    // L6136-6148: First-contact tile `Ft` coverage check (`-120` if 0 allies cover, `+15*min(3,ge)` otherwise)
    for (const auto& [blocker_u, ft_key] : first_contacts) {
        int ge = 0;
        for (const auto& ke : units) {
            if (ke.cell_key == blocker_u->cell_key) {
                continue;
            }
            if (ke.range_set.count(ft_key) > 0 || ke.range_set.count(blocker_u->cell_key) > 0) {
                ++ge;
            }
        }
        if (ge == 0) {
            M -= 120.0;
        }
        else {
            M += 15.0 * std::min(3, ge);
        }
    }

    // L6149-6179: `avoidRightCol` red-gate adjacency penalty & single-tile spawn penalty
    if (opts.avoid_right_col) {
        for (const auto& u : units) {
            for (const auto& sp : traffic.spawns) {
                if (std::abs(u.pa.r - sp.first) + std::abs(u.pa.c - sp.second) == 1) {
                    M -= 600.0;
                    me += 1.0;
                    break;
                }
            }
            int max_deploy_c = -1;
            for (int c = 0; c < traffic.cols; ++c) {
                char ch = traffic.grid[u.pa.r][c];
                if (ch == 'a' || ch == 'b' || ch == 'h') {
                    max_deploy_c = std::max(max_deploy_c, c);
                }
            }
            const bool can_frontline = is_tank_role(*u.op)
                || u.op->profession == "WARRIOR"
                || u.op->profession == "PIONEER"
                || u.op->stack_info.on_deploy;
            if (u.pa.c == max_deploy_c && !can_frontline) {
                M -= 200.0;
            }
        }
    }
    for (const auto& p : traffic.paths) {
        if (p.is_single_tile && !p.first_tile.empty()) {
            auto it = by_cell.find(p.first_tile);
            if (it != by_cell.end()) {
                const auto& u = units[it->second];
                const bool ok = is_tank_role(*u.op) || u.op->profession == "WARRIOR"
                    || u.op->profession == "PIONEER" || u.op->stack_info.on_deploy;
                if (!ok) {
                    M -= 150.0;
                }
            }
        }
    }

    // L6181-6285: Trait stacking geometry score (`front1`, `back1`, `sameRow`, `around4/8/12`, `inRange`, `on_deploy`, `on_skill`, `end_of_prep`)
    for (size_t i = 0; i < units.size(); ++i) {
        const auto& u = units[i];
        const auto& op = *u.op;
        const double oe = has_focus_bond(op) ? 2.0 : 1.0;

        for (const auto& rule : op.stack_info.rules) {
            if (!rule.need_bond.empty() && active_bonds.count(rule.need_bond) == 0) {
                continue;
            }
            int matched_neighbors = 0;
            for (size_t j = 0; j < units.size(); ++j) {
                if (i == j && rule.other_only) {
                    continue;
                }
                const auto& nb = units[j];
                if (!rule.need_bond.empty() && !has_bond(*nb.op, rule.need_bond)) {
                    continue;
                }
                const int dr = nb.pa.r - u.pa.r;
                const int dc = nb.pa.c - u.pa.c;
                const int manhattan = std::abs(dr) + std::abs(dc);
                const int chebyshev = std::max(std::abs(dr), std::abs(dc));
                bool geom_ok = false;
                if (rule.geom == "front1") {
                    auto [fdr, fdc] = rotate_offset({ 0, 1 }, u.pa.facing);
                    geom_ok = (dr == fdr && dc == fdc);
                }
                else if (rule.geom == "back1") {
                    auto [bdr, bdc] = rotate_offset({ 0, -1 }, u.pa.facing);
                    geom_ok = (dr == bdr && dc == bdc);
                }
                else if (rule.geom == "sameRow") {
                    geom_ok = (dr == 0 && dc != 0);
                }
                else if (rule.geom == "around4") {
                    geom_ok = (manhattan == 1);
                }
                else if (rule.geom == "around8") {
                    geom_ok = (chebyshev == 1);
                }
                else if (rule.geom == "around12") {
                    geom_ok = (manhattan >= 1 && manhattan <= 2);
                }
                else if (rule.geom == "inRange") {
                    geom_ok = (u.range_set.count(nb.cell_key) > 0);
                }
                if (geom_ok) {
                    ++matched_neighbors;
                }
            }
            if (matched_neighbors > 0) {
                Z += std::min(3, matched_neighbors) * 38.0 * oe;
            }
            else {
                Z -= 15.0 * oe;
            }
        }

        if (op.stack_info.on_deploy) {
            const bool on_road = (u.pa.r >= 0 && u.pa.r < traffic.rows && u.pa.c >= 0 && u.pa.c < traffic.cols
                                  && traffic.ground_traffic[u.pa.r][u.pa.c] > 0.0);
            Z += (on_road ? 32.0 : 24.0) * oe;
        }
        if (op.stack_info.on_skill) {
            GrazeStats gs = compute_cell_graze(traffic, op, u.pa.r, u.pa.c, u.pa.facing);
            if (gs.ground + gs.air > 0.05) {
                Z += 22.0 * std::min(2.5, gs.ground + gs.air) * oe;
            }
            else {
                Z -= 10.0 * oe;
            }
        }
        if (op.stack_info.end_of_prep) {
            Z += 22.0 * oe;
        }
    }

    res.defense = M;
    res.stacking = Z;
    res.exposed = C;
    res.advance_score = T;
    res.gate = me;
    res.unguarded_routes = unguarded;
    res.score = M + Z - C * 80.0 + (opts.advance ? T : 0.0);
    return res;
}

double SPAFarmTaskPlugin::evaluate_placement(
    const std::vector<PlacementAssign>& placement,
    const std::vector<UnitSlot>& bench_units,
    const StageTrafficMap& traffic,
    bool advance_mode) const
{
    PlacementEvalOptions opts;
    for (const auto& cb : m_active_core_bonds) {
        opts.focus_bonds.insert(cb);
    }
    if (m_goal != "medal") {
        for (const auto& gt : kGoalTasks) {
            if (gt.id == m_goal) {
                opts.focus_bonds.insert(gt.bonds.begin(), gt.bonds.end());
                break;
            }
        }
    }
    opts.focus_bond_mul = (m_goal == "medal") ? 2.0 : 6.0;
    opts.core_bond_mul = 1.5;
    opts.support_defense_weight = 1.0;
    opts.support_heal_weight = 1.0;
    opts.advance = advance_mode;
    opts.round = m_current_round;
    opts.avoid_right_col = (m_current_round >= 9 && m_current_boss.find("左") == std::string::npos);
    return evaluate_placement_full(placement, bench_units, traffic, opts).score;
}

std::pair<std::vector<SPAFarmTaskPlugin::PlacementAssign>, std::vector<SPAFarmTaskPlugin::PlacementMove>>
SPAFarmTaskPlugin::solve_placement(
    const StageTrafficMap& traffic,
    int max_deploy,
    bool advance_mode) const
{
    // Exact 1:1 port of `Zr` + `is` + `ss` + `ws` in service.pretty.mjs lines 6302-6455 & 7560-7646:
    // Two-stage optimization (`ss`):
    // - Stage 1 maximizes defense (`defense - 80 * exposed`) via SA (`Zr`) + greedy local refinement (`is`).
    // - Stage 2 preserves defense within `best_def - max(80, |best_def| * 0.12)` and `exposed <= min_exposed`, `gate <= min_gate`,
    //   while maximizing stacking + advance (`stacking + (advance ? advance_score : 0) + 0.25 * defense`).
    std::vector<UnitSlot> candidates;
    for (const auto& u : m_units) {
        if (!u.name.empty() && !u.auxiliary && !u.item && find_operator(u.name) != nullptr) {
            candidates.push_back(u);
        }
    }
    if (candidates.empty()) {
        return { {}, {} };
    }

    std::vector<std::pair<int, int>> melee_cells;
    std::vector<std::pair<int, int>> ranged_cells;
    for (int r = 1; r <= 4 && r < traffic.rows; ++r) {
        for (int c = 0; c < traffic.cols; ++c) {
            char ch = traffic.grid[r][c];
            if (ch == 'a' || ch == 'b') {
                melee_cells.emplace_back(r, c);
            }
            if (ch == 'a' || ch == 'h') {
                ranged_cells.emplace_back(r, c);
            }
        }
    }

    std::unordered_map<std::string, int> owned_counts;
    for (const auto& u : candidates) {
        owned_counts[u.name]++;
    }
    std::unordered_set<std::string> active_bonds_est(m_active_core_bonds.begin(), m_active_core_bonds.end());

    std::sort(candidates.begin(), candidates.end(), [&](const UnitSlot& a, const UnitSlot& b) {
        const auto* opa = find_operator(a.name);
        const auto* opb = find_operator(b.name);
        double sa = opa ? (score_operator_for_plan(*opa, owned_counts, active_bonds_est) + (a.elite ? 25.0 : 0.0) + (a.area == "board" ? 4.0 : 0.0)) : 0.0;
        double sb = opb ? (score_operator_for_plan(*opb, owned_counts, active_bonds_est) + (b.elite ? 25.0 : 0.0) + (b.area == "board" ? 4.0 : 0.0)) : 0.0;
        return sa > sb;
    });

    const int target_cnt = std::min(static_cast<int>(candidates.size()), std::max(1, max_deploy));
    std::vector<UnitSlot> chosen(candidates.begin(), candidates.begin() + target_cnt);
    std::vector<UnitSlot> bench_rest(candidates.begin() + target_cnt, candidates.end());

    std::set<std::pair<int, int>> used_cells;
    std::vector<PlacementAssign> current;
    static const std::array<std::string, 4> kDirs = { "RIGHT", "UP", "LEFT", "DOWN" };

    for (const auto& u : chosen) {
        const auto* op = find_operator(u.name);
        if (!op) {
            continue;
        }
        const auto& pool = (op->position == "RANGED") ? ranged_cells : melee_cells;
        if (pool.empty()) {
            continue;
        }
        std::pair<int, int> best_rc = pool.front();
        std::string best_dir = u.facing.empty() ? "LEFT" : u.facing;
        double best_cell_score = -1e9;

        for (const auto& rc : pool) {
            if (used_cells.count(rc) > 0) {
                continue;
            }
            for (const auto& dir : kDirs) {
                GrazeStats gs = compute_cell_graze(traffic, *op, rc.first, rc.second, dir);
                double sc = gs.ground * 10.0 + gs.air * 5.0 + gs.early_ground * 6.0;
                if (op->position == "MELEE" && traffic.ground_traffic[rc.first][rc.second] > 0.0) {
                    sc += traffic.ground_traffic[rc.first][rc.second] * 18.0;
                }
                if (u.area == "board" && u.r == rc.first && u.c == rc.second) {
                    sc += 6.0;
                }
                if (sc > best_cell_score) {
                    best_cell_score = sc;
                    best_rc = rc;
                    best_dir = dir;
                }
            }
        }
        used_cells.insert(best_rc);
        current.push_back(PlacementAssign {
            u.slot,
            u.area,
            u.r,
            u.c,
            u.name,
            best_rc.first,
            best_rc.second,
            best_dir,
            u.elite });
    }

    if (current.empty()) {
        return { {}, {} };
    }

    PlacementEvalOptions opts;
    for (const auto& cb : m_active_core_bonds) {
        opts.focus_bonds.insert(cb);
    }
    if (m_goal != "medal") {
        for (const auto& gt : kGoalTasks) {
            if (gt.id == m_goal) {
                opts.focus_bonds.insert(gt.bonds.begin(), gt.bonds.end());
                break;
            }
        }
    }
    opts.focus_bond_mul = (m_goal == "medal") ? 2.0 : 6.0;
    opts.core_bond_mul = 1.5;
    opts.support_defense_weight = 1.0;
    opts.support_heal_weight = 1.0;
    opts.advance = advance_mode;
    opts.round = m_current_round;
    opts.avoid_right_col = (m_current_round >= 9 && m_current_boss.find("左") == std::string::npos);

    std::mt19937 rng(42 + m_current_round * 131 + static_cast<int>(current.size()) * 17);

    auto run_sa_stage = [&](std::vector<PlacementAssign> init_state,
                            int total_iters,
                            auto&& score_fn) -> std::vector<PlacementAssign> {
        std::vector<PlacementAssign> cur = std::move(init_state);
        double cur_sc = score_fn(evaluate_placement_full(cur, bench_rest, traffic, opts));
        std::vector<PlacementAssign> best = cur;
        double best_sc = cur_sc;

        for (int iter = 0; iter < total_iters; ++iter) {
            const double temp = 80.0 * std::pow(0.5 / 80.0, static_cast<double>(iter) / std::max(1, total_iters));
            std::vector<PlacementAssign> next = cur;
            const size_t idx = rng() % next.size();
            const auto* op = find_operator(next[idx].name);
            if (!op) {
                continue;
            }
            const int mut_type = rng() % 3;
            if (mut_type == 0) {
                next[idx].facing = kDirs[rng() % 4];
            }
            else if (mut_type == 1) {
                const auto& pool = (op->position == "RANGED") ? ranged_cells : melee_cells;
                if (!pool.empty()) {
                    auto cand_rc = pool[rng() % pool.size()];
                    bool occupied = false;
                    for (size_t j = 0; j < next.size(); ++j) {
                        if (j != idx && next[j].r == cand_rc.first && next[j].c == cand_rc.second) {
                            occupied = true;
                            break;
                        }
                    }
                    if (!occupied) {
                        next[idx].r = cand_rc.first;
                        next[idx].c = cand_rc.second;
                    }
                }
            }
            else if (next.size() >= 2) {
                size_t j = rng() % next.size();
                if (j != idx) {
                    const auto* op_j = find_operator(next[j].name);
                    if (op_j && op_j->position == op->position) {
                        std::swap(next[idx].r, next[j].r);
                        std::swap(next[idx].c, next[j].c);
                    }
                }
            }

            double next_sc = score_fn(evaluate_placement_full(next, bench_rest, traffic, opts));
            double delta = next_sc - cur_sc;
            if (delta >= 0.0 || std::exp(delta / std::max(0.1, temp)) > std::generate_canonical<double, 10>(rng)) {
                cur = std::move(next);
                cur_sc = next_sc;
                if (cur_sc > best_sc) {
                    best_sc = cur_sc;
                    best = cur;
                }
            }
        }
        return best;
    };

    // Stage 1 (`ss` lines 6412-6417): Maximize defense (`defense - 80 * exposed`)
    auto stage1_best = run_sa_stage(current, 320, [](const PlacementEvalResult& r) {
        return r.defense - r.exposed * 80.0;
    });
    const PlacementEvalResult f_eval = evaluate_placement_full(stage1_best, bench_rest, traffic, opts);
    const double w_def_floor = f_eval.defense - std::max(80.0, std::abs(f_eval.defense) * 0.12);
    const double i_exp_ceil = f_eval.exposed;
    const double m_gate_ceil = f_eval.gate;

    // Stage 2 (`ss` lines 6418-6433): Maximize stacking + advance while keeping defense >= w_def_floor
    auto best_assign = run_sa_stage(stage1_best, 260, [&](const PlacementEvalResult& r) {
        if (r.exposed > i_exp_ceil) {
            return -1e9 - r.exposed * 1e4;
        }
        if (r.gate > m_gate_ceil) {
            return -1e9 - r.gate * 1e4;
        }
        if (r.defense < w_def_floor) {
            return -1e6 + r.defense;
        }
        return r.stacking + (opts.advance ? r.advance_score : 0.0) + r.defense * 0.25;
    });

    // Build move sequence (`ws` in service.pretty.mjs lines 7560-7646)
    std::vector<PlacementMove> moves;
    for (const auto& u : m_units) {
        if (u.area != "board" || u.auxiliary || u.ghost) {
            continue;
        }
        bool still_on_board = false;
        for (const auto& pa : best_assign) {
            if (pa.slot == u.slot) {
                still_on_board = true;
                break;
            }
        }
        if (!still_on_board) {
            for (int bc = 3; bc <= 11; ++bc) {
                bool taken = false;
                for (const auto& bu : m_units) {
                    if (bu.area == "bench" && bu.c == bc) {
                        taken = true;
                        break;
                    }
                }
                if (!taken) {
                    moves.push_back(PlacementMove {
                        "bench", u.name, "board", u.r, u.c, "bench", 6, bc, "" });
                    break;
                }
            }
        }
    }

    for (const auto& pa : best_assign) {
        const auto* op = find_operator(pa.name);
        const bool face_indep = (op && op->facing_independent);
        if (pa.from_area == "bench") {
            moves.push_back(PlacementMove {
                "deploy",
                pa.name,
                "bench",
                pa.from_r,
                pa.from_c,
                "board",
                pa.r,
                pa.c,
                pa.facing });
        }
        else if (pa.from_r != pa.r || pa.from_c != pa.c) {
            moves.push_back(PlacementMove {
                "reposition",
                pa.name,
                "board",
                pa.from_r,
                pa.from_c,
                "board",
                pa.r,
                pa.c,
                pa.facing });
        }
        else if (!face_indep) {
            for (const auto& u : m_units) {
                if (u.slot == pa.slot && !u.facing.empty() && u.facing != pa.facing) {
                    moves.push_back(PlacementMove {
                        "orient",
                        pa.name,
                        "board",
                        pa.r,
                        pa.c,
                        "board",
                        pa.r,
                        pa.c,
                        pa.facing });
                    break;
                }
            }
        }
    }

    return { best_assign, moves };
}

std::optional<std::pair<double, double>> SPAFarmTaskPlugin::detect_facing_white_box(const cv::Mat& img)
{
    // Exact 1:1 port of `Ei(a)` in service.pretty.mjs lines 4907-4944:
    // Downsamples to 960x540, finds bright neutral pixels (`min > 200, max - min < 40`),
    // extracts 4-connected components inside x: 200..780, y: 40..490, and matches bounding box
    // width 110..235, height 80..200, fill ratio 0.06..0.55.
    if (img.empty()) {
        return std::nullopt;
    }
    cv::Mat small_img;
    cv::resize(img, small_img, cv::Size(960, 540), 0.0, 0.0, cv::INTER_AREA);
    const int W = 960;
    const int H = 540;
    std::vector<uint8_t> mask(W * H, 0);
    for (int y = 40; y < 490; ++y) {
        const auto* row = small_img.ptr<cv::Vec3b>(y);
        for (int x = 200; x < 780; ++x) {
            const int B = row[x][0], G = row[x][1], R = row[x][2];
            const int mx = std::max({ R, G, B });
            const int mn = std::min({ R, G, B });
            if (mn > 200 && (mx - mn) < 40) {
                mask[y * W + x] = 1;
            }
        }
    }

    std::vector<uint8_t> vis(W * H, 0);
    std::optional<std::pair<double, double>> best_center;
    int best_n = 0;

    for (int y = 40; y < 490; ++y) {
        for (int x = 200; x < 780; ++x) {
            int idx = y * W + x;
            if (!mask[idx] || vis[idx]) {
                continue;
            }
            int x0 = x, x1 = x, y0 = y, y1 = y, n = 0;
            std::vector<int> stack = { idx };
            vis[idx] = 1;
            while (!stack.empty()) {
                int cur = stack.back();
                stack.pop_back();
                int cx = cur % W;
                int cy = cur / W;
                x0 = std::min(x0, cx);
                x1 = std::max(x1, cx);
                y0 = std::min(y0, cy);
                y1 = std::max(y1, cy);
                ++n;
                const int nbrs[4] = { cur - 1, cur + 1, cur - W, cur + W };
                for (int nb : nbrs) {
                    if (nb >= 0 && nb < W * H && mask[nb] && !vis[nb]) {
                        vis[nb] = 1;
                        stack.push_back(nb);
                    }
                }
            }
            int bw = x1 - x0 + 1;
            int bh = y1 - y0 + 1;
            double fill = static_cast<double>(n) / static_cast<double>(bw * bh);
            if (bw >= 110 && bw <= 235 && bh >= 80 && bh <= 200 && fill >= 0.06 && fill <= 0.55 && n > best_n) {
                best_n = n;
                best_center = { (x0 + x1) * 1.0, (y0 + y1) * 1.0 }; // * 0.5 * 2.0 = * 1.0 in 1920x1080
            }
        }
    }
    return best_center;
}

bool SPAFarmTaskPlugin::execute_placement_move(const PlacementMove& mv, const CameraDetectResult& cam)
{
    // Exact 1:1 port of `At(I)` in service.pretty.mjs lines 9724-9983
    const auto H = active_board_homography(cam);
    auto [fx, fy] = project_point(H, mv.from_c + 0.5, mv.from_r + 0.5);
    auto [tx, ty] = project_point(H, mv.to_c + 0.5, mv.to_r + 0.5);

    if (mv.type == "orient") {
        tap(tx, ty, 200);
    }
    else {
        drag_board_unit(fx, fy, tx, ty, false);
        sleep_ms(static_cast<int>(std::lround(150.0 * m_step_scale)));
    }

    if (mv.to_area != "board") {
        close_detail_panel();
        return true;
    }

    // Poll up to 5 times for white facing box (`Ei`)
    std::optional<std::pair<double, double>> box_center;
    for (int i = 0; i < 5 && !need_exit(); ++i) {
        cv::Mat cur = shot();
        box_center = detect_facing_white_box(cur);
        if (box_center.has_value()) {
            break;
        }
        sleep_ms(120);
    }

    double cx = box_center ? box_center->first : tx;
    double cy = box_center ? box_center->second : ty;

    auto dir_delta = [](std::string_view dir) -> std::pair<double, double> {
        if (dir == "RIGHT") {
            return { 210.0, 0.0 };
        }
        if (dir == "LEFT") {
            return { -210.0, 0.0 };
        }
        if (dir == "UP") {
            return { 0.0, -180.0 };
        }
        if (dir == "DOWN") {
            return { 0.0, 180.0 };
        }
        return { -210.0, 0.0 };
    };

    auto [dx, dy] = dir_delta(mv.facing);
    for (int attempt = 0; attempt < 3 && !need_exit(); ++attempt) {
        drag_segments(cx, cy, cx + dx, cy + dy, 6, 20, 80);
        sleep_ms(220);
        cv::Mat after = shot();
        auto still_box = detect_facing_white_box(after);
        if (!still_box.has_value()) {
            // Update local unit state
            std::string old_slot = (mv.from_area == "bench")
                ? ("bench:" + std::to_string(mv.from_c + 1))
                : ("board:" + std::to_string(mv.from_r) + "," + std::to_string(mv.from_c));
            std::string new_slot = "board:" + std::to_string(mv.to_r) + "," + std::to_string(mv.to_c);
            for (auto& u : m_units) {
                if (u.slot == old_slot) {
                    u.slot = new_slot;
                    u.area = "board";
                    u.r = mv.to_r;
                    u.c = mv.to_c;
                    u.facing = mv.facing;
                    m_remembered_units[new_slot] = u;
                    break;
                }
            }
            if (old_slot != new_slot) {
                m_remembered_units.erase(old_slot);
            }
            close_detail_panel();
            return true;
        }
        cx = still_box->first;
        cy = still_box->second;
    }

    close_detail_panel();
    return false;
}

int SPAFarmTaskPlugin::run_auto_place(const CameraDetectResult& cam, bool advance_mode)
{
    // Exact 1:1 port of `we(I = {})` in service.pretty.mjs lines 9724-9983:
    // ① If `收起` is visible on screen, tap `收起` (300ms) first so the shop panel does not occlude the board.
    // ② Call `wait_stable_board(8000)`.
    // ③ If unstable, call `close_detail_panel()` + `tap(1150, 950, 800)` + `wait_stable_board(6000)`.
    // ④ If still unstable, log `"部署前棋盘定位未稳定"` and abort deployment cleanly.
    // ⑤ Before executing moves, call `wait_stable_board(5000)` (fallback `close_detail_panel()` + `wait_stable_board(5000)`),
    //   resolve swap cycles via a temporary empty bench/board slot (`Re`/`xe`), and after moves call `sleep_ms(300)` + `wait_stable_board(4000)`.
    (void)cam;
    cv::Mat init_img = shot();
    if (!init_img.empty()) {
        auto init_ocr = ocr(init_img);
        if (auto collapse_btn = find_ocr(init_ocr, { "收起" })) {
            tap(collapse_btn->cx, collapse_btn->cy, static_cast<int>(std::lround(300.0 * m_step_scale)));
        }
    }

    auto st = wait_stable_board(8000);
    if (!st.has_value()) {
        close_detail_panel();
        tap(1150, 950, static_cast<int>(std::lround(800.0 * m_step_scale)));
        st = wait_stable_board(6000);
    }
    if (!st.has_value()) {
        log_info("部署前棋盘定位未稳定");
        return 0;
    }

    CameraDetectResult cur_cam { st->camera, st->raw_H, st->fit };
    refresh_board_snapshot(st->img, cur_cam, false);
    inspect_unknown_units(cur_cam, true);

    StageTrafficMap traffic = build_stage_traffic(m_stage_id);
    auto [assigns, moves] = solve_placement(traffic, m_deploy_cap, advance_mode);
    if (moves.empty()) {
        return 0;
    }

    auto pre_mv_st = wait_stable_board(5000);
    if (!pre_mv_st.has_value()) {
        close_detail_panel();
        pre_mv_st = wait_stable_board(5000);
    }
    if (!pre_mv_st.has_value()) {
        log_info("移动前棋盘未稳定");
        return 0;
    }
    cur_cam = CameraDetectResult { pre_mv_st->camera, pre_mv_st->raw_H, pre_mv_st->fit };

    // Resolve move ordering and break swap cycles using a free bench/board cell (`L9815-9898`)
    std::set<std::pair<int, int>> occupied_cells;
    for (const auto& u : m_units) {
        if ((u.area == "board" && !u.ghost) || u.area == "bench") {
            occupied_cells.emplace(u.r, u.c);
        }
    }

    auto find_temp_cell = [&]() -> std::optional<std::tuple<std::string, int, int>> {
        for (int bc = 3; bc <= 11; ++bc) {
            if (occupied_cells.count({ 6, bc }) == 0) {
                return std::make_tuple(std::string("bench"), 6, bc);
            }
        }
        for (int r = 1; r <= 4 && r < traffic.rows; ++r) {
            for (int c = 0; c < traffic.cols; ++c) {
                char ch = traffic.grid[r][c];
                if ((ch == 'a' || ch == 'b' || ch == 'h') && occupied_cells.count({ r, c }) == 0) {
                    return std::make_tuple(std::string("board"), r, c);
                }
            }
        }
        return std::nullopt;
    };

    std::vector<PlacementMove> pending = moves;
    int executed = 0;
    while (!pending.empty() && !need_exit()) {
        // 1. Find a move whose destination is currently unoccupied (or is an in-place orient)
        int ready_idx = -1;
        for (size_t i = 0; i < pending.size(); ++i) {
            const auto& mv = pending[i];
            if (mv.type == "orient"
                || (mv.from_r == mv.to_r && mv.from_c == mv.to_c)
                || occupied_cells.count({ mv.to_r, mv.to_c }) == 0) {
                ready_idx = static_cast<int>(i);
                break;
            }
        }

        if (ready_idx >= 0) {
            PlacementMove mv = pending[ready_idx];
            pending.erase(pending.begin() + ready_idx);
            log_info(
                "布阵动作: " + mv.type + " " + mv.unit + " -> ("
                + std::to_string(mv.to_r) + "," + std::to_string(mv.to_c) + ") 朝向:" + mv.facing);
            if (execute_placement_move(mv, cur_cam)) {
                occupied_cells.erase({ mv.from_r, mv.from_c });
                occupied_cells.emplace(mv.to_r, mv.to_c);
                ++executed;
            }
            continue;
        }

        // 2. Cycle detected: park `pending.front()` to a temporary free cell first
        auto temp_cell = find_temp_cell();
        if (!temp_cell.has_value()) {
            PlacementMove mv = pending.front();
            pending.erase(pending.begin());
            if (execute_placement_move(mv, cur_cam)) {
                ++executed;
            }
            continue;
        }

        auto [t_area, tr, tc] = *temp_cell;
        PlacementMove& cyc_mv = pending.front();
        PlacementMove park_mv {
            t_area == "bench" ? "bench" : "reposition",
            cyc_mv.unit,
            cyc_mv.from_area,
            cyc_mv.from_r,
            cyc_mv.from_c,
            t_area,
            tr,
            tc,
            cyc_mv.facing.empty() ? "LEFT" : cyc_mv.facing
        };
        if (execute_placement_move(park_mv, cur_cam)) {
            occupied_cells.erase({ cyc_mv.from_r, cyc_mv.from_c });
            occupied_cells.emplace(tr, tc);
            cyc_mv.from_area = t_area;
            cyc_mv.from_r = tr;
            cyc_mv.from_c = tc;
            cyc_mv.type = (t_area == "bench") ? "deploy" : "reposition";
        }
        else {
            pending.erase(pending.begin());
        }
    }

    sleep_ms(static_cast<int>(std::lround(300.0 * m_step_scale)));
    auto post_st = wait_stable_board(4000);
    if (!post_st.has_value()) {
        log_info("部署后棋盘未稳定");
        return executed;
    }
    cur_cam = CameraDetectResult { post_st->camera, post_st->raw_H, post_st->fit };
    refresh_board_snapshot(post_st->img, cur_cam, false);
    return executed;
}

// ============================================================================
// Shop Multi-Step Action Planner & Execution (`_r`, `gs`, `ps`, `Lr`, `ha`, `ur`, `ct`, `oe`)
// ============================================================================

SPAFarmTaskPlugin::RoundPacing SPAFarmTaskPlugin::get_round_pacing(int round)
{
    // Exact 1:1 port of `hs(a)` in service.pretty.mjs lines 7972-7982
    int r = std::max(1, round);
    if (r == 1) {
        return { 1, 3, 2, false };
    }
    if (r == 2) {
        return { 2, 4, 3, false };
    }
    if (r == 3) {
        return { 3, 5, 4, false };
    }
    return { r, 6, 5, r >= 9 };
}

double SPAFarmTaskPlugin::score_operator_for_plan(
    const OperatorInfo& op,
    const std::unordered_map<std::string, int>& owned_counts,
    const std::unordered_set<std::string>& active_bonds) const
{
    // Combines `ps` (quick/medal mode, lines 8184-8267) and `gs` (task mode, lines 7984-8105)
    double score = op.tier * 2.5 + op.dps_weight * 4.0;
    int copies = 0;
    if (auto it = owned_counts.find(op.name); it != owned_counts.end()) {
        copies = it->second;
    }

    if (m_goal == "medal") {
        const bool is_stacker = std::find(kQuickStackers.begin(), kQuickStackers.end(), op.name) != kQuickStackers.end();
        const bool is_finisher = std::find(kQuickFinishers.begin(), kQuickFinishers.end(), op.name) != kQuickFinishers.end();
        bool has_core_bond = false;
        for (const auto& cb : m_active_core_bonds) {
            if (std::find(op.bond_names.begin(), op.bond_names.end(), cb) != op.bond_names.end()) {
                has_core_bond = true;
                break;
            }
        }
        if (is_finisher) {
            score += 48.0;
        }
        if (is_stacker) {
            score += 36.0;
        }
        if (has_core_bond) {
            score += (copies == 0) ? 28.0 : 12.0;
        }
        // Check profile priority list (`kPriorityFull` / `kPriorityLow`)
        const auto& prio_list = (m_profile == "low") ? kPriorityLow : kPriorityFull;
        for (size_t i = 0; i < prio_list.size(); ++i) {
            if (op.name.find(prio_list[i].first) != std::string::npos
                || op.name.find(prio_list[i].second) != std::string::npos) {
                score += std::max(8.0, 32.0 - static_cast<double>(i) * 1.2);
                break;
            }
        }
    }
    else {
        for (const auto& gt : kGoalTasks) {
            if (gt.id == m_goal) {
                bool has_goal_bond = false;
                for (const auto& gb : gt.bonds) {
                    if (std::find(op.bond_names.begin(), op.bond_names.end(), gb) != op.bond_names.end()) {
                        score += (copies == 0) ? 42.0 : 18.0;
                        has_goal_bond = true;
                    }
                }
                if (has_goal_bond && (op.trait_text.find("叠加") != std::string::npos || op.trait_text.find("层数") != std::string::npos)) {
                    score += 32.0;
                }
                break;
            }
        }
    }

    if (!op.stack_info.rules.empty() || op.stack_info.on_skill || op.stack_info.on_deploy) {
        score += 18.0;
    }
    if (copies == 1) {
        score += 10.0; // pair bonus
    }
    else if (copies == 2) {
        score += 38.0; // triple merge to elite!
    }
    for (const auto& b : op.bond_names) {
        if (active_bonds.count(b) > 0) {
            score += 6.0;
        }
    }
    return score;
}

std::vector<SPAFarmTaskPlugin::ShopPlanStep> SPAFarmTaskPlugin::plan_shop_Steps(
    const ShopHeaderState& header,
    const std::vector<ShopCard>& shop)
{
    // Exact 1:1 port of `ha` (`Lr` / `_r`) in service.pretty.mjs lines 8106-8390
    std::vector<ShopPlanStep> steps;
    int gold = header.gold.value_or(m_current_gold);
    int shop_lv = header.level.value_or(m_current_shop_level);
    int up_cost = header.upgrade_cost.value_or(99);
    const RoundPacing pacing = get_round_pacing(std::max(1, m_current_round));

    std::unordered_map<std::string, int> owned_counts;
    int board_cnt = 0;
    int bench_cnt = 0;
    for (const auto& u : m_units) {
        if (!u.name.empty() && !u.auxiliary) {
            owned_counts[u.name]++;
        }
        if (u.area == "board" && !u.auxiliary) {
            ++board_cnt;
        }
        else if (u.area == "bench") {
            ++bench_cnt;
        }
    }

    std::unordered_set<std::string> active_bonds(m_active_core_bonds.begin(), m_active_core_bonds.end());

    // 1. Always claim free cards (`freeCards`) first!
    for (const auto& card : shop) {
        if (card.free_card && !card.name.empty() && bench_cnt < 9) {
            steps.push_back(ShopPlanStep {
                "buy", card.name, card.kind, card.x, 0, "", "", "免费卡牌0费必拿" });
            owned_counts[card.name]++;
            ++bench_cnt;
        }
    }

    // 2. Check if we should upgrade shop toward `pacing.target_shop_level`
    const int total_combat_units = board_cnt + bench_cnt;
    if (shop_lv < pacing.target_shop_level && header.upgrade_btn.has_value() && gold >= up_cost) {
        bool defense_safe = (total_combat_units >= pacing.min_deploy)
            || (m_current_round <= 2 && gold >= up_cost + 2)
            || (up_cost <= 3);
        if (defense_safe) {
            steps.push_back(ShopPlanStep {
                "upgrade",
                "商店Lv" + std::to_string(shop_lv + 1),
                "upgrade",
                header.upgrade_btn->first,
                up_cost,
                "",
                "",
                "按回合节奏升本至Lv" + std::to_string(pacing.target_shop_level) });
            gold -= up_cost;
            ++shop_lv;
        }
    }

    // 3. Score remaining shop cards and buy high-value operators / bond items
    struct ScoredCard
    {
        ShopCard card;
        double score = 0.0;
    };
    std::vector<ScoredCard> scored;
    for (const auto& card : shop) {
        if (card.free_card || card.name.empty()) {
            continue;
        }
        if (card.kind == "operator") {
            if (const auto* op = find_operator(card.name)) {
                double sc = score_operator_for_plan(*op, owned_counts, active_bonds);
                if (total_combat_units < pacing.min_deploy) {
                    sc += 30.0; // Need bodies to fill min_deploy
                }
                scored.push_back(ScoredCard { card, sc });
            }
        }
        else if (card.kind == "item") {
            if (const auto* it = find_item(card.name)) {
                const std::string cat = it->category.empty()
                    ? classify_item_category(it->name, it->effect)
                    : it->category;
                if (it->is_debuff || cat == "debuff" || m_bought_items.count(it->name) > 0) {
                    continue;
                }
                double item_sc = -1.0;
                if (!it->granted_bond_name.empty()) {
                    bool useful_bond = std::find(
                                           m_active_core_bonds.begin(),
                                           m_active_core_bonds.end(),
                                           it->granted_bond_name)
                        != m_active_core_bonds.end();
                    if (useful_bond) {
                        item_sc = (m_goal == "medal") ? 46.0 : 95.0;
                    }
                }
                else if (cat == "stack") {
                    item_sc = 42.0;
                }
                else if (cat == "utility" && (it->effect.find("最大可部署人数") != std::string::npos || it->effect.find("晋升") != std::string::npos || it->effect.find("资金") != std::string::npos)) {
                    item_sc = 38.0;
                }
                else if ((cat == "output" || cat == "defense") && shop_lv >= pacing.target_shop_level && m_current_round >= 4) {
                    item_sc = 28.0;
                }
                if (item_sc > 0.0) {
                    scored.push_back(ScoredCard { card, item_sc });
                }
            }
        }
    }

    std::sort(scored.begin(), scored.end(), [](const ScoredCard& a, const ScoredCard& b) {
        return a.score > b.score;
    });

    const double buy_threshold = (shop_lv < pacing.target_shop_level) ? 42.0 : 26.0;
    for (const auto& sc : scored) {
        if (gold < sc.card.cost) {
            continue;
        }
        if (sc.score < buy_threshold && total_combat_units >= pacing.min_deploy) {
            continue;
        }
        if (sc.card.kind == "operator" && bench_cnt >= 9) {
            // Find weakest bench filler to swap (`swap` step)
            std::string weakest_slot;
            std::string weakest_name;
            double weakest_score = 1e9;
            for (const auto& u : m_units) {
                if (u.area == "bench" && !u.name.empty() && !u.elite) {
                    if (const auto* uop = find_operator(u.name)) {
                        double usc = score_operator_for_plan(*uop, owned_counts, active_bonds);
                        if (usc < weakest_score) {
                            weakest_score = usc;
                            weakest_slot = u.slot;
                            weakest_name = u.name;
                        }
                    }
                }
            }
            if (!weakest_slot.empty() && sc.score > weakest_score + 18.0) {
                steps.push_back(ShopPlanStep {
                    "swap",
                    sc.card.name,
                    sc.card.kind,
                    sc.card.x,
                    sc.card.cost,
                    weakest_slot,
                    weakest_name,
                    "出售低分候补换购高优干员" });
                gold = gold + 1 - sc.card.cost;
                owned_counts[sc.card.name]++;
            }
            continue;
        }

        steps.push_back(ShopPlanStep {
            "buy",
            sc.card.name,
            sc.card.kind,
            sc.card.x,
            sc.card.cost,
            "",
            "",
            "评分:" + std::to_string(static_cast<int>(sc.score)) });
        gold -= sc.card.cost;
        if (sc.card.kind == "operator") {
            owned_counts[sc.card.name]++;
            ++bench_cnt;
        }
    }

    // 4. Refresh if shop is at target level and gold is sufficient
    if (steps.empty() && shop_lv >= pacing.target_shop_level && gold >= 4) {
        steps.push_back(ShopPlanStep { "refresh", "刷新商店", "refresh", 1786, 2, "", "", "高本搜卡" });
    }
    else if (steps.empty()) {
        steps.push_back(ShopPlanStep { "done", "", "", 0, 0, "", "", "本轮商店操作完成" });
    }
    return steps;
}

bool SPAFarmTaskPlugin::buy_shop_card(int card_x, const std::string& expected_name, int upgrade_x_hint)
{
    // Exact 1:1 port of `Tt(I, k)` (`ur`) in service.pretty.mjs lines 8857-8907:
    // Snaps x to 243px grid from upgrade_x_hint if available, taps (B + 65, 900),
    // polls up to 1500ms checking [B - 80, 985, 320, 80] for `/确认购买/` and [20, 265, 560, 70]
    // for the detail panel name (`ot`). If name mismatches expected_name, closes detail and aborts!
    // Otherwise taps (B + 65, 1025) to confirm purchase.
    int B = card_x;
    if (upgrade_x_hint > 0) {
        int slot_k = std::max(1, static_cast<int>(std::lround(static_cast<double>(card_x + 65 - upgrade_x_hint) / 243.0)));
        B = upgrade_x_hint + 243 * slot_k - 65;
    }

    tap(B + 65, 900, 180);

    bool confirmed_ui = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 1500
           && !need_exit()) {
        cv::Mat cur = shot();
        auto btn_ocr = ocr(cur, std::array<int, 4> { std::max(0, B - 80), 985, 320, 80 });
        auto title_ocr = ocr(cur, std::array<int, 4> { 20, 265, 560, 70 });

        std::string panel_title;
        for (const auto& it : title_ocr) {
            panel_title += it.text;
        }
        if (!expected_name.empty() && !panel_title.empty()) {
            auto cm = match_dict(panel_title, m_chess_dict);
            auto im = match_dict(panel_title, m_item_dict);
            std::string det_name = !cm.empty() ? cm.front().name : (!im.empty() ? im.front().name : "");
            if (!det_name.empty() && norm_text(det_name) != norm_text(expected_name)) {
                log_info("购买核名不符: 期望 " + expected_name + " 实测 " + det_name + "，取消购买");
                close_detail_panel();
                return false;
            }
        }
        if (has_ocr(btn_ocr, { "确认购买" }) || !panel_title.empty()) {
            confirmed_ui = true;
            break;
        }
        sleep_ms(120);
    }

    if (!confirmed_ui) {
        close_detail_panel();
        return false;
    }

    tap(B + 65, 1025, 380);
    if (!expected_name.empty()) {
        if (find_operator(expected_name)) {
            m_recent_bought_names.push_back(expected_name);
            if (m_recent_bought_names.size() > 10) {
                m_recent_bought_names.erase(m_recent_bought_names.begin());
            }
        }
        else if (find_item(expected_name)) {
            m_bought_items.insert(expected_name);
        }
    }
    return true;
}

bool SPAFarmTaskPlugin::upgrade_shop(const ShopHeaderState& header)
{
    // Exact 1:1 port of `ct(I)` in service.pretty.mjs lines 9057-9084:
    // Taps upgrade button (k, D), then polls up to 1500ms in [k - 130, D - 120, 260, 200]
    // for `/确认升级/` and clicks its center if present.
    if (!header.upgrade_btn.has_value()) {
        return false;
    }
    const int k = header.upgrade_btn->first;
    const int D = header.upgrade_btn->second;
    tap(k, D, 200);

    const std::array<int, 4> confirm_roi = {
        std::max(0, k - 130),
        std::max(0, D - 120),
        260,
        200
    };
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 1500
           && !need_exit()) {
        cv::Mat cur = shot();
        auto items = ocr(cur, confirm_roi);
        if (auto hit = find_ocr(items, { "确认升级" })) {
            tap(hit->cx, hit->cy, 350);
            return true;
        }
        // Check if upgrade already took effect without a popup
        auto next_hdr = read_shop_header(cur, ocr(cur));
        if ((header.level.has_value() && next_hdr.level.has_value() && *next_hdr.level > *header.level)
            || (header.gold.has_value() && next_hdr.gold.has_value() && *next_hdr.gold < *header.gold)) {
            return true;
        }
        sleep_ms(140);
    }
    return true;
}

bool SPAFarmTaskPlugin::sell_unit_slot(const std::string& slot, const CameraDetectResult& cam)
{
    // Exact 1:1 port of `oe(I)` in service.pretty.mjs lines 8968-9015 (including `waitStableBoard(4e3)` L8970)
    (void)cam;
    auto it = std::find_if(m_units.begin(), m_units.end(), [&](const UnitSlot& u) {
        return u.slot == slot;
    });
    if (it == m_units.end()) {
        return false;
    }
    auto st = wait_stable_board(4000);
    if (!st.has_value()) {
        close_detail_panel();
        st = wait_stable_board(4000);
    }
    if (!st.has_value()) {
        return false;
    }
    auto [fx, fy] = project_point(st->H, it->c + 0.5, it->r + 0.5);
    cv::Mat held_img = drag_board_unit(fx, fy, 1780, 930, true);
    if (!held_img.empty()) {
        auto sell_ocr = ocr(held_img, std::array<int, 4> { 1550, 780, 350, 260 });
        if (has_ocr(sell_ocr, { "不可出售" })) {
            it->auxiliary = true;
            m_remembered_units[it->slot] = *it;
            close_detail_panel();
            return false;
        }
    }
    sleep_ms(static_cast<int>(std::lround(700.0 * m_step_scale)));
    close_detail_panel();
    m_remembered_units.erase(slot);
    m_units.erase(it);
    return true;
}

// ============================================================================
// Equipment Classification (`Es`/`Fs`/`ma`/`vs`/`An`/`ga`), Row `Q`, Jessica Pager, Mob Elite & Summons
// ============================================================================

std::string SPAFarmTaskPlugin::classify_item_category(std::string_view name, std::string_view effect)
{
    // Exact 1:1 port of `Es`, `Fs`, `ma`, `vs`, and `An(a)` in service.pretty.mjs lines 8391-8399:
    // - Es = /流失|无法被治疗|生命上限-|阻挡数-1|攻击速度-|更容易受到攻击(?!.*生命上限\+)/
    // - Fs = /资金|刷新|促销|最大可部署人数|指令|晋升|调度中心/
    // - ma = /层数|盟约|身份牌|推荐信|初始干员/
    // - vs = /生命上限\+|防御力\+|阻挡数\+|复活|护盾|免疫|致命伤害|隐匿/
    if (name == "信标") {
        return "debuff";
    }
    if (name == "博士投影") {
        return "output";
    }

    auto has_any = [](std::string_view text, std::initializer_list<std::string_view> kws) {
        for (std::string_view kw : kws) {
            if (!kw.empty() && text.find(kw) != std::string_view::npos) {
                return true;
            }
        }
        return false;
    };

    const bool match_ma = has_any(effect, { "层数", "盟约", "身份牌", "推荐信", "初始干员" });
    const bool match_Fs = has_any(effect, { "资金", "刷新", "促销", "最大可部署人数", "指令", "晋升", "调度中心" });
    const bool match_Es = has_any(effect, { "流失", "无法被治疗", "生命上限-", "阻挡数-1", "攻击速度-" })
        || (effect.find("更容易受到攻击") != std::string_view::npos && effect.find("生命上限+") == std::string_view::npos);
    const bool match_vs = has_any(effect, { "生命上限+", "防御力+", "阻挡数+", "复活", "护盾", "免疫", "致命伤害", "隐匿" });
    const bool match_atk_dmg = has_any(effect, { "攻击力+", "伤害" });

    if (effect.find("装备时销毁") != std::string_view::npos) {
        return match_ma ? "stack" : "utility";
    }
    if (match_Es) {
        return "debuff";
    }
    if (match_Fs) {
        return "utility";
    }
    if (match_ma) {
        return "stack";
    }
    if (match_vs && !match_atk_dmg) {
        return "defense";
    }
    return "output";
}

const SPAFarmTaskPlugin::UnitSlot* SPAFarmTaskPlugin::pick_equipment_target(
    std::string_view category,
    const ItemInfo* item_info) const
{
    // Exact 1:1 port of `ga(a, s, e, t, f = null)` in service.pretty.mjs lines 8401-8414
    if (category == "debuff") {
        return nullptr;
    }

    const UnitSlot* best_target = nullptr;
    double best_score = -1e18;

    for (const auto& c : m_units) {
        if (c.area != "board" || c.auxiliary || c.ghost || c.name.empty()) {
            continue;
        }
        const auto* op = find_operator(c.name);
        if (!op) {
            continue;
        }
        if (item_info && !item_info->granted_bond_name.empty()) {
            if (std::find(op->bond_names.begin(), op->bond_names.end(), item_info->granted_bond_name)
                != op->bond_names.end()) {
                continue;
            }
        }

        const double val_i = 200.0 * (1.0 + 0.25 * (op->tier - 1)) * (c.elite ? 1.4 : 1.0);
        int eq_cnt = 0;
        if (auto eq_it = m_unit_equipped_count.find(c.name); eq_it != m_unit_equipped_count.end()) {
            eq_cnt = eq_it->second;
        }
        const double val_m = -static_cast<double>(eq_cnt) * 150.0;

        const bool is_stacker = !op->stack_info.rules.empty() || op->stack_info.on_skill
            || op->stack_info.on_deploy || op->stack_info.end_of_prep
            || (std::find(kQuickStackers.begin(), kQuickStackers.end(), op->name) != kQuickStackers.end());
        bool has_core_bond = false;
        for (const auto& cb : m_active_core_bonds) {
            if (std::find(op->bond_names.begin(), op->bond_names.end(), cb) != op->bond_names.end()) {
                has_core_bond = true;
                break;
            }
        }

        double u_sc = 0.0;
        if (category == "stack") {
            u_sc = (is_stacker ? 1000.0 : 0.0) + (has_core_bond ? 500.0 : 0.0) + val_i / 10.0 + val_m;
        }
        else if (category == "defense") {
            u_sc = (op->position == "MELEE" ? 1000.0 : 0.0) + op->block_cnt * 100.0 + op->max_hp / 100.0 + val_m;
        }
        else if (category == "utility") {
            u_sc = val_m;
        }
        else {
            u_sc = val_i + (op->position == "RANGED" ? 50.0 : 0.0) + val_m;
        }

        if (u_sc > best_score) {
            best_score = u_sc;
            best_target = &c;
        }
    }
    return best_target;
}

void SPAFarmTaskPlugin::handle_overflow_units(const CameraDetectResult& cam)
{
    // Exact 1:1 port of `nn(I)` in service.pretty.mjs lines 9209-9248 (with `waitStableBoard(4e3)` L9213, L9232)
    (void)cam;
    auto st = wait_stable_board(4000);
    if (!st.has_value()) {
        return;
    }
    for (int c = 4; c <= 9 && !need_exit(); ++c) {
        auto [fx, fy] = project_point(st->H, c + 0.5, 5.5);
        int x0 = std::clamp(static_cast<int>(std::lround(fx - 22)), 0, st->img.cols);
        int y0 = std::clamp(static_cast<int>(std::lround(fy - 22)), 0, st->img.rows);
        int x1 = std::clamp(static_cast<int>(std::lround(fx + 22)), 0, st->img.cols);
        int y1 = std::clamp(static_cast<int>(std::lround(fy + 22)), 0, st->img.rows);
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        cv::Scalar mean, stddev;
        cv::meanStdDev(st->img(cv::Rect(x0, y0, x1 - x0, y1 - y0)), mean, stddev);
        if ((stddev[0] + stddev[1] + stddev[2]) / 3.0 < 28.0) {
            continue;
        }
        tap(fx, fy, 220);
        cv::Mat pimg = shot();
        if (!is_detail_panel_open(pimg)) {
            continue;
        }
        auto [det_name, unused_facing] = read_unit_detail_panel(pimg);
        (void)unused_facing;
        close_detail_panel();

        const auto* op = det_name.empty() ? nullptr : find_operator(det_name);
        if (!op) {
            continue; // Not an operator overflow (normal equipment item)
        }

        // Find an empty bench slot `3..11`
        int free_bc = -1;
        for (int bc = 3; bc <= 11; ++bc) {
            bool taken = false;
            for (const auto& u : m_units) {
                if (u.area == "bench" && u.c == bc) {
                    taken = true;
                    break;
                }
            }
            if (!taken) {
                free_bc = bc;
                break;
            }
        }
        if (free_bc >= 0) {
            if (auto mv_st = wait_stable_board(4000)) {
                auto [sx, sy] = project_point(mv_st->H, c + 0.5, 5.5);
                auto [tx, ty] = project_point(mv_st->H, free_bc + 0.5, 6.5);
                log_info("回收暂存区干员: " + det_name + " -> 候补席 " + std::to_string(free_bc + 1));
                drag_board_unit(sx, sy, tx, ty, false);
                sleep_ms(300);
            }
        }
    }
}

void SPAFarmTaskPlugin::scan_and_distribute_equipment(const cv::Mat& img, const CameraDetectResult& cam)
{
    // Exact 1:1 port of `Vt` + `on` + `Cr` in service.pretty.mjs lines 9419-9515 (including `waitStableBoard` L9420, L9432, L9458, L9461)
    (void)img;
    (void)cam;
    handle_overflow_units(cam);

    auto init_st = wait_stable_board(3000);
    auto st = wait_stable_board(4000, init_st ? std::optional<std::array<double, 4>>(init_st->pts) : std::nullopt);
    if (!st.has_value()) {
        log_info("装备：棋盘未稳定，本回合跳过");
        return;
    }

    std::vector<int> active_cols;
    for (int c = 4; c <= 9; ++c) {
        auto [px, py] = project_point(st->H, c + 0.5, 5.5);
        int x0 = std::clamp(static_cast<int>(std::lround(px - 22)), 0, st->img.cols);
        int y0 = std::clamp(static_cast<int>(std::lround(py - 22)), 0, st->img.rows);
        int x1 = std::clamp(static_cast<int>(std::lround(px + 22)), 0, st->img.cols);
        int y1 = std::clamp(static_cast<int>(std::lround(py + 22)), 0, st->img.rows);
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        cv::Scalar mean, stddev;
        cv::meanStdDev(st->img(cv::Rect(x0, y0, x1 - x0, y1 - y0)), mean, stddev);
        double avg_std = (stddev[0] + stddev[1] + stddev[2]) / 3.0;
        if (avg_std >= 28.0) {
            active_cols.push_back(c);
        }
    }

    for (int c : active_cols) {
        if (need_exit()) {
            return;
        }
        auto slot_st = wait_stable_board(3000);
        if (!slot_st.has_value()) {
            continue;
        }
        auto [fx, fy] = project_point(slot_st->H, c + 0.5, 5.5);
        tap(fx, fy, 220);
        cv::Mat pimg = shot();
        if (!is_detail_panel_open(pimg)) {
            continue;
        }
        auto [item_name, unused_facing] = read_unit_detail_panel(pimg);
        (void)unused_facing;
        close_detail_panel();

        if (item_name.empty()) {
            continue;
        }
        m_equip_slots[c] = EquipSlotInfo { c, "item", item_name, m_current_round };
        if (item_name.find("寻呼") != std::string::npos) {
            continue; // Handled by `try_use_jessica_pager`
        }

        const auto* item_info = find_item(item_name);
        const std::string cat = item_info
            ? (item_info->category.empty() ? classify_item_category(item_info->name, item_info->effect) : item_info->category)
            : classify_item_category(item_name, "");

        // If debuff equipment (`Es` / `An == "debuff"`), drag to `(1785, 900)` to destroy (`L9444-9448`)
        if (cat == "debuff" || (item_info && item_info->is_debuff)) {
            if (auto destroy_st = wait_stable_board(3000)) {
                auto [sx, sy] = project_point(destroy_st->H, c + 0.5, 5.5);
                log_info("销毁负面装备: " + item_name + " -> (1785, 900)");
                drag_board_unit(sx, sy, 1785, 900, false);
                sleep_ms(300);
                m_equip_slots.erase(c);
            }
            continue;
        }

        const UnitSlot* best_target = pick_equipment_target(cat, item_info);
        if (best_target) {
            if (auto drag_st = wait_stable_board(3000)) {
                auto [sx, sy] = project_point(drag_st->H, c + 0.5, 5.5);
                auto [tx, ty] = project_point(drag_st->H, best_target->c + 0.5, best_target->r + 0.5);
                log_info("穿戴装备 [" + cat + "]: " + item_name + " -> " + best_target->name);
                drag_board_unit(sx, sy, tx, ty, false);
                sleep_ms(300);
                m_unit_equipped_count[best_target->name]++;
                if (item_info && !item_info->granted_bond_name.empty()) {
                    m_unit_equipped_bond[best_target->name] = item_info->granted_bond_name;
                }
                wait_stable_board(3000);
            }
        }
    }
}

bool SPAFarmTaskPlugin::try_use_jessica_pager(const CameraDetectResult& cam)
{
    // Exact 1:1 port of `Ur` + `Dr` in service.pretty.mjs lines 9522-9575 (including `waitStableBoard` L9525, L9540, L9550, L9551, L9560, L9568)
    (void)cam;
    if (m_current_strategy.find("杰西卡") == std::string::npos && m_pager_used_round == m_current_round) {
        return false;
    }
    auto st = wait_stable_board(4000);
    if (!st.has_value()) {
        log_info("寻呼模块：棋盘未稳定，本回合不使用");
        return false;
    }

    int pager_col = -1;
    for (const auto& [c, eq] : m_equip_slots) {
        if (eq.name.find("寻呼") != std::string::npos) {
            pager_col = c;
            break;
        }
    }
    if (pager_col < 0) {
        return false;
    }

    const UnitSlot* target_unit = nullptr;
    for (const auto& u : m_units) {
        if (u.area == "board" && !u.name.empty() && !u.auxiliary) {
            if (const auto* op = find_operator(u.name)) {
                for (const auto& cb : m_active_core_bonds) {
                    if (std::find(op->bond_names.begin(), op->bond_names.end(), cb) != op->bond_names.end()) {
                        target_unit = &u;
                        break;
                    }
                }
            }
            if (target_unit) {
                break;
            }
        }
    }
    if (!target_unit) {
        return false;
    }

    auto drag_st = wait_stable_board(4000);
    if (!drag_st.has_value()) {
        close_detail_panel();
        drag_st = wait_stable_board(4000);
    }
    if (!drag_st.has_value()) {
        return false;
    }

    auto [fx, fy] = project_point(drag_st->H, pager_col + 0.5, 5.5);
    auto [tx, ty] = project_point(drag_st->H, target_unit->c + 0.5, target_unit->r + 0.5);
    log_info("使用杰西卡寻呼模块 -> " + target_unit->name);
    drag_board_unit(fx, fy, tx, ty, false);
    sleep_ms(400);
    wait_stable_board(3000);
    m_pager_used_round = m_current_round;
    m_equip_slots.erase(pager_col);
    return true;
}

bool SPAFarmTaskPlugin::try_use_mob_skill(const CameraDetectResult& cam, int gold)
{
    // Exact 1:1 port of `ut` (Mob `[1845, 350]` drag to board unit to elite for 1 gold) in service.pretty.mjs lines 9016-9056 (with `waitStableBoard(4e3)` L9021)
    (void)cam;
    if (m_mob_used_this_round || gold < 1 || m_current_strategy.find("莫布") == std::string::npos) {
        return false;
    }
    const UnitSlot* best_target = nullptr;
    int best_tier = 0;
    for (const auto& u : m_units) {
        if (u.area == "board" && !u.elite && !u.auxiliary && !u.name.empty()) {
            if (const auto* op = find_operator(u.name)) {
                if (op->tier >= best_tier) {
                    best_tier = op->tier;
                    best_target = &u;
                }
            }
        }
    }
    if (!best_target) {
        return false;
    }
    auto st = wait_stable_board(4000);
    if (!st.has_value()) {
        return false;
    }
    auto [tx, ty] = project_point(st->H, best_target->c + 0.5, best_target->r + 0.5);
    log_info("使用大嘴莫布策略升精 -> " + best_target->name);
    drag_board_unit(1845, 350, tx, ty, false);
    sleep_ms(static_cast<int>(std::lround(700.0 * m_step_scale)));
    close_detail_panel();
    m_mob_used_this_round = true;
    return true;
}

int SPAFarmTaskPlugin::deploy_summons(const CameraDetectResult& cam)
{
    // Exact 1:1 port of `Dt` in service.pretty.mjs lines 9130-9195 (with `waitStableBoard(4e3)` L9147 and `waitStableBoard(3e3)` L9170)
    (void)cam;
    std::vector<UnitSlot> bench_summons;
    for (const auto& u : m_units) {
        if (u.area == "bench" && u.auxiliary && !u.item) {
            bench_summons.push_back(u);
        }
    }
    if (bench_summons.empty()) {
        return 0;
    }
    auto st = wait_stable_board(4000);
    if (!st.has_value()) {
        return 0;
    }
    CameraDetectResult cur_cam { st->camera, st->raw_H, st->fit };
    StageTrafficMap traffic = build_stage_traffic(m_stage_id);
    std::set<std::pair<int, int>> occupied;
    for (const auto& u : m_units) {
        if (u.area == "board") {
            occupied.emplace(u.r, u.c);
        }
    }

    int deployed = 0;
    for (const auto& s : bench_summons) {
        for (int r = 1; r <= 4 && r < traffic.rows; ++r) {
            bool placed = false;
            for (int c = 0; c < traffic.cols; ++c) {
                char ch = traffic.grid[r][c];
                if ((ch == 'a' || ch == 'b') && occupied.count({ r, c }) == 0) {
                    PlacementMove mv { "deploy", s.name.empty() ? "召唤物" : s.name, "bench", s.r, s.c, "board", r, c, "LEFT" };
                    if (execute_placement_move(mv, cur_cam)) {
                        occupied.emplace(r, c);
                        ++deployed;
                        placed = true;
                        if (auto after_st = wait_stable_board(3000)) {
                            cur_cam = CameraDetectResult { after_st->camera, after_st->raw_H, after_st->fit };
                        }
                        break;
                    }
                }
            }
            if (placed) {
                break;
            }
        }
    }
    return deployed;
}

void SPAFarmTaskPlugin::run_prep_phase(const cv::Mat& img, const CameraDetectResult& cam)
{
    // Exact 1:1 port of `je(I)` in service.pretty.mjs lines 9984-10251
    m_mob_used_this_round = false;

    // Ensure shop is expanded during shopping
    cv::Mat cur_img = img;
    CameraDetectResult cur_cam = cam;
    if (cur_cam.camera == "collapsed") {
        auto full_ocr = ocr(cur_img);
        if (auto exp_btn = find_ocr(full_ocr, { "展开" })) {
            tap(exp_btn->cx, exp_btn->cy, 450);
            cur_img = shot();
            cur_cam = detect_camera(cur_img, goal_only_allowed());
        }
    }

    refresh_board_snapshot(cur_img, cur_cam, true);

    for (int iter = 0; iter < 14 && !need_exit(); ++iter) {
        cur_img = shot();
        auto full_items = ocr(cur_img);
        ShopHeaderState hdr = read_shop_header(cur_img, full_items);
        if (hdr.level.has_value()) {
            m_current_shop_level = *hdr.level;
        }
        if (hdr.gold.has_value()) {
            m_current_gold = *hdr.gold;
        }
        if (hdr.upgrade_btn.has_value()) {
            m_upgrade_x_Ref = hdr.upgrade_btn->first;
        }

        m_shop = read_shop_cards(cur_img);
        auto steps = plan_shop_Steps(hdr, m_shop);
        if (steps.empty() || steps.front().action == "done") {
            break;
        }

        bool acted = false;
        for (const auto& step : steps) {
            if (need_exit()) {
                return;
            }
            if (step.action == "done") {
                break;
            }
            if (step.action == "upgrade") {
                log_info("商店动作: 升级商店 -> " + step.target + " (费用:" + std::to_string(step.cost) + ")");
                if (upgrade_shop(hdr)) {
                    acted = true;
                }
                break;
            }
            if (step.action == "swap") {
                log_info("商店动作: 出售 " + step.sell_name + " 换购 " + step.target);
                if (sell_unit_slot(step.sell_slot, cur_cam)) {
                    buy_shop_card(step.x, step.target, m_upgrade_x_Ref.value_or(0));
                    acted = true;
                }
                break;
            }
            if (step.action == "buy") {
                log_info("商店动作: 购买 " + step.target + " (" + step.reason + ")");
                if (buy_shop_card(step.x, step.target, m_upgrade_x_Ref.value_or(0))) {
                    acted = true;
                    // Immediately refresh snapshot after purchase
                    cur_img = shot();
                    refresh_board_snapshot(cur_img, cur_cam, false);
                }
            }
            else if (step.action == "refresh") {
                log_info("商店动作: 刷新商店");
                if (hdr.refresh_btn.has_value()) {
                    tap(hdr.refresh_btn->first, hdr.refresh_btn->second, 450);
                }
                else {
                    tap(1786, 686, 450);
                }
                acted = true;
                break;
            }
        }
        if (!acted) {
            break;
        }
    }

    // Collapse shop if needed to get clear view of board/bench for unit inspection & placement
    cur_img = shot();
    cur_cam = detect_camera(cur_img, goal_only_allowed());
    log_info(std::string(cur_cam.camera.empty() ? "校准失败（跳过部署/装备/召唤）: " : "校准: ") + cur_cam.detail);
    if (!cur_cam.camera.empty()) {
        refresh_board_snapshot(cur_img, cur_cam, false);
        inspect_unknown_units(cur_cam, true);
        scan_and_distribute_equipment(cur_img, cur_cam);
        if (try_use_jessica_pager(cur_cam)) {
            cur_img = shot();
            m_shop = read_shop_cards(cur_img);
            for (const auto& card : m_shop) {
                if (card.free_card && !card.name.empty()) {
                    buy_shop_card(card.x, card.name, m_upgrade_x_Ref.value_or(0));
                }
            }
            cur_img = shot();
            refresh_board_snapshot(cur_img, cur_cam, false);
        }
        run_auto_place(cur_cam, false);
        try_use_mob_skill(cur_cam, m_current_gold);
        deploy_summons(cur_cam);
    }

    // 收尾前重读一次商店头：升级/购买后等级与资金已经变了，
    // 沿用循环里最后一次成功的旧值会让「整备完成」日志整体滞后一档。
    {
        cv::Mat tail_img = shot();
        if (!tail_img.empty()) {
            auto tail_items = ocr(tail_img);
            ShopHeaderState tail_hdr = read_shop_header(tail_img, tail_items);
            if (tail_hdr.level.has_value()) {
                m_current_shop_level = *tail_hdr.level;
            }
            if (tail_hdr.gold.has_value()) {
                m_current_gold = *tail_hdr.gold;
            }
        }
    }

    // Click Ready (`准备就绪` `[1780, 170]`)
    log_info(
        "回合 " + std::to_string(std::max(1, m_current_round))
        + " 整备完成 (商店Lv" + std::to_string(m_current_shop_level)
        + " 资金:" + std::to_string(m_current_gold) + ") -> 点击准备就绪");
    tap(1780, 170, 800);
    m_last_prep_round = m_current_round;
    cv::Mat post_ready = shot();
    if (!post_ready.empty()) {
        auto pr_items = ocr(post_ready);
        if (has_ocr(pr_items, { "是否确认准备", "确认准备", "资金将在" })) {
            if (auto no_prompt = find_ocr(pr_items, { "本局不再提示", "不再提示" })) {
                tap(no_prompt->x - 20, no_prompt->y + no_prompt->h * 0.5, 250);
            }
            else {
                tap(860, 602, 250);
            }
            if (auto hit = find_ocr_exact(pr_items, { "确认", "确定" })) {
                tap(hit->cx, hit->cy, 1000);
            }
            else {
                tap(1410, 728, 1000);
            }
        }
    }
}

// ============================================================================
// Screen Classifier (`bi`) & Strategy / Squad / Popup Handlers (`Ot`, `Ft`, `Or`, `Vn`, `K`, `ce`, `ln`)
// ============================================================================

SPAFarmTaskPlugin::ScreenState SPAFarmTaskPlugin::classify_screen(
    const cv::Mat& img,
    const std::vector<OcrItem>& items) const
{
    // Exact 1:1 port of `bi(a)` in service.pretty.mjs lines 4727-4805
    if (has_ocr(items, { "与神经网络连接中断", "连接已断开", "重新连接" })) {
        return ScreenState::Reconnect;
    }
    if (has_ocr(items, { "暂时离开模拟", "放弃本次模拟", "确认放弃" }) && has_ocr(items, { "确认", "放弃", "确定", "取消" })) {
        return ScreenState::Abandon;
    }
    if ((has_ocr(items, { "是否确认准备", "确认准备", "资金将在" }))
        && (has_ocr(items, { "剩余资金", "未使用的", "指令", "本局不再提示" }) || has_ocr_exact(items, { "确认", "确定" }))) {
        return ScreenState::Confirm;
    }
    if (has_ocr(items, { "装备已满，请选择", "销毁并替换", "穿戴装备已满" })) {
        return ScreenState::EquipFull;
    }

    // Three-choice overlay (`选择干员|选择战略|选择补给|选择支援|选择奖励` or >= 2 `选择` buttons in y: 550..920)
    if (!has_ocr(items, { "选择策略", "确认本局信息" })) {
        if (has_ocr(items, { "选择干员", "选择战略", "选择补给", "选择支援", "选择奖励", "选择一项策略" })) {
            return ScreenState::ThreeChoice;
        }
        int choose_btns = 0;
        for (const auto& it : items) {
            if ((it.text == "选择" || it.text == "选取") && it.cy > 550 && it.cy < 920) {
                ++choose_btns;
            }
        }
        if (choose_btns >= 2) {
            return ScreenState::ThreeChoice;
        }
    }

    if (has_ocr(items, { "战术决策", "抉择" }) && has_ocr(items, { "确认选择", "确认" })) {
        return ScreenState::Decision;
    }
    if (has_ocr(items, { "点击任意位置继续", "点击屏幕继续", "点击空白处关闭", "获得奖励" })) {
        return ScreenState::Reward;
    }
    if (has_ocr(items, { "返回主页" })
        || (has_ocr(items, { "模拟完成", "模拟中断", "演习结束", "战斗结算" }) && has_ocr(items, { "返回", "退出", "统计" }))) {
        return ScreenState::Result;
    }
    if (has_ocr(items, { "确定自选编队", "自选编队" }) && !has_ocr(items, { "选择策略" })) {
        return ScreenState::Squad;
    }
    if (has_ocr(items, { "确认本局信息" })
        || (has_ocr(items, { "核心盟约", "非核心盟约", "敌方领袖", "特训敌人" }) && !has_ocr(items, { "选择策略" }))) {
        return ScreenState::Info;
    }
    if (has_ocr(items, { "选择策略" })
        || (has_ocr_exact(items, { "确认选择" }) && has_ocr(items, { "固定编队", "自选编队", "已经通过", "已通过模拟", "通过次数" }))) {
        return ScreenState::Strategy;
    }
    if (has_ocr(items, { "请选择模拟难度", "选择模拟难度" })
        || (has_ocr(items, { "个人模拟" }) && has_ocr(items, { "极速模拟", "标准模拟" }))
        || (has_ocr(items, { "开始模拟" }) && has_ocr(items, { "独立模拟", "极速模拟" }) && !has_ocr(items, { "回合" }))) {
        return ScreenState::Mode;
    }
    if (find_ocr_in_roi(items, { "断线重连" }, 1100, 720, 1920, 1080).has_value()) {
        return ScreenState::Reconnect;
    }
    if (has_ocr(items, { "开始模拟" })
        || (has_ocr(items, { "极速模拟" }) && has_ocr(items, { "独立模拟" }))) {
        return ScreenState::Lobby;
    }
    if (has_ocr(items, { "点击取消", "拖动以选择朝向" }) && has_ocr(items, { "准备就绪" })) {
        return ScreenState::Facing;
    }

    if (!img.empty() && detect_facing_white_box(img).has_value()) {
        return ScreenState::Facing;
    }

    if (has_ocr(items, { "准备就绪" })
        && (has_ocr(items, { "刷新", "收起", "展开" }) || has_ocr_exact(items, { "升级", "已满级", "满级" }))) {
        return ScreenState::Shop;
    }

    auto cam = detect_camera(img, goal_only_allowed());
    if (!cam.camera.empty()) {
        auto [rnd, is_prep] = read_round_and_prep(img, items);
        (void)rnd;
        return is_prep ? ScreenState::Shop : ScreenState::Battle;
    }

    if (has_ocr(items, { "回合", "COUNTDOWN" })) {
        return ScreenState::Battle;
    }
    return ScreenState::Unknown;
}

cv::Mat SPAFarmTaskPlugin::extract_strategy_icon(const cv::Mat& img, int gx, int gy)
{
    // Exact 1:1 port of `Ft` / `Or` in service.pretty.mjs lines 4891-4906 (80x80 grayscale icon patch)
    int x0 = std::clamp(gx - 40, 0, std::max(0, img.cols - 80));
    int y0 = std::clamp(gy - 40, 0, std::max(0, img.rows - 80));
    cv::Mat patch = img(cv::Rect(x0, y0, 80, 80));
    cv::Mat gray;
    cv::cvtColor(patch, gray, cv::COLOR_BGR2GRAY);
    return gray;
}

double SPAFarmTaskPlugin::icon_diff(const cv::Mat& a, const cv::Mat& b)
{
    if (a.empty() || b.empty() || a.size() != b.size()) {
        return 255.0;
    }
    double s = 0.0;
    const int n = a.rows * a.cols;
    for (int y = 0; y < a.rows; ++y) {
        const uint8_t* ra = a.ptr<uint8_t>(y);
        const uint8_t* rb = b.ptr<uint8_t>(y);
        for (int x = 0; x < a.cols; ++x) {
            s += std::abs(static_cast<int>(ra[x]) - static_cast<int>(rb[x]));
        }
    }
    return n > 0 ? (s / static_cast<double>(n)) : 255.0;
}

bool SPAFarmTaskPlugin::has_strategy_selected_border(const cv::Mat& img, int gx, int gy)
{
    const int strips[4][4] = {
        { gx - 88, gy - 88, gx + 88, gy - 72 },
        { gx - 88, gy + 72, gx + 88, gy + 88 },
        { gx - 88, gy - 72, gx - 72, gy + 72 },
        { gx + 72, gy - 72, gx + 88, gy + 72 }
    };
    int orange = 0, total = 0;
    for (const auto& st : strips) {
        int x0 = std::clamp(st[0], 0, img.cols);
        int y0 = std::clamp(st[1], 0, img.rows);
        int x1 = std::clamp(st[2], 0, img.cols);
        int y1 = std::clamp(st[3], 0, img.rows);
        for (int y = y0; y < y1; ++y) {
            const auto* row = img.ptr<cv::Vec3b>(y);
            for (int x = x0; x < x1; ++x) {
                int B = row[x][0], G = row[x][1], R = row[x][2];
                ++total;
                if (R > 180 && G > 50 && G < 160 && B < 60 && (R - G) > 40) {
                    ++orange;
                }
            }
        }
    }
    return total > 0 && (static_cast<double>(orange) / total) > 0.25;
}

int SPAFarmTaskPlugin::detect_strategy_badge_tier(const cv::Mat& img, int gx, int gy)
{
    // Exact 1:1 port of `Vn(png, Ie, H, 190)` in service.pretty.mjs lines 8756-8769
    int x0 = std::clamp(gx + 30, 0, img.cols);
    int y0 = std::clamp(gy - 88, 0, img.rows);
    int x1 = std::clamp(gx + 88, 0, img.cols);
    int y1 = std::clamp(gy - 30, 0, img.rows);
    int cyan = 0, gold = 0;
    for (int y = y0; y < y1; ++y) {
        const auto* row = img.ptr<cv::Vec3b>(y);
        for (int x = x0; x < x1; ++x) {
            int B = row[x][0], G = row[x][1], R = row[x][2];
            if (B > 130 && G > 120 && (B - R) > 25) {
                ++cyan;
            }
            else if (R > 150 && G > 95 && G < R * 0.94 && B < G * 0.62) {
                ++gold;
            }
        }
    }
    if (cyan >= 60) {
        return 6;
    }
    if (gold >= 80) {
        return 3;
    }
    return 0;
}

bool SPAFarmTaskPlugin::handle_strategy_screen(const cv::Mat& img)
{
    // Exact 1:1 port of `Ot(I)` + `ye` in service.pretty.mjs lines 8711-8853 & 10359-10368
    static constexpr int kGridX[4] = { 696, 913, 1130, 1347 };
    static constexpr int kGridY[4] = { 370, 588, 806, 1005 };

    cv::Mat cur_img = img;
    auto init_items = ocr(cur_img);

    // Step 1: If match info (`本局信息`) has not been inspected yet on strategy page, click `本局信息` (`ye` lines 10359-10368)
    if (!m_match_info_read) {
        if (auto info_btn = find_ocr(init_items, { "本局信息" })) {
            log_info("策略选择前先点击「本局信息」读取本局敌方领袖与激活核心盟约");
            tap(info_btn->cx, info_btn->cy, 900);
            cv::Mat info_img = shot();
            auto info_items = ocr(info_img);
            handle_info_screen(info_img, info_items);
            cur_img = shot();
        }
    }

    for (int page = 0; page < 6 && !need_exit(); ++page) {
        const int max_rows = (page == 5) ? 4 : 3;
        for (int row_idx = 0; row_idx < max_rows; ++row_idx) {
            int gy = kGridY[row_idx];
            for (int col_idx = 0; col_idx < 4; ++col_idx) {
                if (need_exit()) {
                    return false;
                }
                int gx = kGridX[col_idx];

                // Check visual medal badge first (`Vn`)
                int badge_tier = detect_strategy_badge_tier(cur_img, gx, gy);
                if (badge_tier >= m_target && m_goal == "medal") {
                    continue;
                }

                // Check icon cache (`Or` with threshold `< 10`)
                cv::Mat icon = extract_strategy_icon(cur_img, gx, gy);
                std::string cached_name;
                for (const auto& seen : m_seen_strategy_icons) {
                    if (icon_diff(icon, seen.icon) < 10.0) {
                        cached_name = seen.name;
                        break;
                    }
                }
                if (!cached_name.empty() && m_done_strategies.count(cached_name) > 0 && m_goal == "medal") {
                    continue;
                }

                // Click strategy cell if not already selected
                if (!has_strategy_selected_border(cur_img, gx, gy)) {
                    tap(gx, gy, 350);
                    cur_img = shot();
                }

                // Read right detail panel `[1430, 100, 470, 880]`
                auto panel_ocr = ocr(cur_img, std::array<int, 4> { 1430, 100, 470, 880 });
                std::string panel_text;
                std::string strat_name;
                int passed = 0;

                for (const auto& it : panel_ocr) {
                    panel_text += it.text;
                    for (std::string_view kw : { std::string_view("已通过模拟"), std::string_view("通过次数") }) {
                        auto pos = it.text.find(kw);
                        if (pos != std::string::npos) {
                            std::string d = extract_digits_mapped(it.text.substr(pos));
                            if (!d.empty()) {
                                passed = std::stoi(d);
                            }
                        }
                    }
                }
                if (passed == 0) {
                    for (std::string_view kw : { std::string_view("已通过模拟"), std::string_view("通过次数") }) {
                        auto pos = panel_text.find(kw);
                        if (pos != std::string::npos) {
                            std::string d = extract_digits_mapped(panel_text.substr(pos, 24));
                            if (!d.empty()) {
                                passed = std::stoi(d);
                                break;
                            }
                        }
                    }
                }

                auto sm = match_dict(panel_text, m_strategy_dict);
                if (!sm.empty()) {
                    strat_name = sm.front().name;
                }
                else if (!panel_ocr.empty()) {
                    strat_name = panel_ocr.front().text;
                }

                if (!strat_name.empty()) {
                    m_strategy_clears[strat_name] = std::max(m_strategy_clears[strat_name], passed);
                    bool already_cached = false;
                    for (const auto& s : m_seen_strategy_icons) {
                        if (s.name == strat_name) {
                            already_cached = true;
                            break;
                        }
                    }
                    if (!already_cached) {
                        m_seen_strategy_icons.push_back(SeenStrategyIcon { strat_name, icon });
                    }
                }

                if (m_goal != "medal") {
                    // For task-1..task-7 goals, prefer Jessica (`杰西卡`) strategy (`pr` in service.pretty.mjs)
                    if (!strat_name.empty() && strat_name.find("杰西卡") == std::string::npos) {
                        continue;
                    }
                }
                else if (passed >= m_target || badge_tier >= m_target) {
                    if (!strat_name.empty()) {
                        m_done_strategies.insert(strat_name);
                    }
                    log_info("跳过已满章策略: " + strat_name + " (通过次数:" + std::to_string(std::max(passed, badge_tier)) + "/" + std::to_string(m_target) + ")");
                    continue;
                }

                // Found our target strategy!
                m_current_strategy = strat_name;
                m_current_strategy_id.clear();
                for (const auto& sr : m_strategies) {
                    if (sr.name == strat_name) {
                        m_current_strategy_id = sr.id;
                        break;
                    }
                }
                log_info(
                    "选定策略: " + (m_current_strategy.empty() ? std::string("当前策略") : m_current_strategy)
                    + " (当前通关:" + std::to_string(passed) + "/" + std::to_string(m_target)
                    + "，总体策略完成:" + std::to_string(m_done_strategies.size()) + "/" + std::to_string(total_available_strategies()) + ")");
                if (auto conf_btn = find_ocr_exact(panel_ocr, { "确认选择" })) {
                    tap(conf_btn->cx, conf_btn->cy, 1200);
                }
                else {
                    tap(1680, 990, 1200);
                }
                return true;
            }
        }

        // Scroll strategy grid down (`ut(1020, 820, 1020, 360, 700)`)
        log_info("当前页策略均已达标，向下滑动策略列表 (" + std::to_string(page + 1) + "/6)");
        swipe(1020, 820, 1020, 360, 700, 800);
        cur_img = shot();
    }

    return false;
}

void SPAFarmTaskPlugin::handle_squad_screen(const cv::Mat& img, const std::vector<OcrItem>& items)
{
    // Exact 1:1 port of `squad` handler in service.pretty.mjs lines 10420-10423
    (void)img;
    std::vector<std::pair<double, double>> banned;
    for (const auto& it : items) {
        if (it.text.find("禁用") != std::string::npos) {
            banned.emplace_back(it.cx, it.cy);
        }
    }
    std::unordered_map<std::string, int> empty_owned;
    std::unordered_set<std::string> active_bonds(m_active_core_bonds.begin(), m_active_core_bonds.end());

    for (auto [lo_y, hi_y] : { std::pair<int, int> { 380, 520 }, std::pair<int, int> { 760, 900 } }) {
        struct CandOp
        {
            double cx = 0.0;
            double cy = 0.0;
            double score = 0.0;
        };
        std::vector<CandOp> row_cands;
        for (const auto& it : items) {
            if (it.cy < lo_y || it.cy > hi_y || it.text.find("禁用") != std::string::npos) {
                continue;
            }
            bool is_banned = false;
            for (auto [bx, by] : banned) {
                if (std::abs(it.cx - bx) < 90.0 && (by - it.cy) > 0.0 && (by - it.cy) < 85.0) {
                    is_banned = true;
                    break;
                }
            }
            if (is_banned) {
                continue;
            }
            auto cm = match_dict(it.text, m_chess_dict);
            double sc = 10.0;
            if (!cm.empty()) {
                if (const auto* op = find_operator(cm.front().name)) {
                    sc = score_operator_for_plan(*op, empty_owned, active_bonds);
                }
            }
            row_cands.push_back(CandOp { it.cx, it.cy, sc });
        }
        std::sort(row_cands.begin(), row_cands.end(), [](const CandOp& a, const CandOp& b) {
            return a.score > b.score;
        });
        for (size_t i = 0; i < std::min<size_t>(2, row_cands.size()); ++i) {
            tap(row_cands[i].cx, row_cands[i].cy - 170.0, 380);
        }
    }

    // CRITICAL FIX: Never click `"确定自选编队"` (which is the top header title)!
    // Click `"准备就绪"` or bottom-right button ROI `[1300, 820, 1920, 1080]`, fallback `(1686, 986)`.
    log_info("界面: 确定自选编队 -> 完成干员勾选并点击右下角准备就绪");
    if (auto hit = find_ocr(items, { "准备就绪", "开始模拟" })) {
        tap(hit->cx, hit->cy, 1500);
    }
    else if (auto roi_hit = find_ocr_in_roi(items, { "确认选择", "确认", "确定" }, 1300, 820, 1920, 1080)) {
        tap(roi_hit->cx, roi_hit->cy, 1500);
    }
    else {
        tap(1686, 986, 1500);
    }
}

void SPAFarmTaskPlugin::handle_info_screen(const cv::Mat& img, const std::vector<OcrItem>& items)
{
    // Exact 1:1 port of `ln` (lines 9627-9650) & `info` screen handler (lines 10340-10358) in service.pretty.mjs:
    // Inspects active core bonds on `确认本局信息` / `本局信息` screen and switches core bonds if `精准` or `奥术` is banned!
    m_match_info_read = true;
    if (auto info = read_info_screen(img)) {
        std::vector<std::string> active_cores;
        for (const auto& b : info->bonds) {
            if (b.core && b.active) {
                active_cores.push_back(b.name);
            }
        }
        if (m_goal == "medal" && !active_cores.empty()) {
            std::vector<std::string> chosen;
            for (std::string_view pref : { "精准", "奥术", "迅捷", "强攻", "守望", "坚城" }) {
                if (std::find(active_cores.begin(), active_cores.end(), pref) != active_cores.end()) {
                    chosen.emplace_back(pref);
                    if (chosen.size() >= 2) {
                        break;
                    }
                }
            }
            if (!chosen.empty()) {
                m_active_core_bonds = chosen;
            }
        }
        std::string bond_str;
        for (const auto& cb : m_active_core_bonds) {
            if (!bond_str.empty()) {
                bond_str += "/";
            }
            bond_str += cb;
        }
        if (!info->boss.empty()) {
            m_current_boss = info->boss;
        }
        log_info(
            "界面: 确认本局信息 (主抓核心盟约: " + bond_str
            + (info->boss.empty() ? "" : (" | 敌方领袖: " + info->boss)) + ")");
    }
    else {
        log_info("界面: 确认本局信息 -> 点击右下角准备就绪");
    }

    // CRITICAL FIX: `"确认本局信息"` is the top header title! Never click `"确认本局信息"`.
    // In service.pretty.mjs lines 10340-10358:
    // - If `准备就绪` button is visible, click `准备就绪` (bottom-right `(1686, 986)`).
    // - If `已就绪` is visible, wait 500ms.
    // - If `确认本局信息` title is present (pre-match confirmation screen), click `(1686, 986)`.
    // - Otherwise (opened via `本局信息` overlay), close via top-right `(1805, 65)`.
    if (auto ready_btn = find_ocr(items, { "准备就绪" })) {
        tap(ready_btn->cx, ready_btn->cy, 1500);
    }
    else if (has_ocr(items, { "已就绪" })) {
        sleep_ms(500);
    }
    else if (has_ocr(items, { "确认本局信息" })) {
        tap(1686, 986, 1500);
    }
    else {
        tap(1805, 65, 700);
    }
}

void SPAFarmTaskPlugin::handle_three_choice_screen(const cv::Mat& img, const std::vector<OcrItem>& items)
{
    // Exact 1:1 port of `K(I, k)` in service.pretty.mjs lines 10252-10335
    (void)img;
    double best_x = 960.0;
    double best_score = -1.0;

    std::unordered_map<std::string, int> owned;
    for (const auto& u : m_units) {
        if (!u.name.empty()) {
            owned[u.name]++;
        }
    }
    std::unordered_set<std::string> active_bonds(m_active_core_bonds.begin(), m_active_core_bonds.end());

    // Find all `选择` buttons or evaluate card columns (560, 960, 1360)
    std::vector<OcrItem> btns;
    for (const auto& it : items) {
        if ((it.text == "选择" || it.text == "选取") && it.cy > 500 && it.cy < 950) {
            btns.push_back(it);
        }
    }

    for (const auto& it : items) {
        auto cm = match_dict(it.text, m_chess_dict);
        if (!cm.empty()) {
            if (const auto* op = find_operator(cm.front().name)) {
                double sc = score_operator_for_plan(*op, owned, active_bonds);
                if (sc > best_score) {
                    best_score = sc;
                    best_x = it.cx;
                }
            }
        }
    }

    if (!btns.empty()) {
        const OcrItem* chosen_btn = &btns.front();
        double min_dx = 1e9;
        for (const auto& b : btns) {
            double dx = std::abs(b.cx - best_x);
            if (dx < min_dx) {
                min_dx = dx;
                chosen_btn = &b;
            }
        }
        log_info("界面: 三选一奖励 -> 点击选择 (" + std::to_string(static_cast<int>(chosen_btn->cx)) + "," + std::to_string(static_cast<int>(chosen_btn->cy)) + ")");
        tap(chosen_btn->cx, chosen_btn->cy, 600);
    }
    else {
        log_info("界面: 三选一奖励 -> 选择中间项并确认");
        tap(960, 520, 400);
    }

    if (auto conf = find_ocr_in_roi(items, { "确认", "确定" }, 500, 700, 1600, 1060)) {
        tap(conf->cx, conf->cy, 800);
    }
}

void SPAFarmTaskPlugin::handle_decision_screen(const cv::Mat& img, const std::vector<OcrItem>& items)
{
    // Exact 1:1 port of `ce(I, k)` in service.pretty.mjs lines 10337-10378
    (void)img;
    log_info("界面: 战术决策 -> 选择第一项并确认选择");
    tap(450, 300, 400);
    if (auto conf = find_ocr_exact(items, { "确认选择", "确认" })) {
        tap(conf->cx, conf->cy, 1000);
    }
    else {
        tap(320, 480, 1000);
    }
}

// ============================================================================
// Main Run Loop (`_run`, 1:1 with `ye` in service.pretty.mjs lines 10256-10530)
// ============================================================================

bool SPAFarmTaskPlugin::_run()
{
    LogTraceFunction;
    load_all_data();
    reset_match_state();

    log_info(
        "启动卫戍协议自动刷章任务 | 模式:" + m_goal
        + " | 配置:" + m_profile
        + " | 目标通关次数:" + std::to_string(m_target)
        + " | 计划通关上限:" + std::to_string(m_runs) + "局");

    double last_garrison_seen = now_sec();
    double last_progress_time = now_sec();
    int unknown_streak = 0;
    std::string stop_reason = "已达到设定通关局数上限";
    bool finished_ok = true;

    while (m_runs_completed < m_runs && !need_exit()) {
        if (m_attempts > 0 && m_attempts_done >= m_attempts) {
            stop_reason = "已达到设定最大尝试场次上限 (" + std::to_string(m_attempts) + " 场)";
            break;
        }

        if (now_sec() - last_progress_time > kStallTimeoutSec) {
            stop_reason = "超过 10 分钟无实质进展，触发防卡死保护停止";
            finished_ok = false;
            break;
        }

        cv::Mat img = shot();
        if (img.empty()) {
            sleep_sec(1.0);
            continue;
        }

        auto items = ocr(img);

        // Global overlay checks matching `handleScreen` in service.pretty.mjs (lines 10256-10319)
        if (has_ocr(items, { "数据已更新", "即将进行同步" })) {
            tap(960, 738, 3000);
            continue;
        }
        if (has_ocr(items, { "今日配给" })) {
            tap(960, 960, 1500);
            continue;
        }
        if (has_ocr(items, { "签到", "许愿墙", "活动公告", "系统公告" })
            && !has_ocr(items, { "个人模拟", "休息一下" })) {
            tap(1822, 104, 500);
            continue;
        }
        if (has_ocr_exact(items, { "终端" }) && has_ocr(items, { "理智" })) {
            tap(1750, 215, 4000);
            continue;
        }

        ScreenState state = classify_screen(img, items);

        if (state != ScreenState::Unknown) {
            last_garrison_seen = now_sec();
            unknown_streak = 0;
        }
        else if (now_sec() - last_garrison_seen > kLeaveTimeoutSec) {
            stop_reason = "连续 30 秒未检测到卫戍协议界面，自动停止";
            finished_ok = false;
            break;
        }

        switch (state) {
        case ScreenState::Lobby: {
            log_info("界面: 卫戍协议主界面 -> 进入模拟");
            if (auto hit = find_ocr(items, { "开始模拟", "极速模拟" })) {
                tap(hit->cx, hit->cy, 2000);
            }
            else {
                tap(1280, 1000, 2000);
            }
            last_progress_time = now_sec();
            break;
        }
        case ScreenState::Mode: {
            log_info("界面: 选择模拟模式 -> 点击个人模拟/极速模拟");
            if (auto p_hit = find_ocr_exact(items, { "个人模拟" })) {
                tap(p_hit->cx, p_hit->cy, 600);
            }
            if (auto hit = find_ocr(items, { "开始模拟", "极速模拟" })) {
                tap(hit->cx, hit->cy, 2200);
            }
            else {
                tap(1700, 986, 2200);
            }
            reset_match_state();
            last_progress_time = now_sec();
            break;
        }
        case ScreenState::Reconnect: {
            // CRITICAL FIX: Never click dialog title `"断线重连"` at top of screen!
            // Check dialog lower button area `[400, 520, 1520, 920]` for `重新连接`/`确定`/`确认`,
            // or bottom-right lobby button area `[1250, 780, 1920, 1080]` for `断线重连`.
            log_info("界面: 检测到断线重连/连接中断 -> 点击重连按钮");
            if (auto dialog_btn = find_ocr_in_roi(items, { "重新连接", "确定", "确认" }, 400, 520, 1520, 920)) {
                tap(dialog_btn->cx, dialog_btn->cy, 3500);
            }
            else if (auto lobby_btn = find_ocr_in_roi(items, { "断线重连" }, 1250, 780, 1920, 1080)) {
                tap(lobby_btn->cx, lobby_btn->cy, 3500);
            }
            else {
                tap(1660, 970, 3500);
            }
            reset_match_state();
            last_progress_time = now_sec();
            break;
        }
        case ScreenState::Strategy: {
            last_progress_time = now_sec();
            if (!handle_strategy_screen(img)) {
                stop_reason = "全部可用策略均已达成 " + std::to_string(m_target) + " 次通关刻章目标";
                finished_ok = true;
                report_task_summary(stop_reason, finished_ok);
                return true;
            }
            break;
        }
        case ScreenState::Squad: {
            last_progress_time = now_sec();
            handle_squad_screen(img, items);
            break;
        }
        case ScreenState::Info: {
            last_progress_time = now_sec();
            handle_info_screen(img, items);
            break;
        }
        case ScreenState::Confirm: {
            // Replicates `confirm-ready` in service.pretty.mjs lines 10369-10372:
            // Find `本局不再提示` and tap `(B.x - 20, B.y + B.h / 2)`, then tap `/^确认$/`
            log_info("界面: 确认准备弹窗 -> 勾选本局不再提示并确认");
            if (auto no_prompt = find_ocr(items, { "本局不再提示", "不再提示" })) {
                tap(no_prompt->x - 20, no_prompt->y + no_prompt->h * 0.5, 250);
            }
            else {
                tap(860, 602, 250);
            }
            if (auto hit = find_ocr_exact(items, { "确认", "确定" })) {
                tap(hit->cx, hit->cy, 1000);
            }
            else if (auto roi_hit = find_ocr_in_roi(items, { "确认", "确定" }, 960, 580, 1650, 860)) {
                tap(roi_hit->cx, roi_hit->cy, 1000);
            }
            else {
                tap(1410, 728, 1000);
            }
            break;
        }
        case ScreenState::Abandon: {
            // Replicates `/暂时离开模拟/` in service.pretty.mjs lines 10257-10261: always click `取消`!
            log_info("界面: 暂时离开/放弃模拟弹窗 -> 点击取消继续对局");
            if (auto hit = find_ocr_exact(items, { "取消" })) {
                tap(hit->cx, hit->cy, 800);
            }
            else if (auto sub_hit = find_ocr(items, { "取消" })) {
                tap(sub_hit->cx, sub_hit->cy, 800);
            }
            else {
                tap(480, 727, 800);
            }
            break;
        }
        case ScreenState::EquipFull: {
            // Replicates `/装备已满，请选择|销毁并替换/` in service.pretty.mjs lines 10315-10318:
            // Click top-right close `(1846, 284)` to avoid destroying equipped items!
            log_info("界面: 装备已满弹窗 -> 关闭替换窗口保留原装备");
            if (has_ocr(items, { "装备已满，请选择", "销毁并替换" })) {
                tap(1846, 284, 800);
            }
            else if (auto hit = find_ocr_exact(items, { "确认", "确定" })) {
                tap(hit->cx, hit->cy, 600);
            }
            else {
                tap(1846, 284, 800);
            }
            break;
        }
        case ScreenState::ThreeChoice: {
            handle_three_choice_screen(img, items);
            last_progress_time = now_sec();
            break;
        }
        case ScreenState::Decision: {
            handle_decision_screen(img, items);
            last_progress_time = now_sec();
            break;
        }
        case ScreenState::Reward: {
            log_info("界面: 奖励/继续提示 -> 点击继续");
            tap(960, 920, 800);
            break;
        }
        case ScreenState::Detail: {
            close_detail_panel();
            break;
        }
        case ScreenState::Facing: {
            if (auto box = detect_facing_white_box(img)) {
                log_info("界面: 检测到未完成朝向框 -> 自动向左拖拽设朝向");
                drag_segments(box->first, box->second, box->first - 210.0, box->second, 6, 20, 80);
                sleep_ms(300);
            }
            else if (auto cancel = find_ocr(items, { "点击取消" })) {
                tap(cancel->cx, cancel->cy, 600);
            }
            break;
        }
        case ScreenState::Shop: {
            auto [rnd_opt, is_prep] = read_round_and_prep(img, items);
            (void)is_prep;
            if (rnd_opt.has_value()) {
                m_current_round = *rnd_opt;
            }
            else if (m_current_round <= 0) {
                m_current_round = 1;
            }
            auto cam = detect_camera(img, goal_only_allowed());
            if (cam.camera.empty()) {
                cam.camera = "expanded";
                cam.H = m_calib_expanded.base_H;
            }
            run_prep_phase(img, cam);
            last_progress_time = now_sec();
            break;
        }
        case ScreenState::Battle: {
            auto [rnd_opt, is_prep] = read_round_and_prep(img, items);
            if (rnd_opt.has_value() && *rnd_opt != m_current_round) {
                m_current_round = *rnd_opt;
                last_progress_time = now_sec();
            }
            if (is_prep) {
                auto cam = detect_camera(img, goal_only_allowed());
                if (!cam.camera.empty()) {
                    run_prep_phase(img, cam);
                    last_progress_time = now_sec();
                    break;
                }
            }
            sleep_sec(2.0);
            break;
        }
        case ScreenState::Result: {
            bool won = has_ocr(items, { "模拟完成" });
            report_match_finished(won, m_current_round);
            if (auto hit = find_ocr(items, { "返回主页", "返回", "退出" })) {
                tap(hit->cx, hit->cy, 3000);
            }
            else {
                tap(1650, 960, 3000);
            }
            reset_match_state();
            last_progress_time = now_sec();
            break;
        }
        case ScreenState::Unknown:
        default: {
            ++unknown_streak;
            std::string all_txt;
            for (const auto& it : items) {
                all_txt += it.text + " ";
            }
            // CRITICAL FIX: Never match substring `"完成"` (which matches `"模拟完成"` header)!
            if (auto hit = find_ocr(items, { "返回主页", "点击任意", "点击空白处" })) {
                tap(hit->cx, hit->cy, 1000);
            }
            else if (auto exact_hit = find_ocr_exact(items, { "继续", "确定", "确认" })) {
                tap(exact_hit->cx, exact_hit->cy, 1000);
            }
            else if (unknown_streak % 5 == 1) {
                log_info("未识别界面，OCR 片段: " + all_txt.substr(0, std::min<size_t>(120, all_txt.size())));
            }
            sleep_sec(1.5);
            break;
        }
        }
    }

    if (need_exit()) {
        stop_reason = "收到用户/上层停止指令";
    }
    report_task_summary(stop_reason, finished_ok);
    return finished_ok;
}

} // namespace asst
