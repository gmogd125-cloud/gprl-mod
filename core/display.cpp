#include "display.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gprl::display {

namespace {

std::string fixed2(double v) {
    if (!std::isfinite(v)) return "?";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

std::string signed2(double v) {
    if (!std::isfinite(v)) return "?";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%+.2f", v);
    // "-0.00" reads like a bug next to "+0.00"
    std::string s = buf;
    if (s == "-0.00") s = "+0.00";
    return s;
}

std::string fixed1(double v) {
    if (!std::isfinite(v)) return "?";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

}  // namespace

// ---- HUD history ----

std::string historyText(HistoryEntry const& e, DisplayParams const& p) {
    (void)p;
    std::string head = e.down ? "PRESS" : "REL";
    if (e.levelOnly) head = "bot " + head;
    std::string tail = e.suffix.empty() ? std::string() : " | " + e.suffix;
    if (!e.ok) return head + " dropped: " + (e.reason.empty() ? std::string("unknown") : e.reason) + tail;
    std::string edges = "[" + std::string(e.boundedEarly ? "" : "<") + signed2(e.earlyMs) + " " + signed2(e.lateMs) + (e.boundedLate ? "" : ">") + "]";
    if (e.miss) return head + " miss " + fixed2(e.widthMs) + " ms " + edges + tail;
    return head + " " + fixed2(e.widthMs) + " ms " + edges + tail;
}

Tone historyTone(HistoryEntry const& e) {
    if (!e.ok) return Tone::Dropped;
    if (e.miss) return Tone::Miss;
    return Tone::Measured;
}

int historyOpacity(size_t index, DisplayParams const& p) {
    long long o = static_cast<long long>(p.newestOpacity) - static_cast<long long>(index) * p.opacityStep;
    return static_cast<int>(std::clamp<long long>(o, p.minOpacity, 255));
}

std::vector<HistoryLine> historyLines(std::vector<HistoryEntry> const& newestFirst, DisplayParams const& p) {
    std::vector<HistoryLine> out;
    size_t n = std::min(newestFirst.size(), static_cast<size_t>(std::max(0, p.historyLines)));
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        HistoryLine l;
        l.text = historyText(newestFirst[i], p);
        l.tone = historyTone(newestFirst[i]);
        l.opacity = historyOpacity(i, p);
        out.push_back(std::move(l));
    }
    return out;
}

// ---- level analysis coverage ----

std::string asciiDash(std::string const& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        // U+2013 EN DASH = E2 80 93, U+2014 EM DASH = E2 80 94, U+2212 MINUS = E2 88 92
        if (c == 0xE2 && i + 2 < s.size()) {
            unsigned char b = static_cast<unsigned char>(s[i + 1]), d = static_cast<unsigned char>(s[i + 2]);
            if ((b == 0x80 && (d == 0x93 || d == 0x94)) || (b == 0x88 && d == 0x92)) out.push_back('-');
            i += 3;
            continue;
        }
        // any other multi-byte sequence: skip the lead byte and its continuation bytes
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    }
    return out;
}

bool parseLevelCoverage(json::Value const& v, LevelCoverage& out, std::string* err) {
    if (!v.isObject()) {
        if (err) *err = "level analysis response is not an object";
        return false;
    }
    auto const* cov = v.find("coverage");
    if (!cov || !cov->isObject()) {
        if (err) *err = "level analysis response has no coverage object";
        return false;
    }
    LevelCoverage c;
    c.levelId = v.getString("levelId");
    c.levelName = asciiDash(v.getString("levelName"));
    c.levelCounts = v.getBool("levelCounts", true);
    c.status = v.getString("status");
    double percent = cov->getNumber("percent", cov->getNumber("coveragePercent", 0.0));
    c.percent = std::isfinite(percent) ? std::clamp(percent, 0.0, 100.0) : 0.0;
    double display = cov->getNumber("displayPercent", std::floor(c.percent));
    c.displayPercent = std::isfinite(display) ? static_cast<int>(std::clamp(display, 0.0, 100.0)) : 0;
    c.complete = cov->getBool("complete", false);
    double observed = cov->getNumber("observedPercent", 0.0);
    c.observedPercent = std::isfinite(observed) ? std::clamp(observed, 0.0, 100.0) : 0.0;
    if (auto const* m = cov->find("missingDisplay"); m && m->isArray()) {
        for (auto const& item : m->asArray()) {
            if (item.isString()) c.missing.push_back(asciiDash(item.asString()));
        }
    }
    else if (auto const* ranges = cov->find("missing"); ranges && ranges->isArray()) {
        // older producers without missingDisplay: format the ranges ourselves
        for (auto const& r : ranges->asArray()) {
            if (!r.isObject()) continue;
            c.missing.push_back(fixed1(r.getNumber("fromPercent")) + "-" + fixed1(r.getNumber("toPercent")) + "%");
        }
    }
    auto const* analysis = v.find("analysis");
    c.hasAnalysis = analysis && analysis->isObject();
    out = std::move(c);
    return true;
}

