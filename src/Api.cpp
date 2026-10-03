#include "Api.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/web.hpp>

#include <algorithm>
#include <chrono>

#include "../core/analyzer_worker_rules.hpp"
#include "../core/crypto.hpp"
#include "Settings.hpp"

using namespace geode::prelude;

namespace gprl::api {

namespace {

std::string userAgent() { return settings::clientBuild(); }

Response finish(web::WebResponse const& res, std::string const& what) {
    Response out;
    out.status = res.code();
    if (res.cancelled()) {
        out.error = what + ": request cancelled";
        out.status = 0;
        return out;
    }
    auto text = res.string();
    std::string bodyText = text.isOk() ? text.unwrap() : std::string();
    if (!bodyText.empty()) {
        json::ParseError pe;
        if (!json::parse(bodyText, out.body, &pe)) {
            out.error = fmt::format("{}: HTTP {} with unparseable body ({})", what, out.status, pe.message);
            if (out.status >= 200 && out.status < 300) return out;   // a 2xx with junk is still a failure
        }
    }
    if (out.status >= 200 && out.status < 300) {
        out.ok = out.error.empty();
        return out;
    }
    // v0.12.0 (review LOW): a 429 / 503 may say when to come back (delta-seconds only)
    if (out.status != 0) {
        auto ra = res.header("Retry-After");
        if (!ra) ra = res.header("retry-after");
        if (ra) out.retryAfterSeconds = analyzer::rules::parseRetryAfter(std::string(ra->view()));
    }
    // ApiErrorBody
    auto const& err = out.body["error"];
    if (err.isObject()) {
        out.code = err.getString("code");
        out.reason = err["details"].getString("reason");
        out.detailUrl = err["details"].getString("url");
        out.message = err.getString("message");
        out.error = fmt::format("{}: HTTP {} {}: {}", what, out.status, out.code, err.getString("message"));
    }
    else if (out.status == 0) {
        std::string_view msg = res.errorMessage();
        out.error = fmt::format("{}: network error{}{}", what, msg.empty() ? "" : ": ", msg);
    }
    else out.error = fmt::format("{}: HTTP {}", what, out.status);
    return out;
}

web::WebRequest baseRequest(Config const& cfg, std::string const& bearer) {
    web::WebRequest req;
    req.timeout(std::chrono::seconds(cfg.timeoutSeconds));
    req.userAgent(userAgent());
    req.header("Accept", "application/json");
    if (!bearer.empty()) req.header("Authorization", "Bearer " + bearer);
    return req;
}

json::Value modListJson(std::vector<telemetry::EnvironmentMod> const& mods) {
    json::Value list = json::Value::array();
    for (auto const& m : mods) {
        json::Value o = json::Value::object();
        o.set("id", m.id);
        o.set("version", m.version);
        list.push(std::move(o));
    }
    return list;
}

}  // namespace

Response post(Config const& cfg, std::string const& path, std::string const& body, std::string const& bearer, std::string const& signatureHex) {
    if (cfg.baseUrl.empty()) {
        Response r;
        r.error = "API base URL is not configured";
        return r;
    }
    auto req = baseRequest(cfg, bearer);
    req.header("Content-Type", "application/json");
    if (!signatureHex.empty()) req.header("X-GPRL-Signature", signatureHex);
    req.bodyString(body);
    GPRL_DEBUG("GPRL api: POST {}{} ({} bytes)", cfg.baseUrl, path, body.size());
    auto res = req.postSync(cfg.baseUrl + path);
    auto out = finish(res, "POST " + path);
    GPRL_DEBUG("GPRL api: POST {} -> {}{}", path, out.status, out.ok ? "" : " " + out.error);
    return out;
}

Response get(Config const& cfg, std::string const& path, std::string const& bearer) {
    if (cfg.baseUrl.empty()) {
        Response r;
        r.error = "API base URL is not configured";
        return r;
    }
    auto req = baseRequest(cfg, bearer);
    GPRL_DEBUG("GPRL api: GET {}{}", cfg.baseUrl, path);
    auto res = req.getSync(cfg.baseUrl + path);
    auto out = finish(res, "GET " + path);
    GPRL_DEBUG("GPRL api: GET {} -> {}{}", path, out.status, out.ok ? "" : " " + out.error);
    return out;
}

ConnectResult connect(Config const& cfg, ConnectRequest const& rq, std::string const& clientBuild) {
    json::Value body = json::Value::object();
    body.set("accountId", rq.accountId);
    body.set("userId", rq.userId);
    body.set("username", rq.username);
    body.set("argonToken", rq.argonToken);
    body.set("clientBuild", clientBuild);
    body.set("modList", modListJson(rq.modList));
    auto res = post(cfg, "/v1/client/connect", json::canonical(body), {});
    ConnectResult r;
    r.status = res.status;
    r.code = res.code;
    r.reason = res.reason;
    if (!res.ok) {
        r.error = res.error;
        return r;
    }
    r.deviceToken = res.body.getString("deviceToken");
    r.playerId = res.body.getString("playerId");
    r.username = res.body.getString("username");
    r.displayName = res.body.getString("displayName");
    r.identityVerified = res.body.getBool("identityVerified");
    if (r.displayName.empty()) r.displayName = r.username;
    if (r.deviceToken.empty()) {
        r.error = "connect: response without deviceToken";
        return r;
    }
    r.ok = true;
    return r;
}

WebLoginResult webLogin(Config const& cfg, std::string const& deviceToken) {
    auto res = post(cfg, "/v1/client/web-login", "{}", deviceToken);
    WebLoginResult r;
    r.status = res.status;
    r.code = res.code;
    if (!res.ok) {
        r.error = res.error;
        return r;
    }
    r.url = res.body.getString("url");
    r.expiresAt = res.body.getString("expiresAt");
    r.loginCode = res.body.getString("code");
    if (r.url.empty()) {
        r.error = "web-login: response without url";
        return r;
    }
    r.ok = true;
    return r;
}

SessionResult createSession(Config const& cfg, std::string const& deviceToken, std::string const& levelId, std::string const& levelHash,
                            std::string const& clientBuild, std::vector<telemetry::EnvironmentMod> const& mods, IntegrityReport const& integrity,
                            LevelHints const& hints) {
    json::Value body = json::Value::object();
    body.set("levelId", levelId);
    body.set("levelHash", levelHash);
    body.set("clientBuild", clientBuild);
    body.set("modList", modListJson(mods));
    json::Value integ = json::Value::object();
    integ.set("state", name(integrity.state));
    integ.set("gdHash", integrity.gdHash);
    integ.set("geodeHash", integrity.geodeHash);
    integ.set("gprlHash", integrity.gprlHash);
    body.set("integrity", std::move(integ));
    // v0.5.1 level hints (optional, never trusted server side): only what GD actually shows
    if (hints.stars) body.set("levelStars", std::clamp(*hints.stars, 0, 10));
    if (hints.isDemon) body.set("levelIsDemon", *hints.isDemon);
    if (!hints.demonDifficulty.empty()) body.set("levelDemonDifficulty", hints.demonDifficulty);
    if (!hints.name.empty()) body.set("levelName", hints.name);
    // level families: the copied-from level id (GD key 30) as a string of digits, omitted when 0
    if (hints.originalLevelId > 0) body.set("originalLevelId", std::to_string(hints.originalLevelId));
    auto res = post(cfg, "/v1/client/sessions", json::canonical(body), deviceToken);
    SessionResult r;
    r.status = res.status;
    if (!res.ok) {
        r.error = res.error;
        return r;
    }
    r.sessionId = res.body.getString("sessionId");
    r.nonce = res.body.getString("nonce");
    r.seq0 = res.body.getInt("seq0");
    r.ratable = res.body.getBool("ratable");
    r.ratableReason = res.body.getString("ratableReason");
    // "Which levels count" (absent on servers from before v0.5.1 = counts)
    r.levelCounts = res.body.getBool("levelCounts", true);
    r.levelCountsReason = res.body.getString("levelCountsReason");
    if (auto const* rating = res.body.find("levelRating"); rating && rating->isObject()) {
        r.levelRating.stars = static_cast<int>(std::clamp<int64_t>(rating->getInt("stars"), 0, 10));
        r.levelRating.isDemon = rating->getBool("isDemon");
        r.levelRating.demonDifficulty = rating->getString("demonDifficulty");
        r.levelRating.source = rating->getString("source", "none");
    }
    // absent on servers from before v0.6.0 = revision 1 (no clip_available)
    r.telemetryRevision = static_cast<int>(std::clamp<int64_t>(res.body.getInt("telemetryRevision", 1), 1, 1000));
    std::string key64 = res.body.getString("sessionKey");
    if (r.sessionId.empty() || key64.empty() || !crypto::fromBase64(key64, r.sessionKeyBytes) || r.sessionKeyBytes.empty()) {
        r.error = "sessions: response without sessionId / decodable sessionKey";
        return r;
    }
    r.ok = true;
    return r;
}

std::string encodePathSegment(std::string const& segment) {
    std::string encoded;
    for (char c : segment) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') encoded.push_back(c);
        else encoded += fmt::format("%{:02X}", static_cast<unsigned char>(c));
    }
    return encoded;
}

