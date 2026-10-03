#include "clip_flow.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>

#include "crypto.hpp"

namespace gprl::clip {

namespace {

struct NamedChoice {
    Choice value;
    char const* name;
    char const* label;
};
constexpr NamedChoice kChoices[] = {
    {Choice::Nothing, "nothing", "Do nothing"},
    {Choice::SaveLocal, "save", "Save to computer"},
    {Choice::Send, "send", "Send to GPRL moderators"},
    {Choice::SaveAndSend, "save_send", "Save + send"},
};

struct NamedState {
    ClipState value;
    char const* name;
};
constexpr NamedState kStates[] = {
    {ClipState::Preparing, "preparing"},       {ClipState::Ready, "ready"},
    {ClipState::Saved, "saved"},               {ClipState::Uploading, "uploading"},
    {ClipState::Uploaded, "uploaded"},         {ClipState::UploadFailed, "upload_failed"},
    {ClipState::Discarded, "discarded"},       {ClipState::Failed, "failed"},
};

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool isLocalHost(std::string_view origin) {
    // origin = "scheme://host[:port]"
    auto p = origin.find("://");
    if (p == std::string_view::npos) return false;
    std::string_view host = origin.substr(p + 3);
    if (auto colon = host.rfind(':'); colon != std::string_view::npos && host.find(']') == std::string_view::npos) host = host.substr(0, colon);
    return host == "localhost" || host == "127.0.0.1";
}

/// A header name as it may be shown / logged: ASCII letters, digits and '-', at most 40 characters.
std::string safeHeaderName(std::string_view key) {
    std::string out;
    for (char c : key) {
        if (out.size() >= 40) break;
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-') ? c : '?';
    }
    return out;
}

/// The actions of a choice on a prepared clip (state Ready).
std::vector<Action> applyChoice(ClipRecord& r, Choice c, UploadGate const& gate) {
    std::vector<Action> actions;
    switch (c) {
        case Choice::Nothing:
            r.choiceMade = true;
            r.choice = c;
            r.state = ClipState::Discarded;
            r.error.clear();
            actions = {Action::DeleteFile, Action::Persist};
            break;
        case Choice::SaveLocal:
            r.choiceMade = true;
            r.choice = c;
            r.savedLocal = true;
            r.state = ClipState::Saved;
            r.error.clear();
            actions = {Action::MoveToClipsFolder, Action::EmitClipAvailable, Action::Persist};
            break;
        case Choice::Send: {
            std::string block = uploadBlockReason(r, gate);
            if (!block.empty()) {
                // nothing happens to the clip: the player can still pick another choice
                r.choiceMade = false;
                r.error = "not sent: " + block;
                actions = {Action::Persist};
                break;
            }
            r.choiceMade = true;
            r.choice = c;
            r.state = ClipState::Uploading;
            r.error.clear();
            actions = {Action::StartUpload, Action::Persist};
            break;
        }
        case Choice::SaveAndSend: {
            std::string block = uploadBlockReason(r, gate);
            r.choiceMade = true;
            r.choice = c;
            r.savedLocal = true;
            actions = {Action::MoveToClipsFolder, Action::EmitClipAvailable};
            if (!block.empty()) {
                r.state = ClipState::Saved;
                r.error = "saved; not sent: " + block;
            }
            else {
                r.state = ClipState::Uploading;
                r.error.clear();
                actions.push_back(Action::StartUpload);
            }
            actions.push_back(Action::Persist);
            break;
        }
    }
    return actions;
}

}  // namespace

// ---- names ----

char const* name(Choice c) {
    for (auto const& k : kChoices) {
        if (k.value == c) return k.name;
    }
    return "nothing";
}

char const* label(Choice c) {
    for (auto const& k : kChoices) {
        if (k.value == c) return k.label;
    }
    return "Do nothing";
}

bool parse(std::string_view s, Choice& out) {
    for (auto const& k : kChoices) {
        if (s == k.name) {
            out = k.value;
            return true;
        }
    }
    return false;
}

char const* name(ClipState s) {
    for (auto const& k : kStates) {
        if (k.value == s) return k.name;
    }
    return "failed";
}

bool parse(std::string_view s, ClipState& out) {
    for (auto const& k : kStates) {
        if (s == k.name) {
            out = k.value;
            return true;
        }
    }
    return false;
}

char const* name(Action a) {
    switch (a) {
        case Action::MoveToClipsFolder: return "move_to_clips_folder";
        case Action::EmitClipAvailable: return "emit_clip_available";
        case Action::StartUpload: return "start_upload";
        case Action::DeleteFile: return "delete_file";
        case Action::Persist: return "persist";
    }
    return "";
}

// ---- flow ----

std::string uploadBlockReason(ClipRecord const& r, UploadGate const& gate) {
    if (gate.localOnly) return "local-only mode is on (or the API is not set), so nothing is sent";
    if (!gate.connected) return "not connected: press Connect in the GPRL menu first";
    // v0.9.0: a run the server asked to verify (rule "verification") is sent whatever the level is
    if (!r.levelCounts && r.rule != "verification") return "this level does not count for GPRL (not a rated demon), so there is nothing to verify";
    if (r.path.empty() || !validSha256Hex(r.sha256) || r.sizeBytes <= 0) return "the clip file is not ready";
    if (gate.maxBytes > 0 && r.sizeBytes > gate.maxBytes) {
        return "the clip is " + formatBytes(r.sizeBytes) + ", the server accepts at most " + formatBytes(gate.maxBytes)
            + " (lower the clip quality or the buffer length)";
    }
    return {};
}

std::vector<Action> onPrepared(ClipRecord& r, bool ok, std::string const& error, UploadGate const& gate) {
    if (r.state != ClipState::Preparing) return {};
    if (!ok) {
        r.state = ClipState::Failed;
        r.error = error.empty() ? std::string("the clip could not be made") : error;
        return {Action::DeleteFile, Action::Persist};
    }
    r.state = ClipState::Ready;
    r.error.clear();
    if (r.choiceMade) return applyChoice(r, r.choice, gate);
    return {Action::Persist};
}

std::vector<Action> onChoice(ClipRecord& r, Choice c, UploadGate const& gate) {
    switch (r.state) {
        case ClipState::Preparing:
            // remembered, applied by onPrepared (the file does not exist yet)
            r.choiceMade = true;
            r.choice = c;
            return {};
        case ClipState::Ready:
            return applyChoice(r, c, gate);
        case ClipState::UploadFailed:
            if (wantsSend(c)) {
                // An explicit Send / Save + send press on a failed upload = Retry. "Save + send" on a
                // clip that was only being sent also keeps the copy the player now asks for,
                // whatever the retry does.
                std::vector<Action> actions;
                if (c == Choice::SaveAndSend && !r.savedLocal) {
                    r.savedLocal = true;
                    actions = {Action::MoveToClipsFolder, Action::EmitClipAvailable};
                }
                r.choiceMade = true;
                r.choice = r.savedLocal ? Choice::SaveAndSend : Choice::Send;
                auto retry = onRetryUpload(r, gate);
                if (!actions.empty() && r.state == ClipState::UploadFailed) r.error = "saved; " + r.error;
                actions.insert(actions.end(), retry.begin(), retry.end());
                return actions;
            }
            if (c == Choice::SaveLocal && !r.savedLocal) {
                r.choice = c;
                r.savedLocal = true;
                r.state = ClipState::Saved;
                r.error.clear();
                return {Action::MoveToClipsFolder, Action::EmitClipAvailable, Action::Persist};
            }
            // give up on the upload: a saved clip stays saved, an unsaved one is deleted
            if (r.savedLocal) {
                r.state = ClipState::Saved;
                r.error.clear();
                return {Action::Persist};
            }
            r.state = ClipState::Discarded;
            r.error.clear();
            return {Action::DeleteFile, Action::Persist};
        default:
            return {};
    }
}

std::vector<Action> onUploadResult(ClipRecord& r, bool ok, std::string const& error) {
    if (r.state != ClipState::Uploading) return {};
    if (!ok) {
        r.state = ClipState::UploadFailed;
        r.error = error.empty() ? std::string("the upload failed") : error;
        return {Action::Persist};
    }
    r.uploaded = true;
    r.state = ClipState::Uploaded;
    r.error.clear();
    // "Send" without "Save": the player did not ask to keep a copy
    if (!r.savedLocal) return {Action::DeleteFile, Action::Persist};
    return {Action::Persist};
}

std::vector<Action> onRetryUpload(ClipRecord& r, UploadGate const& gate) {
    // only ever after an explicit Send / Save + send
    if (r.state != ClipState::UploadFailed || !r.choiceMade || !wantsSend(r.choice)) return {};
    std::string block = uploadBlockReason(r, gate);
    if (!block.empty()) {
        r.error = "not sent: " + block;
        return {Action::Persist};
    }
    r.state = ClipState::Uploading;
    r.error.clear();
    return {Action::StartUpload, Action::Persist};
}

void reconcileAfterRestart(ClipRecord& r, bool fileExists) {
    switch (r.state) {
        case ClipState::Preparing:
            r.state = ClipState::Failed;
            r.error = "the game closed while the clip was being made";
            r.path.clear();
            break;
        case ClipState::Uploading:
            if (fileExists) {
                r.state = ClipState::UploadFailed;
                r.error = "the game closed during the upload";
            }
            else {
                r.state = ClipState::Failed;
                r.error = "the clip file is missing";
                r.path.clear();
            }
            break;
        case ClipState::Ready:
        case ClipState::UploadFailed:
            if (!fileExists) {
                r.state = ClipState::Failed;
                r.error = "the clip file is missing";
                r.path.clear();
            }
            break;
        default:
            break;
    }
}

bool awaitingChoice(ClipRecord const& r) {
    if (r.state == ClipState::Ready || r.state == ClipState::UploadFailed) return true;
    return r.state == ClipState::Preparing && !r.choiceMade;
}

// ---- upload contract ----

json::Value uploadRequestJson(ClipRecord const& r, std::string const& clientBuild) {
    json::Value o = json::Value::object();
    o.set("clipId", r.clipId);
    o.set("attemptId", r.attemptId);
    o.set("sessionId", r.serverSessionId.empty() ? json::Value(nullptr) : json::Value(r.serverSessionId));
    o.set("levelId", r.levelId);
    o.set("levelHash", r.levelHash);
    o.set("kind", "video");
    o.set("contentType", "video/mp4");
    o.set("sizeBytes", r.sizeBytes);
    o.set("sha256", r.sha256);
    o.set("durationMs", std::round(r.durationMs));
    o.set("width", r.width);
    o.set("height", r.height);
    o.set("hasGameAudio", r.hasGameAudio);
    o.set("hasMicAudio", r.hasMicAudio);
    o.set("hasDesktopAudio", r.hasDesktopAudio);
    o.set("audioTracks", audioTracksJson(r));
    o.set("wholeAttempt", r.wholeAttempt);
    o.set("visibility", "private");
    o.set("clientBuild", clientBuild);
    return o;
}

json::Value audioTracksJson(ClipRecord const& r) {
    json::Value tracks = json::Value::array();
    int index = 0;
    auto add = [&](char const* name, char const* kind) {
        json::Value t = json::Value::object();
        t.set("index", static_cast<int64_t>(index++));
        t.set("name", name);
        t.set("kind", kind);
        tracks.asArray().push_back(std::move(t));
    };
    if (r.hasGameAudio) add("Game", "game");
    if (r.hasMicAudio) add("Microphone", "mic");
    if (r.hasDesktopAudio) add("Desktop", "desktop");
    return tracks;
}

bool parseUploadSession(json::Value const& body, UploadSession& out, std::string* err) {
    auto fail = [&](char const* msg) {
        if (err) *err = msg;
        return false;
    };
    out = UploadSession{};
    if (!body.isObject()) return fail("the upload session is not an object");
    out.uploadId = body.getString("uploadId");
    out.uploadUrl = body.getString("uploadUrl");
    out.expiresAt = body.getString("expiresAt");
    if (out.uploadId.empty() || out.uploadId.size() > 128) return fail("the upload session has no uploadId");
    if (out.uploadUrl.empty() || out.uploadUrl.size() > 4096) return fail("the upload session has no uploadUrl");
    double maxBytes = body.getNumber("maxBytes", 0.0);
    out.maxBytes = maxBytes > 0.0 && std::isfinite(maxBytes) ? static_cast<int64_t>(maxBytes) : 0;
    if (auto const* headers = body.find("headers"); headers && headers->isObject()) {
        if (headers->asObject().size() > kFlow.maxHeaderCount) return fail("the upload session lists too many headers");
        for (auto const& [key, value] : headers->asObject()) {
            if (!value.isString()) return fail("an upload session header is not a string");
            out.headers.emplace_back(key, value.asString());
        }
    }
    return true;
}

std::string originOf(std::string_view url) {
    std::string u = lower(url);
    std::string scheme;
    if (u.rfind("https://", 0) == 0) scheme = "https";
    else if (u.rfind("http://", 0) == 0) scheme = "http";
    else return {};
    size_t hostStart = scheme.size() + 3;
    size_t end = u.find_first_of("/?#", hostStart);
    std::string host = u.substr(hostStart, end == std::string::npos ? std::string::npos : end - hostStart);
    if (host.empty() || host.find('@') != std::string::npos) return {};   // no userinfo tricks
    for (char c : host) {
        bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']';
        if (!ok) return {};
    }
    // default ports are the same origin as no port
    std::string defaultPort = scheme == "https" ? ":443" : ":80";
    if (host.size() > defaultPort.size() && host.compare(host.size() - defaultPort.size(), defaultPort.size(), defaultPort) == 0
        && host.find(']') == std::string::npos) {
        host.resize(host.size() - defaultPort.size());
    }
    return scheme + "://" + host;
}

PutPlan planPut(std::string const& apiBaseUrl, UploadSession const& session) {
    PutPlan plan;
    std::string apiOrigin = originOf(apiBaseUrl);
    if (apiOrigin.empty()) {
        plan.error = "the API base URL is not usable";
        return plan;
    }
    if (session.uploadUrl.empty()) {
        plan.error = "the upload session has no URL";
        return plan;
    }
    // printable ASCII without spaces: a URL with control characters or line breaks is never requested
    for (char c : session.uploadUrl) {
        auto u = static_cast<unsigned char>(c);
        if (u <= 32 || u >= 127) {
            plan.error = "the upload URL contains characters that are not allowed";
            return plan;
        }
    }
    if (session.uploadUrl[0] == '/') {
        if (session.uploadUrl.size() > 1 && session.uploadUrl[1] == '/') {
            plan.error = "the upload URL is not a path on the GPRL API";   // "//host/..." is another host
            return plan;
        }
        std::string base = apiBaseUrl;
        while (!base.empty() && base.back() == '/') base.pop_back();
        plan.url = base + session.uploadUrl;
        plan.sendBearer = true;
    }
    else {
        std::string origin = originOf(session.uploadUrl);
        if (origin.empty()) {
            plan.error = "the upload URL is not an http(s) URL";
            return plan;
        }
        plan.url = session.uploadUrl;
        plan.sendBearer = origin == apiOrigin;
        if (origin.rfind("https://", 0) != 0 && !isLocalHost(origin)) {
            plan.error = "refusing to upload over plain http";
            return plan;
        }
    }
    static constexpr char const* kForbidden[] = {"authorization", "proxy-authorization", "cookie",  "host",
                                                 "content-length", "transfer-encoding",  "connection"};
    if (session.headers.size() > kFlow.maxHeaderCount) {
        plan.error = "the upload session lists too many headers";
        return plan;
    }
    for (auto const& [key, value] : session.headers) {
        std::string k = lower(key);
        bool nameOk = !k.empty() && k.size() <= 64
            && std::all_of(k.begin(), k.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-'; });
        bool valueOk = value.size() <= kFlow.maxHeaderLength
            && std::all_of(value.begin(), value.end(), [](char c) { return static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) < 127; });
        bool forbidden = std::any_of(std::begin(kForbidden), std::end(kForbidden), [&](char const* f) { return k == f; });
        if (!nameOk || !valueOk || forbidden) {
            plan.error = "the upload session asked for a header the mod will not send (" + safeHeaderName(key) + ")";
            plan.headers.clear();
            return plan;
        }
        plan.headers.emplace_back(key, value);
    }
    plan.ok = true;
    return plan;
}

UploadStep classifySessionResponse(int status, std::string const& code) {
    UploadStep s;
    if (status >= 200 && status < 300) {
        s.verdict = UploadVerdict::Done;
        return s;
    }
    if (status == 409 && code == "evidence_exists") {
        s.verdict = UploadVerdict::Done;
        s.alreadyUploaded = true;
        s.message = "the server already has this clip";
        return s;
    }
    if (status == 0 || status == 408 || status == 425 || status == 429 || status >= 500) {
        // 501 Not Implemented is a statement about the server, not a transient failure
        if (status == 501) {
            s.message = "this GPRL server does not accept evidence uploads yet";
            return s;
        }
        s.verdict = UploadVerdict::Retry;
        s.message = status == 0 ? "network error" : "the server is busy (HTTP " + std::to_string(status) + ")";
        return s;
    }
    switch (status) {
        case 401:
            s.reconnect = true;
            s.message = "the server rejected this device: press Connect again";
            break;
        case 403: s.message = "the server refused the upload for this account"; break;
        case 404:
        case 405: s.message = "this GPRL server does not accept evidence uploads yet"; break;
        case 413: s.message = "the clip is larger than the server accepts"; break;
        default:
            s.message = "the server rejected the upload request (HTTP " + std::to_string(status) + (code.empty() ? "" : " " + code) + ")";
            break;
    }
    return s;
}

UploadStep classifyPutResponse(int status, std::string const& code, std::string const& serverSha256, std::string const& ourSha256) {
    UploadStep s;
    if (status >= 200 && status < 300) {
        if (!serverSha256.empty() && lower(serverSha256) != lower(ourSha256)) {
            s.verdict = UploadVerdict::Retry;
            s.message = "the server received different bytes (hash mismatch)";
            return s;
        }
        s.verdict = UploadVerdict::Done;
        return s;
    }
    if (status == 409 && code == "evidence_exists") {
        s.verdict = UploadVerdict::Done;
        s.alreadyUploaded = true;
        s.message = "the server already has this clip";
        return s;
    }
    if (status == 401) {
        s.reconnect = true;
        s.message = "the server rejected this device: press Connect again";
        return s;
    }
    if (status == 413) {
        s.message = "the clip is larger than the server accepts";
        return s;
    }
    if (status == 501) {
        s.message = "this GPRL server does not store evidence yet";
        return s;
    }
    // an expired / used one-time URL (403, 404, 409, 410) and every transient failure: a NEW
    // upload session is requested and the bytes are sent again
    if (status == 0 || status == 403 || status == 404 || status == 408 || status == 409 || status == 410 || status == 425 || status == 429
        || status >= 500) {
        s.verdict = UploadVerdict::Retry;
        s.message = status == 0 ? "network error during the upload" : "the upload was not accepted (HTTP " + std::to_string(status) + ")";
        return s;
    }
    s.message = "the server rejected the upload (HTTP " + std::to_string(status) + (code.empty() ? "" : " " + code) + ")";
    return s;
}

int backoffMs(int tryIndex) {
    if (tryIndex <= 1) return 0;
    int i = std::min(tryIndex - 2, 2);
    return kFlow.backoffMs[i];
}

// ---- the server's hint ----

PreserveHint parsePreserveHint(json::Value const& ackBody) {
    PreserveHint hint;
    auto const* member = ackBody.find("preserveEvidence");
    if (!member) return hint;
    hint.known = true;
    if (!member->isObject()) return hint;   // null / anything else: "evaluated, nothing named"
    if (auto const* reason = member->find("reason"); reason && reason->isString()) {
        for (char c : reason->asString()) {
            if (hint.reason.size() >= kFlow.maxPreserveReasonLength) break;
            auto u = static_cast<unsigned char>(c);
            if (u >= 32 && u < 127) hint.reason += c;   // GD's bitmap fonts: printable ASCII only
        }
    }
    auto const* ids = member->find("attemptIds");
    if (!ids || !ids->isArray()) return hint;
    for (auto const& id : ids->asArray()) {
        if (hint.attemptIds.size() >= kFlow.maxPreserveAttempts) break;
        if (!id.isString() || id.asString().empty() || id.asString().size() > kFlow.maxAttemptIdLength) continue;
        if (std::find(hint.attemptIds.begin(), hint.attemptIds.end(), id.asString()) != hint.attemptIds.end()) continue;
        hint.attemptIds.push_back(id.asString());
    }
    return hint;
}

// ---- v0.10.0: YouTube links ----

bool overUploadCap(ClipRecord const& r, UploadGate const& gate) {
    return gate.maxBytes > 0 && r.sizeBytes > gate.maxBytes;
}

double maxUploadSeconds(int videoKbps, int audioTracks, int audioKbps, int64_t maxBytes) {
    double kbps = static_cast<double>(std::max(videoKbps, 1)) + static_cast<double>(std::max(audioTracks, 0)) * std::max(audioKbps, 0);
    if (maxBytes <= 0) return 0.0;
    return static_cast<double>(maxBytes) * 8.0 / (kbps * 1000.0);
}

namespace {

bool validVideoId(std::string_view id) {
    if (id.size() != 11) return false;
    for (char c : id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    }
    return true;
}

std::string_view trimView(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string lowerAscii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

std::string youtubeVideoId(std::string_view raw) {
    std::string_view s = trimView(raw);
    if (s.empty() || s.size() > 500) return {};
    // scheme
    if (auto p = s.find("://"); p != std::string_view::npos) {
        std::string scheme = lowerAscii(s.substr(0, p));
        if (scheme != "https" && scheme != "http") return {};
        s.remove_prefix(p + 3);
    }
    auto slash = s.find('/');
    std::string host = lowerAscii(slash == std::string_view::npos ? s : s.substr(0, slash));
    if (auto at = host.find('@'); at != std::string::npos) return {};   // user@host: not a plain link
    if (auto colon = host.find(':'); colon != std::string::npos) host = host.substr(0, colon);
    std::string_view rest = slash == std::string_view::npos ? std::string_view{} : s.substr(slash);   // "/path?query#frag"
    std::string_view path = rest;
    std::string_view query;
    if (auto h = path.find('#'); h != std::string_view::npos) path = path.substr(0, h);
    if (auto q = path.find('?'); q != std::string_view::npos) {
        query = path.substr(q + 1);
        path = path.substr(0, q);
    }
    auto segment = [&](std::size_t n) -> std::string_view {
        std::string_view p = path;
        std::size_t i = 0;
        while (!p.empty()) {
            if (p.front() == '/') {
                p.remove_prefix(1);
                continue;
            }
            auto e = p.find('/');
            std::string_view seg = e == std::string_view::npos ? p : p.substr(0, e);
            if (i == n) return seg;
            ++i;
            if (e == std::string_view::npos) break;
            p.remove_prefix(e);
        }
        return {};
    };
    auto param = [&](std::string_view key) -> std::string_view {
        std::string_view q = query;
        while (!q.empty()) {
            auto amp = q.find('&');
            std::string_view kv = amp == std::string_view::npos ? q : q.substr(0, amp);
            auto eq = kv.find('=');
            if (eq != std::string_view::npos && kv.substr(0, eq) == key) return kv.substr(eq + 1);
            if (amp == std::string_view::npos) break;
            q.remove_prefix(amp + 1);
        }
        return {};
    };
    std::string_view id;
    if (host == "youtu.be" || host == "www.youtu.be") id = segment(0);
    else if (host == "youtube.com" || host == "www.youtube.com" || host == "m.youtube.com" || host == "music.youtube.com"
             || host == "youtube-nocookie.com" || host == "www.youtube-nocookie.com") {
        auto first = segment(0);
        if (first.empty() || first == "watch") id = param("v");
        else if (first == "shorts" || first == "live" || first == "embed" || first == "v") id = segment(1);
    }
    else return {};
    return validVideoId(id) ? std::string(id) : std::string();
}

json::Value linkRequestJson(ClipRecord const& r, std::string const& url, std::string const& clientBuild) {
    json::Value o = json::Value::object();
    o.set("attemptId", r.attemptId);
    o.set("sessionId", r.serverSessionId.empty() ? json::Value(nullptr) : json::Value(r.serverSessionId));
    o.set("levelId", r.levelId);
    o.set("url", std::string(trimView(url)));
    o.set("visibility", "private");
    o.set("clipId", r.clipId);
    o.set("clientBuild", clientBuild);
    return o;
}

std::string linkProblemText(std::string const& detailUrl, std::string const& serverMessage) {
    if (detailUrl == "youtube_private") return "the YouTube video is Private - set it to Unlisted or Public on YouTube, then send the link again";
    if (detailUrl == "youtube_unavailable") return "YouTube has no video at that link (deleted, still processing, or a typo) - check it and send again";
    if (detailUrl == "not_youtube") return "that is not a link to a YouTube video";
    return serverMessage.empty() ? std::string("the link was not accepted") : serverMessage;
}

VerificationHint parseVerificationHint(json::Value const& ackBody) {
    VerificationHint hint;
    auto const* member = ackBody.find("verificationRequests");
    if (!member) return hint;
    hint.known = true;
    if (!member->isArray()) return hint;   // null / anything else: "reviewed, nothing to verify"
    auto text = [](json::Value const* v, std::size_t cap) {
        std::string out;
        if (!v || !v->isString()) return out;
        for (char c : v->asString()) {
            if (out.size() >= cap) break;
            auto u = static_cast<unsigned char>(c);
            if (u >= 32 && u < 127) out += c;   // GD's bitmap fonts: printable ASCII only
        }
        return out;
    };
    for (auto const& item : member->asArray()) {
        if (hint.requests.size() >= kFlow.maxPreserveAttempts) break;
        if (!item.isObject()) continue;
        VerificationRequest q;
        q.attemptId = item.getString("attemptId");
        q.caseId = item.getString("caseId");
        if (q.attemptId.empty() || q.attemptId.size() > kFlow.maxAttemptIdLength) continue;
        if (q.caseId.empty() || q.caseId.size() > kFlow.maxAttemptIdLength) continue;
        bool dup = false;
        for (auto const& have : hint.requests) dup = dup || have.caseId == q.caseId;
        if (dup) continue;
        q.requestId = text(item.find("requestId"), kFlow.maxAttemptIdLength);
        q.reason = text(item.find("reason"), kFlow.maxPreserveReasonLength);
        q.progressStart = static_cast<int>(std::clamp<int64_t>(item.getInt("progressStart", 0), 0, 100));
        q.progressEnd = static_cast<int>(std::clamp<int64_t>(item.getInt("progressEnd", 100), 0, 100));
        hint.requests.push_back(std::move(q));
    }
    return hint;
}

// ---- hashing ----

bool hashFile(std::filesystem::path const& path, std::string& hexOut, int64_t& sizeOut, std::atomic<bool> const* cancel) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    crypto::Sha256 sha;
    std::vector<char> buf(kFlow.hashChunkBytes);
    int64_t total = 0;
    while (in) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        sha.update(buf.data(), static_cast<size_t>(got));
        total += got;
    }
    if (in.bad()) return false;
    hexOut = crypto::toHex(sha.finish());
    sizeOut = total;
    return true;
}

// ---- ids and names ----

std::string makeClipId(std::string const& attemptId, int64_t unixMs, uint32_t counter) {
    char ms[32];
    std::snprintf(ms, sizeof(ms), "%llx", static_cast<unsigned long long>(unixMs < 0 ? 0 : unixMs));
    std::string seed = attemptId + "|" + std::to_string(unixMs) + "|" + std::to_string(counter);
    std::string digest = crypto::toHex(crypto::sha256(seed));
    return std::string("clip-") + ms + "-" + digest.substr(0, 8);
}

bool validClipId(std::string_view id) {
    if (id.size() < kFlow.clipIdMin || id.size() > kFlow.clipIdMax) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
}

bool validSha256Hex(std::string_view hex) {
    if (hex.size() != 64) return false;
    return std::all_of(hex.begin(), hex.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string clipFileName(std::string_view levelName, std::string_view levelId, double percent, bool completed, int year, int month, int day,
                         int hour, int minute, int second) {
    std::string name;
    for (char c : levelName) {
        if (name.size() >= 60) break;
        bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-' || c == '(' || c == ')';
        name += ok ? c : '_';
    }
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (name.empty()) {
        name = "level ";
        for (char c : levelId) {
            if (name.size() < 30 && std::isalnum(static_cast<unsigned char>(c))) name += c;
        }
    }
    int pct = completed ? 100 : static_cast<int>(std::floor(std::clamp(percent, 0.0, 100.0)));
    char buf[96];
    std::snprintf(buf, sizeof(buf), " %dpct %04d-%02d-%02d %02d-%02d-%02d.mp4", pct, std::clamp(year, 0, 9999), std::clamp(month, 0, 99),
                  std::clamp(day, 0, 99), std::clamp(hour, 0, 99), std::clamp(minute, 0, 99), std::clamp(second, 0, 99));
    return "GPRL " + name + buf;
}

// ---- index ----

json::Value recordToJson(ClipRecord const& r) {
    json::Value o = json::Value::object();
    o.set("clipId", r.clipId);
    o.set("attemptId", r.attemptId);
    o.set("sessionLocalId", r.sessionLocalId);
    o.set("serverSessionId", r.serverSessionId);
    o.set("levelId", r.levelId);
    o.set("levelName", r.levelName);
    o.set("levelHash", r.levelHash);
    o.set("attemptNo", r.attemptNo);
    o.set("percent", r.percent);
    o.set("completed", r.completed);
    o.set("levelCounts", r.levelCounts);
    o.set("rule", r.rule);
    o.set("state", name(r.state));
    o.set("choiceMade", r.choiceMade);
    o.set("choice", name(r.choice));
    o.set("path", r.path);
    o.set("sha256", r.sha256);
    o.set("sizeBytes", r.sizeBytes);
    o.set("durationMs", r.durationMs);
    o.set("width", r.width);
    o.set("height", r.height);
    o.set("hasGameAudio", r.hasGameAudio);
    o.set("hasMicAudio", r.hasMicAudio);
    o.set("hasDesktopAudio", r.hasDesktopAudio);
    o.set("needsLink", r.needsLink);
    o.set("linkUrl", r.linkUrl);
    o.set("linkSent", r.linkSent);
    o.set("wholeAttempt", r.wholeAttempt);
    o.set("savedLocal", r.savedLocal);
    o.set("eventSent", r.eventSent);
    o.set("uploaded", r.uploaded);
    o.set("uploadId", r.uploadId);
    o.set("uploadTries", r.uploadTries);
    o.set("error", r.error);
    o.set("createdAtMs", r.createdAtMs);
    o.set("endT", r.endT);
    o.set("endTick", r.endTick);
    return o;
}

bool recordFromJson(json::Value const& v, ClipRecord& r) {
    if (!v.isObject()) return false;
    r = ClipRecord{};
    r.clipId = v.getString("clipId");
    r.attemptId = v.getString("attemptId");
    if (!validClipId(r.clipId) || r.attemptId.empty()) return false;
    if (!parse(v.getString("state"), r.state)) return false;
    if (!parse(v.getString("choice"), r.choice)) r.choice = Choice::Nothing;
    r.sessionLocalId = v.getString("sessionLocalId");
    r.serverSessionId = v.getString("serverSessionId");
    r.levelId = v.getString("levelId");
    r.levelName = v.getString("levelName");
    r.levelHash = v.getString("levelHash");
    r.attemptNo = static_cast<int>(v.getInt("attemptNo"));
    r.percent = v.getNumber("percent");
    r.completed = v.getBool("completed");
    r.levelCounts = v.getBool("levelCounts", true);
    r.rule = v.getString("rule");
    r.choiceMade = v.getBool("choiceMade");
    r.path = v.getString("path");
    r.sha256 = v.getString("sha256");
    r.sizeBytes = v.getInt("sizeBytes");
    r.durationMs = v.getNumber("durationMs");
    r.width = static_cast<int>(v.getInt("width"));
    r.height = static_cast<int>(v.getInt("height"));
    r.hasGameAudio = v.getBool("hasGameAudio");
    r.hasMicAudio = v.getBool("hasMicAudio");
    r.hasDesktopAudio = v.getBool("hasDesktopAudio");
    r.needsLink = v.getBool("needsLink");
    r.linkUrl = v.getString("linkUrl");
    r.linkSent = v.getBool("linkSent");
    r.wholeAttempt = v.getBool("wholeAttempt", true);
    r.savedLocal = v.getBool("savedLocal");
    r.eventSent = v.getBool("eventSent");
    r.uploaded = v.getBool("uploaded");
    r.uploadId = v.getString("uploadId");
    r.uploadTries = static_cast<int>(v.getInt("uploadTries"));
    r.error = v.getString("error");
    r.createdAtMs = v.getInt("createdAtMs");
    r.endT = v.getNumber("endT");
    r.endTick = v.getInt("endTick");
    return true;
}

std::string indexToText(std::vector<ClipRecord> const& records) {
    json::Value root = json::Value::object();
    root.set("version", kFlow.version);
    json::Value list = json::Value::array();
    for (auto const& r : records) list.push(recordToJson(r));
    root.set("clips", std::move(list));
    return json::stringifyPretty(root) + "\n";
}

std::vector<ClipRecord> indexFromText(std::string_view text) {
    std::vector<ClipRecord> out;
    json::Value root;
    if (!json::parse(text, root) || !root.isObject()) return out;
    auto const* clips = root.find("clips");
    if (!clips || !clips->isArray()) return out;
    for (auto const& v : clips->asArray()) {
        ClipRecord r;
        if (recordFromJson(v, r)) out.push_back(std::move(r));
    }
    return out;
}

std::vector<ClipRecord> trimIndex(std::vector<ClipRecord>& records) {
    std::vector<ClipRecord> dropped;
    // 1. too many prepared clips waiting for a choice: the oldest go (bounded disk use)
    auto waiting = [](ClipRecord const& r) { return r.state == ClipState::Ready; };
    size_t ready = static_cast<size_t>(std::count_if(records.begin(), records.end(), waiting));
    while (ready > static_cast<size_t>(kFlow.maxPendingClips)) {
        auto oldest = records.end();
        for (auto it = records.begin(); it != records.end(); ++it) {
            if (waiting(*it) && (oldest == records.end() || it->createdAtMs < oldest->createdAtMs)) oldest = it;
        }
        if (oldest == records.end()) break;
        dropped.push_back(std::move(*oldest));
        records.erase(oldest);
        --ready;
    }
    // 2. finished records: only the newest few are listed
    size_t done = static_cast<size_t>(std::count_if(records.begin(), records.end(), [](ClipRecord const& r) { return finished(r.state); }));
    while (done > static_cast<size_t>(kFlow.maxIndexRecords)) {
        auto oldest = records.end();
        for (auto it = records.begin(); it != records.end(); ++it) {
            if (finished(it->state) && (oldest == records.end() || it->createdAtMs < oldest->createdAtMs)) oldest = it;
        }
        if (oldest == records.end()) break;
        records.erase(oldest);
        --done;
    }
    return dropped;
}

// ---- text ----

std::string formatBytes(int64_t bytes) {
    char buf[48];
    double mb = static_cast<double>(std::max<int64_t>(bytes, 0)) / (1024.0 * 1024.0);
    if (mb < 1.0) std::snprintf(buf, sizeof(buf), "%.0f KB", mb * 1024.0);
    else if (mb < 100.0) std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
    else if (mb < 1024.0) std::snprintf(buf, sizeof(buf), "%.0f MB", mb);
    else std::snprintf(buf, sizeof(buf), "%.2f GB", mb / 1024.0);
    return buf;
}

std::string formatDuration(double milliseconds) {
    auto total = static_cast<int64_t>(std::llround(std::max(0.0, milliseconds) / 1000.0));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(total / 60), static_cast<long long>(total % 60));
    return buf;
}

std::string statusLine(ClipRecord const& r, double uploadFraction) {
    std::string head = "Clip: " + (r.levelName.empty() ? "level " + r.levelId : r.levelName);
    char pct[16];
    std::snprintf(pct, sizeof(pct), " %d%%", r.completed ? 100 : static_cast<int>(std::floor(std::clamp(r.percent, 0.0, 100.0))));
    head += pct;
    if (r.durationMs > 0.0) head += " " + formatDuration(r.durationMs);
    if (r.sizeBytes > 0) head += " " + formatBytes(r.sizeBytes);
    std::string tail;
    switch (r.state) {
        case ClipState::Preparing: tail = r.choiceMade ? std::string("preparing (") + label(r.choice) + " chosen)" : std::string("preparing..."); break;
        case ClipState::Ready: tail = r.error.empty() ? std::string("waiting for your choice") : "waiting for your choice (" + r.error + ")"; break;
        case ClipState::Saved:
            if (r.linkSent) tail = "saved on this computer; YouTube link sent to the moderators";
            else if (r.needsLink) tail = "saved on this computer - too long to upload: send a YouTube link (GPRL menu > Account)";
            else tail = r.error.empty() ? std::string("saved on this computer") : r.error;
            break;
        case ClipState::Uploading: {
            char up[32];
            std::snprintf(up, sizeof(up), "uploading %d%%", static_cast<int>(std::floor(std::clamp(uploadFraction, 0.0, 1.0) * 100.0)));
            tail = up;
            break;
        }
        case ClipState::Uploaded: tail = r.savedLocal ? "sent to GPRL moderators + saved" : "sent to GPRL moderators"; break;
        case ClipState::UploadFailed: tail = "upload failed: " + r.error; break;
        case ClipState::Discarded: tail = "discarded"; break;
        case ClipState::Failed: tail = "failed: " + r.error; break;
    }
    return head + " - " + tail;
}

}  // namespace gprl::clip