std::string coverageLine(LevelCoverage const& c) {
    std::string line = "Level Analysis Coverage: " + std::to_string(c.displayPercent) + "%";
    if (c.complete) line += " / Complete";
    else if (c.missing.empty()) line += " / Missing: - 0.0-100.0%";
    else {
        line += " / Missing:";
        for (auto const& m : c.missing) line += " - " + m;
    }
    if (!c.levelCounts) line += " (not a rated demon: not analyzed)";
    return line;
}

// ---- which levels count ----

std::string levelCountsLine(bool levelCounts, std::string const& reason) {
    if (levelCounts) return {};
    // v0.8.4 (owner decision 2026-10-01): every level feeds calibration; the verdict only says
    // the level is not on the website's levels list
    std::string line = "Not a rated demon: not on the levels list (still feeds your calibration)";
    std::string r = asciiDash(reason);
    // the server's reason repeats the verdict ("...: not counted."); keep it only when it adds a fact
    if (!r.empty() && r != "Not a rated demon: not counted." && r != "Not a rated demon: not counted") line += " - " + r;
    return line;
}

// ---- why a session does not feed the rating ----

std::string unratedSessionNotice(std::string const& ratableReason) {
    std::string const r = asciiDash(ratableReason);
    // "mod menu without a state adapter: <name> (<id>)" (trust.ts classifyMods; reasons joined by "; ")
    static constexpr std::string_view kMenu = "mod menu without a state adapter: ";
    if (auto at = r.find(kMenu); at != std::string::npos) {
        size_t const from = at + kMenu.size();
        size_t end = r.find(" (", from);
        size_t const semi = r.find(';', from);
        if (end == std::string::npos || (semi != std::string::npos && semi < end)) end = semi;
        std::string name = r.substr(from, end == std::string::npos ? std::string::npos : end - from);
        if (name.size() > 40) name.resize(40);
        if (!name.empty())
            return "GPRL: this session is NOT rated. " + name + " is loaded and GPRL cannot see whether its cheats are on. Disable " + name +
                   " while you play to calibrate (the Eclipse menu works).";
    }
    static constexpr std::string_view kUnknown = "unknown gameplay-affecting mod ";
    if (auto at = r.find(kUnknown); at != std::string::npos) {
        size_t const from = at + kUnknown.size();
        size_t const end = r.find(';', from);
        std::string id = r.substr(from, end == std::string::npos ? std::string::npos : end - from);
        if (id.size() > 60) id.resize(60);
        if (!id.empty()) return "GPRL: this session is NOT rated: unknown gameplay mod " + id + " is loaded. Disable it to calibrate.";
    }
    if (r.empty()) return "GPRL: this session is NOT rated.";
    std::string text = "GPRL: this session is NOT rated: " + r;
    if (text.size() > 160) text.resize(160);
    return text;
}

// ---- level hints ----

std::string demonDifficultyName(int gdDemonDifficulty, bool isDemon) {
    if (!isDemon) return {};
    switch (gdDemonDifficulty) {
        case 3: return "easy";
        case 4: return "medium";
        case 5: return "insane";
        case 6: return "extreme";
        default: return "hard";   // 0 = GD's default demon face (hard)
    }
}

std::string levelNameHint(std::string const& name, DisplayParams const& p) {
    std::string out;
    for (char ch : name) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 0x20 && c < 0x7F) out.push_back(ch);
    }
    size_t b = out.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    size_t e = out.find_last_not_of(' ');
    out = out.substr(b, e - b + 1);
    if (out.size() > p.levelNameMax) {
        out.resize(p.levelNameMax);
        size_t t = out.find_last_not_of(' ');
        out = t == std::string::npos ? std::string() : out.substr(0, t + 1);
    }
    return out;
}

// ---- attempt clock ----

void AttemptClock::frame(double elapsedMs, bool paused, bool practice, bool startPos) {
    if (!std::isfinite(elapsedMs) || elapsedMs < 0.0 || elapsedMs > m_params.maxFrameMs || paused) {
        ++m_discarded;
        return;
    }
    ++m_counted;
    m_active += elapsedMs;
    if (practice) m_practice += elapsedMs;
    else if (startPos) m_startPos += elapsedMs;
}

}  // namespace gprl::display