RanksResult getRanks(Config const& cfg) {
    auto res = get(cfg, "/api/ranks", {});
    RanksResult r;
    r.status = res.status;
    if (!res.ok) {
        r.error = res.error;
        return r;
    }
    std::string err;
    if (!ranks::parseRankList(res.body, r.list, &err)) {
        r.error = "ranks: " + err;
        return r;
    }
    r.ok = true;
    return r;
}

LeaderboardResult getLeaderboard(Config const& cfg, int limit) {
    auto res = get(cfg, fmt::format("/api/leaderboard?limit={}", std::max(1, limit)), {});
    LeaderboardResult r;
    r.status = res.status;
    if (!res.ok) {
        r.error = res.error;
        return r;
    }
    std::string err;
    if (!ranks::parseLeaderboard(res.body, r.board, &err)) {
        r.error = "leaderboard: " + err;
        return r;
    }
    r.ok = true;
    return r;
}

ProfileResult getPlayer(Config const& cfg, std::string const& username) {
    ProfileResult r;
    if (username.empty()) {
        r.error = "players: no username";
        return r;
    }
    auto res = get(cfg, "/api/players/" + encodePathSegment(username), {});
    r.status = res.status;
    r.notFound = res.status == 404;
    if (!res.ok) {
        r.error = r.notFound ? "no GPRL player named " + username + " yet" : res.error;
        return r;
    }
    std::string err;
    if (!ranks::parseProfile(res.body, r.profile, &err)) {
        r.error = "players: " + err;
        return r;
    }
    r.ok = true;
    return r;
}

