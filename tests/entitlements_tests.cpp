// entitlements_tests (v0.12.2): core/entitlements.cpp - the GET /v1/me/entitlements parser, the
// Account tab texts, the Patreon authorize-URL guard and the plan -> analysis speed mapping.
//   1. every case of tests/fixtures/entitlements.json (all four plans, trial, cancelled-active,
//      past due, expired, missing fields -> Free, extra / hostile claims ignored or clamped,
//      malformed JSON -> invalid Free)
//   2. planLine / lastSyncLine edge cases
//   3. authorizeUrlAllowed: only https://www.patreon.com/oauth2/authorize? (security review LOW-9)
//   3b. the link confirmation code: normalisation, the input text, the confirm body, the success
//      text (the mod's own label) and the error-code messages
//   4. the entitlement decides which `analysis-cpu` tiers run (core/sim/modes.hpp resolveCpu):
//      fast / fastest need Plus / Pro from the SERVER; Free / Supporter keep low and normal
//
// Usage: entitlements_tests.exe <repo root>
#include "../core/entitlements.hpp"
#include "../core/identity.hpp"
#include "../core/json.hpp"
#include "test_util.hpp"

#include <string>

using namespace gprl;
using namespace gprl::entitlements;
namespace sm = gprl::sim::modes;

namespace {

std::string g_root = ".";

char const* allowanceId(sm::SpeedAllowance a) { return sm::name(a); }

void testFixture() {
    SECTION("fixture: tests/fixtures/entitlements.json (geode)");
    std::string path = g_root + "/geode/tests/fixtures/entitlements.json";
    std::string text = test::readFile(path);
    CHECK_MSG(!text.empty(), "fixture missing: " + path);
    json::Value root;
    CHECK(json::parse(text, root));
    auto const& cases = root["cases"];
    CHECK(cases.isArray());
    CHECK(cases.asArray().size() >= 15);
    int n = 0;
    for (auto const& c : cases.asArray()) {
        std::string name = c.getString("name");
        Entitlement e;
        if (c.has("bodyText")) e = parseText(c.getString("bodyText"));
        else {
            e = parse(c["body"]);
            // the text path gives the same answer as the value path
            Entitlement t = parseText(json::stringify(c["body"]));
            CHECK_MSG(t.plan == e.plan && t.valid == e.valid && t.backgroundAnalysis == e.backgroundAnalysis, name + ": text vs value");
        }
        auto const& x = c["expect"];
        CHECK_MSG(e.valid == x.getBool("valid"), name + ": valid");
        CHECK_MSG(std::string(planId(e.plan)) == x.getString("plan"), name + ": plan " + planId(e.plan));
        CHECK_MSG(e.planLabel == planLabel(e.plan), name + ": label is the mod's own");
        CHECK_MSG(std::string(statusId(e.status)) == x.getString("status"), name + ": status " + statusId(e.status));
        CHECK_MSG(e.trial == x.getBool("trial"), name + ": trial");
        CHECK_MSG(e.priorityEvidence == x.getBool("priorityEvidence"), name + ": priorityEvidence");
        CHECK_MSG(std::string(allowanceId(e.backgroundAnalysis)) == x.getString("backgroundAnalysis"),
                  name + ": backgroundAnalysis " + allowanceId(e.backgroundAnalysis));
        auto const& hd = x["historyDays"];
        if (hd.isNull()) CHECK_MSG(!e.historyDays.has_value(), name + ": historyDays all");
        else CHECK_MSG(e.historyDays.has_value() && *e.historyDays == hd.asInt(), name + ": historyDays");
        CHECK_MSG(e.patreonConnected == x.getBool("patreonConnected"), name + ": patreon.connected");
        CHECK_MSG(planLine(e) == x.getString("planLine"), name + ": planLine '" + planLine(e) + "'");
        // invariants on every case
        CHECK_MSG(static_cast<int>(e.backgroundAnalysis) <= static_cast<int>(maxAllowance(e.plan)), name + ": never above the plan");
        if (e.priorityEvidence) CHECK_MSG(e.plan == Plan::Pro && !e.trial, name + ": Priority Evidence only on paid Pro");
        if (e.trial) CHECK_MSG(e.plan != Plan::Free, name + ": a trial is of a paid tier");
        if (!e.valid) CHECK_MSG(e.plan == Plan::Free, name + ": invalid = Free");
        for (char ch : planLine(e)) CHECK_MSG(static_cast<unsigned char>(ch) < 0x80, name + ": ASCII only (GD fonts)");
        ++n;
    }
    CHECK(n == static_cast<int>(cases.asArray().size()));
}

void testDefaultsAndNames() {
    SECTION("defaults: a never-fetched entitlement is Free with the pre-0.12.2 speed");
    Entitlement e;
    CHECK(!e.valid);
    CHECK(e.plan == Plan::Free);
    CHECK(e.backgroundAnalysis == sm::SpeedAllowance::Normal);
    CHECK(!e.priorityEvidence);
    CHECK(planLine(e) == "Plan: Free");

    SECTION("names: PLAN_LABELS / plan ids / statuses of shared/src/entitlements/plans.ts");
    CHECK(std::string(planLabel(Plan::Free)) == "Free");
    CHECK(std::string(planLabel(Plan::Supporter)) == "GPRL Supporter");
    CHECK(std::string(planLabel(Plan::Plus)) == "GPRL Plus");
    CHECK(std::string(planLabel(Plan::Pro)) == "GPRL Pro");
    for (Plan p : {Plan::Free, Plan::Supporter, Plan::Plus, Plan::Pro}) CHECK(parsePlan(planId(p)) == p);
    for (Membership m : {Membership::None, Membership::Active, Membership::Trial, Membership::CanceledActive, Membership::PastDue, Membership::Expired})
        CHECK(parseStatus(statusId(m)) == m);
    CHECK(parsePlan("PRO") == Plan::Free);   // exact ids only
    CHECK(parsePlan("") == Plan::Free);
    CHECK(parseStatus("cancelled") == Membership::None);
    // PLAN_FEATURES.backgroundAnalysis
    CHECK(maxAllowance(Plan::Free) == sm::SpeedAllowance::Normal);
    CHECK(maxAllowance(Plan::Supporter) == sm::SpeedAllowance::Normal);
    CHECK(maxAllowance(Plan::Plus) == sm::SpeedAllowance::Faster);
    CHECK(maxAllowance(Plan::Pro) == sm::SpeedAllowance::Fastest);
    CHECK(entitledStatus(Membership::Active) && entitledStatus(Membership::Trial) && entitledStatus(Membership::CanceledActive) &&
          entitledStatus(Membership::PastDue));
    CHECK(!entitledStatus(Membership::None) && !entitledStatus(Membership::Expired));
    CHECK(connectBody() == "{\"returnTo\":\"mod\"}");
}

void testTexts() {
    SECTION("planLine: suffixes");
    Entitlement e;
    e.valid = true;
    e.plan = Plan::Plus;
    e.status = Membership::CanceledActive;
    e.entitledUntil = "";
    CHECK(planLine(e) == "Plan: GPRL Plus (Patreon) - cancelled");   // no date stated
    e.entitledUntil = "soon";
    CHECK(planLine(e) == "Plan: GPRL Plus (Patreon) - cancelled");   // not a date: not shown
    e.entitledUntil = "2026-12-24T23:59:59.000Z";
    CHECK(planLine(e) == "Plan: GPRL Plus (Patreon) - cancelled, active until 2026-12-24");
    e.trial = true;
    CHECK(planLine(e) == "Plan: GPRL Plus (Patreon) - trial, cancelled, active until 2026-12-24");
    e = Entitlement{};
    e.valid = true;
    e.plan = Plan::Pro;
    e.status = Membership::Active;
    CHECK(planLine(e) == "Plan: GPRL Pro (Patreon)");
    e.status = Membership::PastDue;
    CHECK(planLine(e) == "Plan: GPRL Pro (Patreon) - payment issue");
    e = Entitlement{};
    e.status = Membership::Expired;
    CHECK(planLine(e) == "Plan: Free");   // not connected to Patreon: nothing to say
    e.patreonConnected = true;
    CHECK(planLine(e) == "Plan: Free - Patreon membership expired");

    SECTION("lastSyncLine: relative to the injected now");
    int64_t now = identity::parseIsoUtcSeconds("2026-10-02T12:00:00.000Z");
    CHECK(now > 0);
    CHECK(lastSyncLine("", now) == "Last synchronized: never");
    CHECK(lastSyncLine("yesterday", now) == "Last synchronized: never");
    CHECK(lastSyncLine("2026-10-02T11:59:30.000Z", now) == "Last synchronized: just now");
    CHECK(lastSyncLine("2026-10-02T12:05:00.000Z", now) == "Last synchronized: just now");   // clock skew
    CHECK(lastSyncLine("2026-10-02T11:59:00.000Z", now) == "Last synchronized: 1 min ago");
    CHECK(lastSyncLine("2026-10-02T11:58:00Z", now) == "Last synchronized: 2 min ago");
    CHECK(lastSyncLine("2026-10-02T11:00:01.000Z", now) == "Last synchronized: 59 min ago");
    CHECK(lastSyncLine("2026-10-02T11:00:00.000Z", now) == "Last synchronized: 1 h ago");
    CHECK(lastSyncLine("2026-10-01T12:00:00.000Z", now) == "Last synchronized: 24 h ago");
    CHECK(lastSyncLine("2026-09-29T12:00:00.000Z", now) == "Last synchronized: 3 days ago");
}

void testAuthorizeUrl() {
    SECTION("authorizeUrlAllowed: exactly https://www.patreon.com/oauth2/authorize? (security review LOW-9)");
    CHECK(authorizeUrlAllowed("https://www.patreon.com/oauth2/authorize?response_type=code&client_id=abc&redirect_uri=https%3A%2F%2Fapi.example%2Fv1%2Fpatreon%2Fcallback&state=xyz"));
    CHECK(authorizeUrlAllowed("https://www.patreon.com/oauth2/authorize?state=1"));
    // the bare host and every other Patreon page are refused now
    CHECK(!authorizeUrlAllowed("https://patreon.com/oauth2/authorize?state=1"));
    CHECK(!authorizeUrlAllowed("https://www.patreon.com/oauth2/authorize"));    // no query
    CHECK(!authorizeUrlAllowed("https://www.patreon.com/oauth2/authorize?"));   // nothing after the '?'
    CHECK(!authorizeUrlAllowed("https://www.patreon.com/oauth2/authorizex?state=1"));
    CHECK(!authorizeUrlAllowed("https://www.patreon.com/oauth2/token?code=1"));
    CHECK(!authorizeUrlAllowed("https://www.patreon.com/login?ru=%2Foauth2%2Fauthorize%3Fstate%3D1"));
    CHECK(!authorizeUrlAllowed("https://www.patreon.com/gprl"));
    CHECK(!authorizeUrlAllowed("HTTPS://WWW.PATREON.COM/oauth2/authorize?state=1"));
    CHECK(!authorizeUrlAllowed("https://www.patreon.com/oauth2/authorize/../../evil?state=1"));
    CHECK(!authorizeUrlAllowed("https://www.patreon.com/oauth2/authorize?state=1\xc3\xa9"));   // non-ASCII
    CHECK(!authorizeUrlAllowed(""));
    CHECK(!authorizeUrlAllowed("https://www.patreon.com/"));   // nothing after the origin
    CHECK(!authorizeUrlAllowed("http://www.patreon.com/oauth2/authorize"));
    CHECK(!authorizeUrlAllowed("https://www.patreon.com.evil.example/oauth2/authorize"));
    CHECK(!authorizeUrlAllowed("https://www.patreon.com@evil.example/oauth2"));
    CHECK(!authorizeUrlAllowed("https://www.patreon.com:444/oauth2/authorize"));
    CHECK(!authorizeUrlAllowed("https://evil.example/https://www.patreon.com/"));
    CHECK(!authorizeUrlAllowed("javascript:alert(1)"));
    CHECK(!authorizeUrlAllowed("file:///C:/Windows/System32/calc.exe"));
    CHECK(!authorizeUrlAllowed("https://www.patreon.com/oauth2/authorize?x=\"onload"));
    CHECK(!authorizeUrlAllowed("https://www.patreon.com/oauth2/authorize?x= y"));
    CHECK(!authorizeUrlAllowed("https://www.patreon.com/oauth2\\..\\authorize"));
    CHECK(!authorizeUrlAllowed(std::string("https://www.patreon.com/oauth2/authorize?\n")));
    CHECK(!authorizeUrlAllowed("https://www.patreon.com/oauth2/authorize?state=" + std::string(5000, 'a')));   // over 4096
    CHECK(authorizeUrlAllowed("https://www.patreon.com/oauth2/authorize?state=" + std::string(4000, 'a')));
}

void testConfirmCode() {
    SECTION("Patreon code: the contract's alphabet (PATREON_CONFIRM_CODE_ALPHABET, no I / L / O)");
    CHECK(std::string(kPatreonCodeAlphabet) == "ABCDEFGHJKMNPQRSTUVWXYZ23456789");
    CHECK(std::string(kPatreonCodeAlphabet).size() == 31);
    CHECK(kPatreonCodeLength == 8);
    for (char banned : {'I', 'L', 'O', '0', '1', 'i', 'l', 'o'}) CHECK(std::string(kPatreonCodeInputFilter).find(banned) == std::string::npos);
    CHECK(std::string(kPatreonCodeInputFilter).size() == 31 + 23);   // upper + lower letters + 8 digits

    SECTION("Patreon code: normalizePatreonCode (trim, uppercase, drop spaces / dashes, exactly 8 of the alphabet)");
    CHECK(normalizePatreonCode("ABCD2345") == "ABCD2345");
    CHECK(normalizePatreonCode("abcd2345") == "ABCD2345");
    CHECK(normalizePatreonCode("  aBcD2345\t\n") == "ABCD2345");
    CHECK(normalizePatreonCode("ABCD-2345") == "ABCD2345");     // a dash in the middle is ignored (like the server)
    CHECK(normalizePatreonCode("abcd 2345") == "ABCD2345");     // so is a space
    CHECK(normalizePatreonCode(" AB-CD 23-45 ") == "ABCD2345");
    CHECK(normalizePatreonCode("ZZZZ2222") == "ZZZZ2222");
    CHECK(normalizePatreonCode("HJKMNPQR") == "HJKMNPQR");
    CHECK(normalizePatreonCode("OIOI9999") == "");               // O and I are not in the alphabet
    CHECK(normalizePatreonCode("ABCDEFGL") == "");               // nor L
    CHECK(normalizePatreonCode("abcdefgl") == "");               // in any case
    CHECK(normalizePatreonCode("ABCD2340") == "");               // nor 0
    CHECK(normalizePatreonCode("ABCD2341") == "");               // nor 1
    CHECK(normalizePatreonCode("") == "");
    CHECK(normalizePatreonCode("--------") == "");
    CHECK(normalizePatreonCode("ABCD234") == "");                // 7
    CHECK(normalizePatreonCode("ABCD-234") == "");               // 7 once the dash is gone
    CHECK(normalizePatreonCode("ABCD23456") == "");              // 9
    CHECK(normalizePatreonCode("ABCD_2345") == "");              // only spaces and dashes are dropped
    CHECK(normalizePatreonCode("ABCD\t2345") == "");             // a tab in the middle is not a space
    CHECK(normalizePatreonCode("ABCD234\xc3\x89") == "");        // non-ASCII
    CHECK(normalizePatreonCode(std::string(70, ' ') + "ABCD2345") == "");   // over 64 characters: refused unread
    for (char const* c = kPatreonCodeAlphabet; *c; ++c) CHECK(normalizePatreonCode(std::string(8, *c)) == std::string(8, *c));
    // every character the input filter lets through normalises into the alphabet
    for (char const* c = kPatreonCodeInputFilter; *c; ++c) CHECK(normalizePatreonCode(std::string(8, *c)).size() == 8);
    // the result always matches PATREON_CONFIRM_CODE_PATTERN ^[ABCDEFGHJKMNPQRSTUVWXYZ23456789]{8}$
    for (char const* in : {"abcd2345", " ab-cd 23-45 ", "zzzz9999"}) {
        std::string out = normalizePatreonCode(in);
        CHECK(out.size() == 8);
        for (char ch : out) CHECK(std::string(kPatreonCodeAlphabet).find(ch) != std::string::npos);
    }

    SECTION("Patreon code: patreonCodeInputText (what the box shows while typing / after Paste)");
    CHECK(patreonCodeInputText("abcd") == "ABCD");
    CHECK(patreonCodeInputText(" abcd-2345 ") == "ABCD2345");
    CHECK(patreonCodeInputText("ABCD 2345") == "ABCD2345");
    CHECK(patreonCodeInputText("ABCD23456789") == "ABCD2345");   // at most 8
    CHECK(patreonCodeInputText("0101") == "");
    CHECK(patreonCodeInputText("OIL") == "");
    CHECK(patreonCodeInputText("Code: AB-CD 23-45") == "CDEABCD2");   // what Paste of a whole line gives: letters outside I/L/O kept
    CHECK(patreonCodeInputText("") == "");

    SECTION("Patreon code: the confirm body and the result texts");
    CHECK(confirmBody("ABCD2345") == "{\"code\":\"ABCD2345\"}");
    CHECK(confirmSuccessText("pro") == "Patreon connected: GPRL Pro");
    CHECK(confirmSuccessText("plus") == "Patreon connected: GPRL Plus");
    CHECK(confirmSuccessText("supporter") == "Patreon connected: GPRL Supporter");
    CHECK(confirmSuccessText("free") == "Patreon connected: Free");
    CHECK(confirmSuccessText("GPRL Ultra <script>") == "Patreon connected: Free");   // never server text
    std::string const mismatch = "This Patreon link was started from another GPRL account, so it was cancelled. Connect again from your own account.";
    std::string const alreadyLinked = "This Patreon account is already linked to another GPRL account";
    std::string const expired = "The link expired, try again";
    std::string const tooMany = "Too many wrong codes: connect again";
    std::string const generic = "Patreon could not be connected, try again";

    SECTION("Patreon code: confirmErrorText - details.reason first (PATREON_CONFIRM_ERRORS with their real codes / statuses)");
    CHECK(confirmErrorText(409, "link_mismatch", "conflict") == mismatch);
    CHECK(confirmErrorText(409, "already_linked", "conflict") == alreadyLinked);
    CHECK(confirmErrorText(410, "ticket_expired", "bad_request") == expired);
    CHECK(confirmErrorText(404, "ticket_invalid", "not_found") == generic);
    CHECK(confirmErrorText(429, "too_many_attempts", "rate_limited") == tooMany);
    CHECK(confirmErrorText(400, "invalid_body", "bad_request") == generic);
    // the reason decides, not the status
    CHECK(confirmErrorText(409, "ticket_expired", "conflict") == expired);
    CHECK(confirmErrorText(429, "already_linked", "rate_limited") == alreadyLinked);
    CHECK(confirmErrorText(409, "invalid_body", "conflict") == generic);   // a known kind: no 409 fallback

    SECTION("Patreon code: confirmErrorText - error.code when it names a kind, the status only when neither does");
    CHECK(confirmErrorText(400, "", "too_many_attempts") == tooMany);
    CHECK(confirmErrorText(400, "", "link_mismatch") == mismatch);
    CHECK(confirmErrorText(409, "something_new", "already_linked") == alreadyLinked);   // an unknown reason falls through to the code
    CHECK(confirmErrorText(409, "", "conflict") == mismatch);            // status fallback
    CHECK(confirmErrorText(409, "", "") == mismatch);
    CHECK(confirmErrorText(410, "", "") == expired);
    CHECK(confirmErrorText(410, "", "bad_request") == expired);
    CHECK(confirmErrorText(429, "", "rate_limited") == tooMany);
    CHECK(confirmErrorText(429, "something_new", "rate_limited") == tooMany);
    CHECK(confirmErrorText(404, "", "not_found") == generic);
    CHECK(confirmErrorText(400, "", "bad_request") == generic);
    CHECK(confirmErrorText(500, "", "internal") == generic);
    CHECK(confirmErrorText(503, "", "maintenance") == generic);
    CHECK(confirmErrorText(0, "", "") == generic);                       // network failure

    SECTION("Patreon code: confirmErrorTextFromBody reads error.details.reason / error.code of real-shaped bodies");
    struct BodyCase {
        int status;
        char const* text;
        std::string expect;
    };
    BodyCase const bodies[] = {
        {409, R"({"error":{"code":"conflict","message":"This Patreon account is linked to another GPRL player.","details":{"reason":"already_linked"}}})", alreadyLinked},
        {409, R"({"error":{"code":"conflict","message":"The link was started from another account.","details":{"reason":"link_mismatch"}}})", mismatch},
        {410, R"({"error":{"code":"bad_request","message":"Expired.","details":{"reason":"ticket_expired"}}})", expired},
        {404, R"({"error":{"code":"not_found","message":"Unknown code.","details":{"reason":"ticket_invalid"}}})", generic},
        {429, R"({"error":{"code":"rate_limited","message":"Too many attempts.","details":{"reason":"too_many_attempts"}}})", tooMany},
        {400, R"({"error":{"code":"bad_request","message":"Send exactly one of ticket / code.","details":{"reason":"invalid_body"}}})", generic},
        // no details / odd shapes: error.code, then the status
        {409, R"({"error":{"code":"conflict","message":"Conflict."}})", mismatch},
        {409, R"({"error":{"code":"already_linked","message":"x"}})", alreadyLinked},
        {410, R"({"error":{"code":"bad_request","details":null}})", expired},
        {429, R"({"error":{"code":"rate_limited","details":{"reason":42}}})", tooMany},
        {409, R"({"error":{"code":"conflict","details":"already_linked"}})", mismatch},   // details not an object: no reason
        {404, R"({"error":"ticket_invalid"})", generic},
        {410, R"([])", expired},
        {502, R"({"message":"Bad gateway"})", generic},
    };
    for (auto const& b : bodies) {
        json::Value v;
        CHECK_MSG(json::parse(b.text, v), b.text);
        CHECK_MSG(confirmErrorTextFromBody(b.status, v) == b.expect, std::string(b.text) + " -> " + confirmErrorTextFromBody(b.status, v));
    }
    CHECK(confirmErrorTextFromBody(410, json::Value()) == expired);   // an empty / unparsed body: the status
    CHECK(confirmErrorTextFromBody(0, json::Value()) == generic);
    for (auto const& s : {mismatch, alreadyLinked, expired, tooMany, generic})
        for (char ch : s) CHECK_MSG(static_cast<unsigned char>(ch) < 0x80, "ASCII only (GD fonts)");
    for (char ch : std::string(kPatreonCodeHint)) CHECK_MSG(static_cast<unsigned char>(ch) < 0x80, "ASCII only (GD fonts)");
    CHECK(std::string(kPatreonCodeHint) == "The code has 8 characters: letters and digits 2-9 (no I, L or O)");
}

void testSpeedMapping() {
    SECTION("analysis speed: the plan from the server caps analysis-cpu (fast needs Plus, fastest needs Pro)");
    auto bodyFor = [](char const* plan, char const* status, char const* speed) {
        json::Value b = json::Value::object();
        b.set("plan", plan);
        b.set("status", status);
        json::Value f = json::Value::object();
        f.set("backgroundAnalysis", speed);
        b.set("features", std::move(f));
        return parse(b);
    };
    Entitlement unknown;   // not connected / not fetched
    Entitlement free = bodyFor("free", "none", "normal");
    Entitlement supporter = bodyFor("supporter", "active", "normal");
    Entitlement plus = bodyFor("plus", "active", "faster");
    Entitlement pro = bodyFor("pro", "active", "fastest");
    Entitlement trial = bodyFor("pro", "trial", "fastest");
    Entitlement liar = bodyFor("free", "active", "fastest");
    auto run = [](Entitlement const& e, sm::CpuTier t) { return sm::resolveCpu(t, e.backgroundAnalysis).effective; };
    // Free / Supporter / unknown: exactly today's choices, fast / fastest fall back to normal
    for (auto const* e : {&unknown, &free, &supporter, &liar}) {
        CHECK(run(*e, sm::CpuTier::Low) == sm::CpuTier::Low);
        CHECK(run(*e, sm::CpuTier::Normal) == sm::CpuTier::Normal);
        CHECK(run(*e, sm::CpuTier::Fast) == sm::CpuTier::Normal);
        CHECK(run(*e, sm::CpuTier::Fastest) == sm::CpuTier::Normal);
        CHECK(sm::resolveCpu(sm::CpuTier::Fast, e->backgroundAnalysis).limited);
        CHECK(!sm::resolveCpu(sm::CpuTier::Normal, e->backgroundAnalysis).limited);
    }
    // Plus: + fast; fastest falls back to the best Plus allows
    CHECK(run(plus, sm::CpuTier::Low) == sm::CpuTier::Low);
    CHECK(run(plus, sm::CpuTier::Normal) == sm::CpuTier::Normal);
    CHECK(run(plus, sm::CpuTier::Fast) == sm::CpuTier::Fast);
    CHECK(run(plus, sm::CpuTier::Fastest) == sm::CpuTier::Fast);
    // Pro and the Pro trial (software features): everything
    for (auto const* e : {&pro, &trial}) {
        CHECK(run(*e, sm::CpuTier::Fast) == sm::CpuTier::Fast);
        CHECK(run(*e, sm::CpuTier::Fastest) == sm::CpuTier::Fastest);
        CHECK(run(*e, sm::CpuTier::Low) == sm::CpuTier::Low);
    }
    // the Session tab says why
    CHECK(sm::speedLine(sm::resolveCpu(sm::CpuTier::Fast, free.backgroundAnalysis)) == "Analysis speed: Normal (Fast needs GPRL Plus)");
    CHECK(sm::speedLine(sm::resolveCpu(sm::CpuTier::Fastest, plus.backgroundAnalysis)) == "Analysis speed: Fast (Fastest needs GPRL Pro)");
    CHECK(sm::speedLine(sm::resolveCpu(sm::CpuTier::Fastest, pro.backgroundAnalysis)) == "Analysis speed: Fastest (GPRL Pro)");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) g_root = argv[1];
    testFixture();
    testDefaultsAndNames();
    testTexts();
    testAuthorizeUrl();
    testConfirmCode();
    testSpeedMapping();
    return gprl::test::finish("entitlements_tests");
}
