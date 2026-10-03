#include "entitlements.hpp"

#include <algorithm>
#include <cmath>

#include "identity.hpp"

namespace gprl::entitlements {

namespace sm = sim::modes;

char const* planId(Plan p) {
    switch (p) {
        case Plan::Free: return "free";
        case Plan::Supporter: return "supporter";
        case Plan::Plus: return "plus";
        case Plan::Pro: return "pro";
    }
    return "free";
}

char const* planLabel(Plan p) {
    switch (p) {
        case Plan::Free: return "Free";
        case Plan::Supporter: return "GPRL Supporter";
        case Plan::Plus: return "GPRL Plus";
        case Plan::Pro: return "GPRL Pro";
    }
    return "Free";
}

char const* statusId(Membership m) {
    switch (m) {
        case Membership::None: return "none";
        case Membership::Active: return "active";
        case Membership::Trial: return "trial";
        case Membership::CanceledActive: return "canceled_active";
        case Membership::PastDue: return "past_due";
        case Membership::Expired: return "expired";
    }
    return "none";
}

Plan parsePlan(std::string_view s) {
    if (s == "supporter") return Plan::Supporter;
    if (s == "plus") return Plan::Plus;
    if (s == "pro") return Plan::Pro;
    return Plan::Free;
}

Membership parseStatus(std::string_view s) {
    if (s == "active") return Membership::Active;
    if (s == "trial") return Membership::Trial;
    if (s == "canceled_active") return Membership::CanceledActive;
    if (s == "past_due") return Membership::PastDue;
    if (s == "expired") return Membership::Expired;
    return Membership::None;
}

sm::SpeedAllowance maxAllowance(Plan p) {
    switch (p) {
        case Plan::Free:
        case Plan::Supporter: return sm::SpeedAllowance::Normal;
        case Plan::Plus: return sm::SpeedAllowance::Faster;
        case Plan::Pro: return sm::SpeedAllowance::Fastest;
    }
    return sm::SpeedAllowance::Normal;
}

bool entitledStatus(Membership m) {
    return m == Membership::Active || m == Membership::Trial || m == Membership::CanceledActive || m == Membership::PastDue;
}

namespace {

sm::SpeedAllowance parseAllowance(std::string_view s) {
    if (s == "faster") return sm::SpeedAllowance::Faster;
    if (s == "fastest") return sm::SpeedAllowance::Fastest;
    return sm::SpeedAllowance::Normal;
}

/// "2026-11-02" from "2026-11-02T00:00:00.000Z" (only when the first ten characters are a date).
std::string datePart(std::string_view iso) {
    if (iso.size() < 10) return {};
    for (size_t i = 0; i < 10; ++i) {
        char c = iso[i];
        bool dash = i == 4 || i == 7;
        if (dash ? c != '-' : (c < '0' || c > '9')) return {};
    }
    return std::string(iso.substr(0, 10));
}

}  // namespace

Entitlement parse(json::Value const& body) {
    Entitlement e;
    if (!body.isObject()) return e;
    e.valid = true;
    e.status = parseStatus(body.getString("status"));
    Plan plan = parsePlan(body.getString("plan"));
    // a paid plan needs a membership state that carries one (a contradictory body is Free)
    if (plan != Plan::Free && !entitledStatus(e.status)) plan = Plan::Free;
    e.plan = plan;
    e.planLabel = planLabel(plan);
    e.trial = plan != Plan::Free && (body.getBool("trial") || e.status == Membership::Trial);
    // owner rule: a trial gets the software features, never the Priority Evidence Service
    e.priorityEvidence = plan == Plan::Pro && !e.trial && body.getBool("priorityEvidence");
    sm::SpeedAllowance claimed = parseAllowance(body["features"].getString("backgroundAnalysis"));
    sm::SpeedAllowance cap = maxAllowance(plan);
    e.backgroundAnalysis = static_cast<int>(claimed) > static_cast<int>(cap) ? cap : claimed;
    if (auto const* h = body.find("historyDays")) {
        if (h->isNull()) e.historyDays = std::nullopt;
        else if (h->isNumber() && std::isfinite(h->asNumber())) e.historyDays = static_cast<int>(std::clamp<int64_t>(h->asInt(), 0, 100000));
    }
    e.entitledUntil = body.getString("entitledUntil");
    auto const& patreon = body["patreon"];
    e.patreonConnected = patreon.getBool("connected");
    e.lastSyncAt = patreon.getString("lastSyncAt");
    e.generatedAt = body.getString("generatedAt");
    return e;
}

Entitlement parseText(std::string_view text) {
    json::Value v;
    if (!json::parse(text, v)) return Entitlement{};
    return parse(v);
}

std::string planLine(Entitlement const& e) {
    std::string s = std::string("Plan: ") + planLabel(e.plan);
    if (e.plan == Plan::Free) {
        if (e.patreonConnected && e.status == Membership::Expired) s += " - Patreon membership expired";
        return s;
    }
    s += " (Patreon)";
    std::string notes;
    auto add = [&](std::string const& n) { notes += (notes.empty() ? "" : ", ") + n; };
    if (e.trial) add("trial");
    if (e.status == Membership::CanceledActive) {
        std::string until = datePart(e.entitledUntil);
        add(until.empty() ? std::string("cancelled") : "cancelled, active until " + until);
    }
    if (e.status == Membership::PastDue) add("payment issue");
    if (!notes.empty()) s += " - " + notes;
    return s;
}

std::string lastSyncLine(std::string_view lastSyncAtIso, int64_t nowEpochSeconds) {
    int64_t at = lastSyncAtIso.empty() ? -1 : identity::parseIsoUtcSeconds(lastSyncAtIso);
    if (at < 0) return "Last synchronized: never";
    int64_t ago = nowEpochSeconds - at;
    if (ago < 60) return "Last synchronized: just now";
    int64_t minutes = ago / 60;
    if (minutes < 60) return "Last synchronized: " + std::to_string(minutes) + " min ago";
    int64_t hours = minutes / 60;
    if (hours < 48) return "Last synchronized: " + std::to_string(hours) + " h ago";
    return "Last synchronized: " + std::to_string(hours / 24) + " days ago";
}

bool authorizeUrlAllowed(std::string_view url) {
    if (url.size() > 4096) return false;
    constexpr std::string_view kPrefix = "https://www.patreon.com/oauth2/authorize?";
    if (url.size() <= kPrefix.size() || url.substr(0, kPrefix.size()) != kPrefix) return false;
    for (char ch : url) {
        auto c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c == 0x7f || c >= 0x80) return false;
        if (c == '\\' || c == '"' || c == '\'' || c == '`' || c == '<' || c == '>') return false;
    }
    return true;
}