std::string signBody(std::string const& sessionKeyBytes, std::string const& canonicalBody) {
    return crypto::toHex(crypto::hmacSha256(sessionKeyBytes, canonicalBody));
}

BatchResult postBatch(Config const& cfg, std::string const& deviceToken, telemetry::Batch const& batch, std::string const& sessionKeyBytes,
                      std::string* signatureHexOut) {
    std::string body = telemetry::canonicalBody(batch);
    std::string signature = signBody(sessionKeyBytes, body);
    if (signatureHexOut) *signatureHexOut = signature;
    auto res = post(cfg, "/v1/telemetry/batches", body, deviceToken, signature);
    BatchResult r;
    r.status = res.status;
    r.code = res.code;
    r.retryable = res.retryable();
    if (!res.ok) {
        r.error = res.error;
        return r;
    }
    r.ok = true;
    r.accepted = res.body.getBool("accepted", true);
    r.nextSeq = res.body.getInt("nextSeq", -1);
    r.eventCount = static_cast<int>(res.body.getInt("eventCount", static_cast<int64_t>(batch.events.size())));
    // v0.6.0 preserveEvidence hint (optional): present at all = the server evaluates SPEC §27
    // (core/clip_flow parsePreserveHint: untrusted shape, capped, host-tested)
    auto hint = clip::parsePreserveHint(res.body);
    r.preserveKnown = hint.known;
    r.preserveAttemptIds = std::move(hint.attemptIds);
    r.preserveReason = std::move(hint.reason);
    // v0.9.0 run review: the server names the run of this session it wants verified
    auto verification = clip::parseVerificationHint(res.body);
    r.verificationKnown = verification.known;
    r.verificationRequests = std::move(verification.requests);
    return r;
}

EvidenceSessionResult createEvidenceUpload(Config const& cfg, std::string const& deviceToken, clip::ClipRecord const& record,
                                           std::string const& clientBuild) {
    auto res = post(cfg, clip::kFlow.uploadsPath, json::canonical(clip::uploadRequestJson(record, clientBuild)), deviceToken);
    EvidenceSessionResult r;
    r.status = res.status;
    r.code = res.code;
    r.error = res.error;
    if (res.status >= 200 && res.status < 300) {
        std::string err;
        r.parsed = clip::parseUploadSession(res.body, r.session, &err);
        if (!r.parsed) r.error = "evidence upload: " + err;
    }
    return r;
}

