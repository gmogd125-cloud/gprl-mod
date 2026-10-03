#pragma once
// GPRL plan / entitlement as the server hands it to the mod (v0.12.2): GET /v1/me/entitlements ->
// V1EntitlementsResponse (shared/src/entitlements/plans.ts `Entitlement` + `generatedAt`,
// shared/src/entitlements/api.ts, docs/contracts/patreon.md). PURE, host-tested in
// tests/entitlements_tests.cpp with tests/fixtures/entitlements.json.
//
// The server is the only authority. The mod never derives a plan from a setting, a saved value or
// a file, and never writes the entitlement to disk: it lives in the telemetry worker's memory
// (client::Status), fetched after Connect, every 10 minutes and after Sync Patreon, and forgotten
// on Disconnect. Missing or unknown fields parse as Free; a body that claims more than its plan
// allows (a Free plan with `fastest` analysis, Priority Evidence on a trial) is clamped to the plan.
//
// A plan is a SERVICE level only. In the mod it changes the background analysis speed the player
// may pick (core/sim/modes.hpp resolveCpu) and the Account tab text - nothing in the measuring,
// telemetry, sigma/s or upload content ever reads it.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "json.hpp"
#include "sim/modes.hpp"

namespace gprl::entitlements {

enum class Plan : uint8_t { Free = 0, Supporter = 1, Plus = 2, Pro = 3 };

/// MembershipStatus (plans.ts MEMBERSHIP_STATUSES).
enum class Membership : uint8_t { None, Active, Trial, CanceledActive, PastDue, Expired };

struct Entitlement {
    bool valid = false;            // a server answer was parsed (false = never fetched / unusable -> Free)
    Plan plan = Plan::Free;
    std::string planLabel = "Free";   // the mod's own label for `plan` (PLAN_LABELS), never server text
    Membership status = Membership::None;
    bool trial = false;            // Patreon free trial of a paid tier (software features, no Priority Evidence)
    bool priorityEvidence = false;
    sim::modes::SpeedAllowance backgroundAnalysis = sim::modes::SpeedAllowance::Normal;
    std::optional<int> historyDays = 90;   // nullopt = all history
    std::string entitledUntil;     // ISO, "" = not stated
    bool patreonConnected = false;
    std::string lastSyncAt;        // ISO, "" = never
    std::string generatedAt;
};

/// "free" / "supporter" / "plus" / "pro".
char const* planId(Plan p);
/// "Free" / "GPRL Supporter" / "GPRL Plus" / "GPRL Pro" (plans.ts PLAN_LABELS).
char const* planLabel(Plan p);
/// "none" / "active" / "trial" / "canceled_active" / "past_due" / "expired".
char const* statusId(Membership m);
/// Unknown -> Free.
Plan parsePlan(std::string_view s);
/// Unknown -> None.
Membership parseStatus(std::string_view s);
/// The fastest background analysis a plan includes (plans.ts PLAN_FEATURES.backgroundAnalysis).
sim::modes::SpeedAllowance maxAllowance(Plan p);
/// Membership states that carry a paid plan (active, trial, cancelled-but-active, past due).
bool entitledStatus(Membership m);

/// The parsed body. Not an object -> invalid Free. Rules: unknown plan -> Free; a paid plan with
/// status none / expired -> Free (contradictory body); trial only on a paid plan (status `trial`
/// also sets it); Priority Evidence only on Pro and never on a trial; backgroundAnalysis from
/// features.backgroundAnalysis, missing / unknown -> normal, clamped to maxAllowance(plan); every
/// other member is ignored.
Entitlement parse(json::Value const& body);
/// parse() of JSON text; malformed -> invalid Free.
Entitlement parseText(std::string_view text);

/// The Account tab line: "Plan: Free", "Plan: GPRL Pro (Patreon)", "Plan: GPRL Pro (Patreon) - trial",
/// "Plan: GPRL Plus (Patreon) - cancelled, active until 2026-11-02", "Plan: GPRL Supporter (Patreon) -
/// payment issue", "Plan: Free - Patreon membership expired". ASCII only (GD fonts).
std::string planLine(Entitlement const& e);

/// "Last synchronized: just now" / "1 min ago" / "N min ago" / "N h ago" / "N days ago";
/// "Last synchronized: never" when `lastSyncAtIso` is empty or not ISO 8601 UTC. A time in the
/// future (clock skew) counts as just now.
std::string lastSyncLine(std::string_view lastSyncAtIso, int64_t nowEpochSeconds);

/// The browser may only be sent to Patreon's own OAuth authorize page (security review LOW-9):
/// `url` starts with exactly "https://www.patreon.com/oauth2/authorize?" followed by at least one
/// character, is at most 4096 characters and carries no whitespace, control or non-ASCII
/// characters, backslashes, quotes or angle brackets. A misbehaving server can therefore never make
/// the mod open another site or another Patreon page.
bool authorizeUrlAllowed(std::string_view url);

/// The POST /v1/me/patreon/connect body of the mod: {"returnTo":"mod"} (V1PatreonConnectRequest).
std::string connectBody();

// ---- link confirmation (security review, V1PatreonConfirmRequest) ----
//
// After Connect Patreon the plain page the callback shows names the Patreon account, the GPRL
// account and a one-time code of 8 characters (shared/src/entitlements/api.ts
// PATREON_CONFIRM_CODE_ALPHABET / _LENGTH / _PATTERN: A-Z and 2-9 without I, L, O); the player types
// it into "Enter Patreon code" and the mod sends POST /v1/me/patreon/confirm {"code":"<CODE>"} with
// its device token, so a Patreon link started from one GPRL account can never land on another
// (V1PatreonConfirmRequest; 5 wrong codes delete the pending link). The code is never logged.

inline constexpr size_t kPatreonCodeLength = 8;
/// PATREON_CONFIRM_CODE_ALPHABET: 31 symbols, no I, L, O (and no 0, 1).
inline constexpr char const* kPatreonCodeAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
/// What the TextInput accepts while typing: the alphabet in both cases (lowercase is uppercased).
/// Spaces and dashes are not typed (they would take one of the 8 places); Paste strips them.
inline constexpr char const* kPatreonCodeInputFilter = "ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";
/// Shown when the typed value is not a code.
inline constexpr char const* kPatreonCodeHint = "The code has 8 characters: letters and digits 2-9 (no I, L or O)";

/// The code in normal form (what the server matches, case-insensitive with spaces / dashes
/// ignored): ASCII whitespace trimmed at both ends, ASCII letters uppercased, spaces and dashes
/// dropped; "" unless the result is exactly 8 characters of kPatreonCodeAlphabet (inputs over 64
/// characters are refused unread).
std::string normalizePatreonCode(std::string_view input);
/// ASCII letters uppercased, everything outside the alphabet (spaces, dashes, I, L, O, 0, 1, ...)
/// dropped, at most 8 characters kept: what the input box shows while typing / after Paste (never a
/// validity check).
std::string patreonCodeInputText(std::string_view input);
/// {"code":"<CODE>"} (canonical JSON). `code` must already be normalised.
std::string confirmBody(std::string const& code);
/// "Patreon connected: GPRL Pro" - the mod's own label for the answer's plan id, never server text.
std::string confirmSuccessText(std::string_view planId);

/// The player's message for a failed confirm (PATREON_CONFIRM_ERRORS). The kind is ApiErrorBody
/// `error.details.reason`; `error.code` is used when it names a kind itself; the HTTP status only
/// when neither is a known kind (409 -> link_mismatch, 410 -> ticket_expired, 429 ->
/// too_many_attempts, anything else generic):
///   link_mismatch (409 conflict)        "This Patreon link was started from another GPRL account, so
///                                        it was cancelled. Connect again from your own account."
///   already_linked (409 conflict)       "This Patreon account is already linked to another GPRL account"
///   ticket_expired (410 bad_request)    "The link expired, try again"
///   too_many_attempts (429 rate_limited) "Too many wrong codes: connect again"
///   ticket_invalid (404 not_found), invalid_body (400 bad_request), anything else:
///                                       "Patreon could not be connected, try again"
std::string confirmErrorText(int status, std::string_view reason, std::string_view code);
/// The same from a whole error body: reads `error.details.reason` and `error.code` of `body` (any
/// shape: a missing / non-object member is "unknown").
std::string confirmErrorTextFromBody(int status, json::Value const& body);

}  // namespace gprl::entitlements