std::string connectBody() {
    json::Value body = json::Value::object();
    body.set("returnTo", std::string("mod"));
    return json::canonical(body);
}

namespace {

/// PATREON_CONFIRM_CODE_ALPHABET (uppercase only; no I, L, O, 0, 1).
bool inCodeAlphabet(char c) {
    if (c >= '2' && c <= '9') return true;
    return c >= 'A' && c <= 'Z' && c != 'I' && c != 'L' && c != 'O';
}

char upperAscii(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

bool asciiSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

}  // namespace

std::string normalizePatreonCode(std::string_view input) {
    if (input.size() > 64) return {};
    size_t b = 0, e = input.size();
    while (b < e && asciiSpace(input[b])) ++b;
    while (e > b && asciiSpace(input[e - 1])) --e;
    std::string out;
    out.reserve(kPatreonCodeLength);
    for (size_t i = b; i < e; ++i) {
        char c = upperAscii(input[i]);
        if (c == ' ' || c == '-') continue;   // the server ignores a space or dash in the middle
        if (!inCodeAlphabet(c)) return {};
        out.push_back(c);
        if (out.size() > kPatreonCodeLength) return {};
    }
    return out.size() == kPatreonCodeLength ? out : std::string();
}

std::string patreonCodeInputText(std::string_view input) {
    std::string out;
    for (char ch : input.substr(0, std::min<size_t>(input.size(), 256))) {
        char c = upperAscii(ch);
        if (!inCodeAlphabet(c)) continue;
        out.push_back(c);
        if (out.size() == kPatreonCodeLength) break;
    }
    return out;
}

std::string confirmBody(std::string const& code) {
    json::Value body = json::Value::object();
    body.set("code", code);
    return json::canonical(body);
}

std::string confirmSuccessText(std::string_view planId) { return std::string("Patreon connected: ") + planLabel(parsePlan(planId)); }

namespace {

/// PATREON_CONFIRM_ERRORS -> the player's message; nullptr = not a known kind.
char const* confirmKindText(std::string_view kind) {
    if (kind == "link_mismatch") return "This Patreon link was started from another GPRL account, so it was cancelled. Connect again from your own account.";
    if (kind == "already_linked") return "This Patreon account is already linked to another GPRL account";
    if (kind == "ticket_expired") return "The link expired, try again";
    if (kind == "too_many_attempts") return "Too many wrong codes: connect again";
    if (kind == "ticket_invalid" || kind == "invalid_body") return "Patreon could not be connected, try again";
    return nullptr;
}

}  // namespace

std::string confirmErrorText(int status, std::string_view reason, std::string_view code) {
    if (auto const* text = confirmKindText(reason)) return text;   // ApiErrorBody details.reason (the contract)
    if (auto const* text = confirmKindText(code)) return text;     // a server that put the kind in error.code
    // neither names a kind (a proxy page, a generic error): the status alone
    if (status == 409) return confirmKindText("link_mismatch");
    if (status == 410) return confirmKindText("ticket_expired");
    if (status == 429) return confirmKindText("too_many_attempts");
    return "Patreon could not be connected, try again";
}

std::string confirmErrorTextFromBody(int status, json::Value const& body) {
    auto const& err = body["error"];
    std::string code = err.getString("code");
    std::string reason = err["details"].getString("reason");
    return confirmErrorText(status, reason, code);
}

}  // namespace gprl::entitlements