EvidenceLinkResult postEvidenceLink(Config const& cfg, std::string const& deviceToken, clip::ClipRecord const& record, std::string const& url,
                                    std::string const& clientBuild) {
    auto res = post(cfg, clip::kFlow.linksPath, json::canonical(clip::linkRequestJson(record, url, clientBuild)), deviceToken);
    EvidenceLinkResult r;
    r.status = res.status;
    r.code = res.code;
    r.detailUrl = res.detailUrl;
    r.message = res.message;
    r.error = res.error;
    if (res.status >= 200 && res.status < 300) {
        r.ok = true;
        r.canonicalUrl = res.body.getString("url");
        r.title = res.body.getString("title");
    }
    else if (res.status == 409 && res.code == "evidence_exists") {
        r.ok = true;
    }
    return r;
}

EvidencePutResult putEvidence(clip::PutPlan const& plan, std::string const& bearer, std::string const& sha256Hex, std::vector<uint8_t> bytes,
                              int timeoutSeconds, std::atomic<double>* uploadedFraction) {
    EvidencePutResult r;
    if (!plan.ok) {
        r.error = plan.error;
        return r;
    }
    web::WebRequest req;
    req.timeout(std::chrono::seconds(timeoutSeconds));
    req.userAgent(userAgent());
    req.header("Content-Type", "video/mp4");
    for (auto const& [key, value] : plan.headers) req.header(key, value);
    if (plan.sendBearer && !bearer.empty()) {
        // only the API's own origin ever sees the device token (core/clip_flow planPut)
        req.header("Authorization", "Bearer " + bearer);
        req.header("X-GPRL-Content-SHA256", sha256Hex);
    }
    size_t total = bytes.size();
    if (uploadedFraction) {
        req.onProgress([uploadedFraction, total](web::WebProgress const& p) {
            size_t of = p.uploadTotal() > 0 ? p.uploadTotal() : total;
            if (of > 0) uploadedFraction->store(std::clamp(static_cast<double>(p.uploaded()) / static_cast<double>(of), 0.0, 1.0));
        });
    }
    req.body(std::move(bytes));
    // the URL may be a one-time presigned link: never logged
    GPRL_DEBUG("GPRL api: PUT evidence ({} bytes, {} origin)", total, plan.sendBearer ? "API" : "storage");
    auto res = req.putSync(plan.url);
    auto out = finish(res, "PUT evidence");
    r.status = out.status;
    r.code = out.code;
    r.error = out.error;
    if (out.body.isObject()) r.sha256 = out.body.getString("sha256");
    GPRL_DEBUG("GPRL api: PUT evidence -> {}{}", r.status, out.ok ? "" : " " + out.error);
    return r;
}

EndResult endSession(Config const& cfg, std::string const& deviceToken, std::string const& sessionId, SessionEndReason reason, int64_t lastSeq,
                     std::optional<int64_t> reportedAttempts) {
    json::Value body = json::Value::object();
    body.set("reason", name(reason));
    body.set("lastSeq", lastSeq);
    // v0.5.1 (MASTER §10): the GD save's attempt count, untrusted context; the route accepts 0..2e9
    if (reportedAttempts && *reportedAttempts >= 0) body.set("reportedAttempts", std::min<int64_t>(*reportedAttempts, 2'000'000'000));
    auto res = post(cfg, "/v1/client/sessions/" + encodePathSegment(sessionId) + "/end", json::canonical(body), deviceToken);
    EndResult r;
    r.status = res.status;
    if (!res.ok) {
        r.error = res.error;
        return r;
    }
    r.ok = true;
    r.eventCount = res.body.getInt("eventCount");
    r.recalculationQueued = res.body.getBool("recalculationQueued");
    auto verification = clip::parseVerificationHint(res.body);
    r.verificationKnown = verification.known;
    r.verificationRequests = std::move(verification.requests);
    return r;
}

LevelAnalysisResult getLevelAnalysis(Config const& cfg, std::string const& levelId) {
    LevelAnalysisResult r;
    if (levelId.empty()) {
        r.error = "level analysis: no level id";
        return r;
    }
    auto res = get(cfg, "/v1/levels/" + encodePathSegment(levelId) + "/analysis", {});
    r.status = res.status;
    r.notFound = res.status == 404;
    if (!res.ok) {
        r.error = r.notFound ? "the server has no analysis for level " + levelId + " yet" : res.error;
        return r;
    }
    std::string err;
    if (!display::parseLevelCoverage(res.body, r.coverage, &err)) {
        r.error = "level analysis: " + err;
        return r;
    }
    r.ok = true;
    return r;
}

LevelSimCacheResult levelSimCache(Config const& cfg, int64_t gdLevelId, std::string const& gameplayHash, std::string const& analyzerVersion,
                                  std::string const& simVersion) {
    LevelSimCacheResult r;
    if (gdLevelId <= 0 || gameplayHash.empty()) {
        r.error = "sim cache: no level id / gameplay hash";
        return r;
    }
    auto path = fmt::format("/v1/levels/{}/sim-cache?gameplayHash={}&analyzerVersion={}&simVersion={}", gdLevelId, encodePathSegment(gameplayHash),
                            encodePathSegment(analyzerVersion), encodePathSegment(simVersion));
    auto res = get(cfg, path, {});
    r.status = res.status;
    if (!res.ok) {
        r.error = res.error;
        return r;
    }
    if (!res.body.isObject()) {
        r.error = "sim cache: answer is not an object";
        return r;
    }
    r.ok = true;
    r.found = res.body.getBool("found");
    if (!r.found) return r;
    r.resultStatus = res.body.getString("status");
    r.physicsPercent = res.body.getNumber("physicsPercent");
    r.solvedPercent = res.body.getNumber("solvedPercent");
    if (auto* v = res.body.find("verifiedPercent"); v && v->isNumber()) {
        r.hasVerified = true;
        r.verifiedPercent = v->asNumber();
    }
    r.windowsCount = res.body.getInt("windowsCount");
    r.computedAt = res.body.getString("computedAt");
    return r;
}

LevelSimUploadResult uploadLevelSim(Config const& cfg, std::string const& deviceToken, std::string const& resultJson, std::string const& sessionId) {
    // the body is assembled as text: the result is already serialised (it can be large; a second
    // parse + stringify round trip is avoided). The worker's 2 MB check measures this same text.
    std::string body = analyzer::rules::levelSimUploadBody(sessionId, resultJson);
    auto res = post(cfg, "/v1/me/level-sim", body, deviceToken);
    LevelSimUploadResult r;
    r.status = res.status;
    r.code = res.code;
    r.retryable = res.retryable() && res.status != 429;   // 429 = the server's 10-minute upload rule, retried by the worker on its own clock
    r.retryAfterSeconds = res.retryAfterSeconds;
    if (!res.ok) {
        r.error = res.error;
        r.reason = res.reason;
        return r;
    }
    r.ok = true;
    r.stored = res.body.getBool("stored");
    r.replaced = res.body.getBool("replaced");
    r.reason = res.body.getString("reason");
    r.windowsStored = res.body.getInt("windowsStored");
    auto const& st = res.body["status"];
    if (st.isObject()) r.statusLabel = st.getString("label");
    else if (st.isString()) r.statusLabel = st.asString();
    return r;
}

LevelIdentityUploadResult uploadLevelIdentity(Config const& cfg, std::string const& deviceToken, std::string const& identityJson, std::string const& sessionId) {
    // text assembly like uploadLevelSim: the identity is already serialised (up to ~1 MB)
    std::string body = analyzer::family::uploadBody(sessionId, identityJson);
    auto res = post(cfg, "/v1/me/level-identity", body, deviceToken);
    LevelIdentityUploadResult r;
    r.status = res.status;
    r.code = res.code;
    r.reason = res.reason;
    r.retryable = res.retryable();
    r.retryAfterSeconds = res.retryAfterSeconds;
    if (!res.ok) {
        r.error = res.error;
        return r;
    }
    std::string err;
    if (!analyzer::family::parseAnswer(res.body, r.answer, &err)) {
        r.error = "level identity: " + err;
        return r;
    }
    r.answerText = json::stringify(res.body);
    r.ok = true;
    return r;
}

namespace {

/// The GET /v1/me/calibration body (also the calibration part of POST /v1/me/recalc): one parser.
void calibrationFromResponse(Response const& res, CalibrationResult& r) {
    r.status = res.status;
    if (!res.ok) {
        r.error = res.error;
        return;
    }
    r.algorithmVersion = res.body.getString("algorithmVersion");
    r.updatedAt = res.body.getString("updatedAt");
    std::string err;
    if (!calibrationStateFromJson(res.body["overall"], r.overall, &err)) {
        r.error = "calibration: " + err;
        return;
    }
    calibrationDisplayFromJson(res.body, r.display);   // tolerant: absent => locked
    r.ok = true;
}

}  // namespace

CalibrationResult getCalibration(Config const& cfg, std::string const& deviceToken) {
    auto res = get(cfg, "/v1/me/calibration", deviceToken);
    CalibrationResult r;
    calibrationFromResponse(res, r);
    return r;
}

LiveRecalcResult liveRecalc(Config const& cfg, std::string const& deviceToken) {
    json::Value body = json::Value::object();
    body.set("reason", std::string("live"));
    auto res = post(cfg, "/v1/me/recalc", json::canonical(body), deviceToken);
    LiveRecalcResult r;
    r.status = res.status;
    calibrationFromResponse(res, r.calibration);
    if (!r.calibration.ok) {
        r.error = res.ok ? r.calibration.error : res.error;
        if (r.error.empty()) r.error = "live recalc: unusable answer";
        return r;
    }
    if (!live::parseLiveRecalcAnswer(res.body, r.answer)) {
        // a server without the route answers 404 (not ok above); a 2xx without the live fields is
        // still a calibration answer, but not a recalculation
        r.error = "live recalc: answer without `recalculated`";
        return r;
    }
    r.ok = true;
    return r;
}

EntitlementsResult fetchEntitlements(Config const& cfg, std::string const& deviceToken) {
    auto res = get(cfg, "/v1/me/entitlements", deviceToken);
    EntitlementsResult r;
    r.status = res.status;
    r.code = res.code;
    if (!res.ok) {
        r.error = res.error;
        return r;
    }
    if (!res.body.isObject()) {
        r.error = "entitlements: answer is not an object";
        return r;
    }
    r.entitlement = entitlements::parse(res.body);
    r.ok = r.entitlement.valid;
    return r;
}

PatreonConnectResult patreonConnect(Config const& cfg, std::string const& deviceToken) {
    // the GPRL_DEBUG lines of post() name the path only: the authorize URL in the answer is never logged
    auto res = post(cfg, "/v1/me/patreon/connect", entitlements::connectBody(), deviceToken);
    PatreonConnectResult r;
    r.status = res.status;
    r.code = res.code;
    r.message = res.message;
    if (!res.ok) {
        r.error = res.error;
        return r;
    }
    r.authorizeUrl = res.body.getString("authorizeUrl");
    r.expiresAt = res.body.getString("expiresAt");
    if (r.authorizeUrl.empty()) {
        r.error = "patreon connect: answer without authorizeUrl";
        return r;
    }
    r.ok = true;
    return r;
}

PatreonSyncResult patreonSync(Config const& cfg, std::string const& deviceToken) {
    auto res = post(cfg, "/v1/me/patreon/sync", "{}", deviceToken);
    PatreonSyncResult r;
    r.status = res.status;
    r.code = res.code;
    r.message = res.message;
    r.retryAfterSeconds = res.retryAfterSeconds;
    if (!res.ok) {
        r.error = res.error;
        return r;
    }
    r.ok = true;
    r.connected = res.body.getBool("connected");
    r.available = res.body.getBool("available", true);
    r.plan = res.body.getString("plan");
    r.lastError = res.body.getString("lastError");
    return r;
}

PatreonConfirmResult patreonConfirm(Config const& cfg, std::string const& deviceToken, std::string const& code) {
    // the body carries the one-time code: post() logs only the path and the byte count
    auto res = post(cfg, "/v1/me/patreon/confirm", entitlements::confirmBody(code), deviceToken);
    PatreonConfirmResult r;
    r.status = res.status;
    r.code = res.code;
    r.reason = res.reason;
    if (!res.ok) {
        r.error = res.error;
        // details.reason first, then error.code, the status only when neither names a kind
        r.playerMessage = entitlements::confirmErrorTextFromBody(res.status, res.body);
        return r;
    }
    r.ok = true;
    r.connected = res.body.getBool("connected");
    r.plan = res.body.getString("plan");
    return r;
}

ResetResult resetData(Config const& cfg, std::string const& deviceToken) {
    json::Value body = json::Value::object();
    body.set("confirm", std::string("RESET"));
    auto res = post(cfg, "/v1/me/reset", json::canonical(body), deviceToken);
    ResetResult r;
    r.status = res.status;
    if (!res.ok) {
        r.error = res.error;
        return r;
    }
    r.ok = true;
    r.sessions = res.body.getInt("sessions");
    if (res.body.has("deleted")) r.timingSamples = res.body["deleted"].getInt("timing_samples");
    GPRL_DEBUG("GPRL api: reset -> {} sessions, {} timing samples removed", r.sessions, r.timingSamples);
    return r;
}

}  // namespace gprl::api
