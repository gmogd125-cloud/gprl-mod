// core/clip_flow host tests (geode v0.6.0, stream M3, MASTER §16 / SPEC §28-§31): the four-choice
// popup's state machine (and the invariant that nothing uploads without an explicit Send choice),
// the evidence upload contract (request body, session parsing, where the bearer token may go,
// retry rules), the streaming SHA-256 of a clip file, clip ids / file names and the clip index.
// The file-hash test writes its temporary files into the current directory (the test build folder)
// and removes them again.
#include "test_util.hpp"

#include "../core/clip_flow.hpp"
#include "../core/crypto.hpp"
#include "../core/json.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace gprl;
using namespace gprl::clip;

namespace {

std::string g_scratch = ".";

bool has(std::vector<Action> const& actions, Action a) { return std::find(actions.begin(), actions.end(), a) != actions.end(); }
bool contains(std::string const& text, char const* needle) { return text.find(needle) != std::string::npos; }

constexpr char const* kSha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";   // SHA-256("abc")

ClipRecord preparing() {
    ClipRecord r;
    r.clipId = "clip-19a0b1c2d3e-5f6a7b8c";
    r.attemptId = "s1-a7";
    r.sessionLocalId = "s1";
    r.serverSessionId = "0f0e0d0c-1111-2222-3333-444455556666";
    r.levelId = "10565740";
    r.levelName = "Bloodbath";
    r.levelHash = std::string(64, 'a');
    r.attemptNo = 2041;
    r.percent = 100.0;
    r.completed = true;
    r.rule = "local_completion";
    r.state = ClipState::Preparing;
    r.createdAtMs = 1790000000000;
    r.endT = 96.5;
    r.endTick = 23160;
    return r;
}

/// What the mod fills in when the clip thread finished.
void fillPrepared(ClipRecord& r) {
    r.path = "D:\\GD Clips\\clips\\pending\\clip-19a0b1c2d3e-5f6a7b8c.mp4";
    r.sha256 = kSha;
    r.sizeBytes = 78ll * 1024 * 1024;
    r.durationMs = 100500.0;
    r.width = 1280;
    r.height = 720;
    r.hasGameAudio = true;
}

ClipRecord ready() {
    ClipRecord r = preparing();
    fillPrepared(r);
    UploadGate gate;
    gate.connected = true;
    auto actions = onPrepared(r, true, "", gate);
    CHECK(r.state == ClipState::Ready && actions.size() == 1 && actions[0] == Action::Persist);
    return r;
}

UploadGate connected() {
    UploadGate g;
    g.connected = true;
    return g;
}

void testNames() {
    SECTION("FlowParams + choice / state / action names");
    CHECK(std::string(kFlow.version) == "gprl-clip-flow/1");
    CHECK(std::string(kFlow.uploadsPath) == "/v1/me/evidence/uploads");
    CHECK(kFlow.uploadAttempts == 3 && kFlow.maxPendingClips == 3);
    CHECK(kFlow.popupInputGuardSeconds == 1.0f);   // a click already on its way cannot pick a choice
    CHECK(kFlow.defaultMaxUploadBytes == 95ll * 1024 * 1024);
    // the four choices of MASTER §16, with the exact popup texts
    CHECK(std::string(label(Choice::SaveLocal)) == "Save to computer");
    CHECK(std::string(label(Choice::Send)) == "Send to GPRL moderators");
    CHECK(std::string(label(Choice::SaveAndSend)) == "Save + send");
    CHECK(std::string(label(Choice::Nothing)) == "Do nothing");
    for (Choice c : {Choice::Nothing, Choice::SaveLocal, Choice::Send, Choice::SaveAndSend}) {
        Choice back = Choice::Nothing;
        CHECK(parse(name(c), back) && back == c);
    }
    Choice bad;
    CHECK(!parse("upload", bad));
    CHECK(wantsSave(Choice::SaveLocal) && wantsSave(Choice::SaveAndSend) && !wantsSave(Choice::Send) && !wantsSave(Choice::Nothing));
    CHECK(wantsSend(Choice::Send) && wantsSend(Choice::SaveAndSend) && !wantsSend(Choice::SaveLocal) && !wantsSend(Choice::Nothing));
    for (ClipState s : {ClipState::Preparing, ClipState::Ready, ClipState::Saved, ClipState::Uploading, ClipState::Uploaded, ClipState::UploadFailed,
                        ClipState::Discarded, ClipState::Failed}) {
        ClipState back = ClipState::Failed;
        CHECK(parse(name(s), back) && back == s);
    }
    ClipState badState;
    CHECK(!parse("sent", badState));
    CHECK(finished(ClipState::Saved) && finished(ClipState::Uploaded) && finished(ClipState::Discarded) && finished(ClipState::Failed));
    CHECK(!finished(ClipState::Ready) && !finished(ClipState::Uploading) && !finished(ClipState::UploadFailed) && !finished(ClipState::Preparing));
    CHECK(std::string(name(Action::StartUpload)) == "start_upload");
}

void testFourChoices() {
    SECTION("Do nothing: the clip is deleted, nothing else happens");
    {
        ClipRecord r = ready();
        auto a = onChoice(r, Choice::Nothing, connected());
        CHECK(r.state == ClipState::Discarded && r.choiceMade && r.choice == Choice::Nothing);
        CHECK(has(a, Action::DeleteFile) && has(a, Action::Persist));
        CHECK(!has(a, Action::StartUpload) && !has(a, Action::EmitClipAvailable) && !has(a, Action::MoveToClipsFolder));
        CHECK(!awaitingChoice(r));
        // a finished record ignores further choices
        CHECK(onChoice(r, Choice::Send, connected()).empty() && r.state == ClipState::Discarded);
    }
    SECTION("Save to computer: moved to the clips folder, clip_available emitted, NO upload");
    {
        ClipRecord r = ready();
        auto a = onChoice(r, Choice::SaveLocal, connected());
        CHECK(r.state == ClipState::Saved && r.savedLocal && !r.uploaded);
        CHECK(a.size() == 3 && a[0] == Action::MoveToClipsFolder && a[1] == Action::EmitClipAvailable && a[2] == Action::Persist);
        CHECK(!has(a, Action::StartUpload));
        CHECK(onChoice(r, Choice::SaveAndSend, connected()).empty());   // final: a later Send needs a new clip
    }
    SECTION("Send to GPRL moderators: upload; on success the local file is deleted (no copy was asked for)");
    {
        ClipRecord r = ready();
        auto a = onChoice(r, Choice::Send, connected());
        CHECK(r.state == ClipState::Uploading && !r.savedLocal);
        CHECK(a.size() == 2 && a[0] == Action::StartUpload && a[1] == Action::Persist);
        CHECK(!has(a, Action::EmitClipAvailable));   // clip_available is for local saves
        CHECK(onChoice(r, Choice::Nothing, connected()).empty());   // nothing changes mid-upload
        auto done = onUploadResult(r, true, "");
        CHECK(r.state == ClipState::Uploaded && r.uploaded);
        CHECK(has(done, Action::DeleteFile) && has(done, Action::Persist));
        CHECK(onUploadResult(r, true, "").empty());   // a second result is ignored
    }
    SECTION("Save + send: saved first, then uploaded; the saved file stays");
    {
        ClipRecord r = ready();
        auto a = onChoice(r, Choice::SaveAndSend, connected());
        CHECK(r.state == ClipState::Uploading && r.savedLocal);
        CHECK(a.size() == 4 && a[0] == Action::MoveToClipsFolder && a[1] == Action::EmitClipAvailable && a[2] == Action::StartUpload && a[3] == Action::Persist);
        auto done = onUploadResult(r, true, "");
        CHECK(r.state == ClipState::Uploaded && r.uploaded && r.savedLocal);
        CHECK(!has(done, Action::DeleteFile));
    }
}

void testGate() {
    SECTION("upload gate: Send is refused (and says why) while it cannot work; Save still works");
    ClipRecord r = ready();
    UploadGate offline;                            // not connected
    CHECK(contains(uploadBlockReason(r, offline), "not connected"));
    auto a = onChoice(r, Choice::Send, offline);
    CHECK(r.state == ClipState::Ready && !r.choiceMade);     // the clip is untouched, the player can pick again
    CHECK(!has(a, Action::StartUpload) && !has(a, Action::DeleteFile));
    CHECK(contains(r.error, "not sent: not connected"));
    CHECK(awaitingChoice(r));

    UploadGate local = connected();
    local.localOnly = true;
    CHECK(contains(uploadBlockReason(r, local), "local-only"));

    UploadGate small = connected();
    small.maxBytes = 10ll * 1024 * 1024;
    CHECK(contains(uploadBlockReason(r, small), "78.0 MB") && contains(uploadBlockReason(r, small), "at most 10.0 MB"));

    ClipRecord notCounting = ready();
    notCounting.levelCounts = false;
    CHECK(contains(uploadBlockReason(notCounting, connected()), "does not count"));
    // v0.9.0: a run the server asked to verify is sent whatever the level is
    ClipRecord verification = ready();
    verification.levelCounts = false;
    verification.rule = "verification";
    CHECK(uploadBlockReason(verification, connected()).empty());

    ClipRecord noHash = ready();
    noHash.sha256 = "xyz";
    CHECK(contains(uploadBlockReason(noHash, connected()), "not ready"));
    CHECK(uploadBlockReason(ready(), connected()).empty());

    // Save + send with the gate closed: the save happens, the upload does not, and the record says so
    ClipRecord both = ready();
    a = onChoice(both, Choice::SaveAndSend, offline);
    CHECK(both.state == ClipState::Saved && both.savedLocal);
    CHECK(has(a, Action::MoveToClipsFolder) && has(a, Action::EmitClipAvailable) && !has(a, Action::StartUpload));
    CHECK(contains(both.error, "saved; not sent: not connected"));
}

void testChoiceWhilePreparing() {
    SECTION("a choice made while the clip is still being cut is applied when it is ready");
    ClipRecord r = preparing();
    CHECK(awaitingChoice(r));
    auto a = onChoice(r, Choice::SaveAndSend, connected());
    CHECK(a.empty());                                 // no file yet: nothing to move, nothing to upload
    CHECK(r.state == ClipState::Preparing && r.choiceMade && r.choice == Choice::SaveAndSend);
    CHECK(!awaitingChoice(r));
    fillPrepared(r);
    a = onPrepared(r, true, "", connected());
    CHECK(r.state == ClipState::Uploading && r.savedLocal);
    CHECK(has(a, Action::MoveToClipsFolder) && has(a, Action::EmitClipAvailable) && has(a, Action::StartUpload));

    ClipRecord nothing = preparing();
    onChoice(nothing, Choice::Nothing, connected());
    fillPrepared(nothing);
    a = onPrepared(nothing, true, "", connected());
    CHECK(nothing.state == ClipState::Discarded && has(a, Action::DeleteFile) && !has(a, Action::StartUpload));

    SECTION("the clip could not be made: Failed, whatever was chosen");
    ClipRecord failed = preparing();
    onChoice(failed, Choice::Send, connected());
    a = onPrepared(failed, false, "ffmpeg could not write the clip", connected());
    CHECK(failed.state == ClipState::Failed && failed.error == "ffmpeg could not write the clip");
    CHECK(has(a, Action::DeleteFile) && !has(a, Action::StartUpload));
    CHECK(!awaitingChoice(failed));
    ClipRecord failed2 = preparing();
    onPrepared(failed2, false, "", connected());
    CHECK(!failed2.error.empty());
    // onPrepared on a record that is not preparing does nothing
    ClipRecord r2 = ready();
    CHECK(onPrepared(r2, true, "", connected()).empty() && r2.state == ClipState::Ready);
}

void testUploadFailure() {
    SECTION("a failed upload keeps the file; Retry sends again, Do nothing gives up");
    ClipRecord r = ready();
    onChoice(r, Choice::Send, connected());
    auto a = onUploadResult(r, false, "network error");
    CHECK(r.state == ClipState::UploadFailed && r.error == "network error" && !r.uploaded);
    CHECK(!has(a, Action::DeleteFile));              // the evidence is never lost to a network error
    CHECK(awaitingChoice(r));
    // retry with the gate closed: stays failed and explains
    UploadGate offline;
    a = onRetryUpload(r, offline);
    CHECK(r.state == ClipState::UploadFailed && !has(a, Action::StartUpload) && contains(r.error, "not connected"));
    a = onRetryUpload(r, connected());
    CHECK(r.state == ClipState::Uploading && has(a, Action::StartUpload));
    onUploadResult(r, false, "");
    CHECK(r.state == ClipState::UploadFailed && !r.error.empty());
    // Send chosen again from the popup = retry
    a = onChoice(r, Choice::Send, connected());
    CHECK(r.state == ClipState::Uploading && has(a, Action::StartUpload));
    onUploadResult(r, false, "HTTP 503");
    // Do nothing on a Send-only clip: deleted
    a = onChoice(r, Choice::Nothing, connected());
    CHECK(r.state == ClipState::Discarded && has(a, Action::DeleteFile));

    ClipRecord saved = ready();
    onChoice(saved, Choice::SaveAndSend, connected());
    onUploadResult(saved, false, "HTTP 503");
    // Do nothing on a Save + send clip: the saved file stays, only the upload is given up
    a = onChoice(saved, Choice::Nothing, connected());
    CHECK(saved.state == ClipState::Saved && saved.savedLocal && !has(a, Action::DeleteFile));

    ClipRecord keep = ready();
    onChoice(keep, Choice::Send, connected());
    onUploadResult(keep, false, "HTTP 503");
    // changed their mind: keep a copy instead of sending
    a = onChoice(keep, Choice::SaveLocal, connected());
    CHECK(keep.state == ClipState::Saved && keep.savedLocal && has(a, Action::MoveToClipsFolder) && has(a, Action::EmitClipAvailable));
    CHECK(!has(a, Action::StartUpload));

    SECTION("a failed upload is offered again with all four choices (the popup of the Account tab)");
    // Save + send after a failed Send: the copy is kept first, then the retry starts
    ClipRecord both = ready();
    onChoice(both, Choice::Send, connected());
    onUploadResult(both, false, "this GPRL server does not accept evidence uploads yet");
    a = onChoice(both, Choice::SaveAndSend, connected());
    CHECK(both.state == ClipState::Uploading && both.savedLocal && both.choiceMade && both.choice == Choice::SaveAndSend);
    CHECK(a.size() == 4 && a[0] == Action::MoveToClipsFolder && a[1] == Action::EmitClipAvailable && a[2] == Action::StartUpload && a[3] == Action::Persist);
    auto done = onUploadResult(both, true, "");
    CHECK(both.state == ClipState::Uploaded && !has(done, Action::DeleteFile));   // the saved copy stays
    // ... with the gate closed: saved, the upload stays failed and says why; nothing is sent
    ClipRecord blocked = ready();
    onChoice(blocked, Choice::Send, connected());
    onUploadResult(blocked, false, "HTTP 503");
    a = onChoice(blocked, Choice::SaveAndSend, offline);
    CHECK(blocked.state == ClipState::UploadFailed && blocked.savedLocal);
    CHECK(has(a, Action::MoveToClipsFolder) && has(a, Action::EmitClipAvailable) && has(a, Action::Persist) && !has(a, Action::StartUpload));
    CHECK_MSG(contains(blocked.error, "saved; not sent: not connected"), blocked.error);
    // then "Do nothing" only gives up the upload: the saved file is not deleted
    a = onChoice(blocked, Choice::Nothing, connected());
    CHECK(blocked.state == ClipState::Saved && !has(a, Action::DeleteFile));
    // Save + send on a clip that is already saved does not move / announce it a second time
    ClipRecord twice = ready();
    onChoice(twice, Choice::SaveAndSend, connected());
    onUploadResult(twice, false, "HTTP 503");
    a = onChoice(twice, Choice::SaveAndSend, connected());
    CHECK(twice.state == ClipState::Uploading && a.size() == 2 && a[0] == Action::StartUpload && a[1] == Action::Persist);
    // plain Send on a saved clip keeps it saved after the upload
    onUploadResult(twice, false, "HTTP 503");
    a = onChoice(twice, Choice::Send, connected());
    CHECK(twice.state == ClipState::Uploading && twice.savedLocal && twice.choice == Choice::SaveAndSend);
    CHECK(!has(onUploadResult(twice, true, ""), Action::DeleteFile));
}

/// MASTER §16 / SPEC §28 "never automatic upload": exhaustive walk over every sequence of events up
/// to a depth - StartUpload may only appear when the record carries an explicit Send / Save + send.
void testNeverAutomaticUpload() {
    SECTION("no event sequence without an explicit Send choice ever starts an upload");
    enum class Ev { PreparedOk, PreparedFail, ChooseNothing, ChooseSave, UploadOk, UploadFail, Retry, Restart };
    const Ev events[] = {Ev::PreparedOk, Ev::PreparedFail, Ev::ChooseNothing, Ev::ChooseSave, Ev::UploadOk, Ev::UploadFail, Ev::Retry, Ev::Restart};
    int sequences = 0;
    int uploads = 0;
    // depth 5 over 8 events = 32768 sequences; none of them contains a Send / Save + send choice
    for (int code = 0; code < 8 * 8 * 8 * 8 * 8; ++code) {
        ClipRecord r = preparing();
        int c = code;
        for (int step = 0; step < 5; ++step) {
            Ev ev = events[c % 8];
            c /= 8;
            std::vector<Action> a;
            switch (ev) {
                case Ev::PreparedOk:
                    if (r.state == ClipState::Preparing) fillPrepared(r);
                    a = onPrepared(r, true, "", connected());
                    break;
                case Ev::PreparedFail: a = onPrepared(r, false, "x", connected()); break;
                case Ev::ChooseNothing: a = onChoice(r, Choice::Nothing, connected()); break;
                case Ev::ChooseSave: a = onChoice(r, Choice::SaveLocal, connected()); break;
                case Ev::UploadOk: a = onUploadResult(r, true, ""); break;
                case Ev::UploadFail: a = onUploadResult(r, false, "x"); break;
                case Ev::Retry: a = onRetryUpload(r, connected()); break;
                case Ev::Restart: reconcileAfterRestart(r, !r.path.empty()); break;
            }
            if (has(a, Action::StartUpload) || r.state == ClipState::Uploading || r.uploaded) ++uploads;
        }
        ++sequences;
    }
    CHECK(sequences == 32768);
    CHECK_MSG(uploads == 0, "an upload started without a Send choice");

    SECTION("with a Send choice in the sequence, an upload only ever starts at or after it");
    const Choice sendChoices[] = {Choice::Send, Choice::SaveAndSend};
    int started = 0;
    for (Choice send : sendChoices) {
        for (int at = 0; at < 4; ++at) {
            for (int code = 0; code < 8 * 8 * 8; ++code) {
                ClipRecord r = preparing();
                int c = code;
                bool sendSeen = false;
                for (int step = 0; step < 4; ++step) {
                    std::vector<Action> a;
                    if (step == at) {
                        a = onChoice(r, send, connected());
                        // the choice only counts when the flow accepted it (Preparing / Ready / UploadFailed)
                        if (r.choiceMade && wantsSend(r.choice)) sendSeen = true;
                    }
                    else {
                        Ev ev = events[c % 8];
                        c /= 8;
                        switch (ev) {
                            case Ev::PreparedOk:
                                if (r.state == ClipState::Preparing) fillPrepared(r);
                                a = onPrepared(r, true, "", connected());
                                break;
                            case Ev::PreparedFail: a = onPrepared(r, false, "x", connected()); break;
                            case Ev::ChooseNothing: a = onChoice(r, Choice::Nothing, connected()); break;
                            case Ev::ChooseSave: a = onChoice(r, Choice::SaveLocal, connected()); break;
                            case Ev::UploadOk: a = onUploadResult(r, true, ""); break;
                            case Ev::UploadFail: a = onUploadResult(r, false, "x"); break;
                            case Ev::Retry: a = onRetryUpload(r, connected()); break;
                            case Ev::Restart: reconcileAfterRestart(r, !r.path.empty()); break;
                        }
                    }
                    if (has(a, Action::StartUpload)) {
                        ++started;
                        CHECK_MSG(sendSeen, "StartUpload before any Send choice");
                        CHECK(r.choiceMade && wantsSend(r.choice));
                    }
                }
            }
        }
    }
    CHECK(started > 0);   // the walk did reach uploads

    SECTION("a gate that is closed never lets an upload start, whatever is chosen");
    UploadGate offline;
    for (Choice c : {Choice::Nothing, Choice::SaveLocal, Choice::Send, Choice::SaveAndSend}) {
        ClipRecord r = ready();
        auto a = onChoice(r, c, offline);
        CHECK(!has(a, Action::StartUpload) && r.state != ClipState::Uploading);
        ClipRecord early = preparing();
        onChoice(early, c, offline);
        fillPrepared(early);
        a = onPrepared(early, true, "", offline);
        CHECK(!has(a, Action::StartUpload) && early.state != ClipState::Uploading);
    }
    // onRetryUpload cannot be used to upload a clip that was only saved
    ClipRecord saved = ready();
    onChoice(saved, Choice::SaveLocal, connected());
    CHECK(onRetryUpload(saved, connected()).empty() && saved.state == ClipState::Saved);

    // Verifier (stream M3): the walks above start from a fresh Preparing record. clips.json is a
    // file on disk, so a stored record can hold ANY combination of state / choiceMade / choice /
    // savedLocal. The mod reconciles every stored record at start (Clipper::init).
    SECTION("from ANY stored record: no upload without a Send the record already carries, and none at all without a press");
    const ClipState states[] = {ClipState::Preparing, ClipState::Ready,        ClipState::Saved,     ClipState::Uploading,
                                ClipState::Uploaded,  ClipState::UploadFailed, ClipState::Discarded, ClipState::Failed};
    const Choice allChoices[] = {Choice::Nothing, Choice::SaveLocal, Choice::Send, Choice::SaveAndSend};
    int walks = 0, reached = 0, withoutSend = 0, silent = 0;
    for (ClipState st : states) {
        for (int made = 0; made < 2; ++made) {
            for (Choice stored : allChoices) {
                for (int savedLocal = 0; savedLocal < 2; ++savedLocal) {
                    for (int withRetry = 0; withRetry < 2; ++withRetry) {
                        // withRetry 1: the 8 events (Retry is a press, legal only after a Send);
                        // withRetry 0: the 7 events that need no press on a Send / Retry button
                        int base = withRetry ? 8 : 7;
                        for (int code = 0; code < base * base * base * base; ++code) {
                            ClipRecord r = preparing();
                            fillPrepared(r);
                            r.state = st;
                            r.choiceMade = made != 0;
                            r.choice = stored;
                            r.savedLocal = savedLocal != 0;
                            bool priorSend = r.choiceMade && wantsSend(r.choice);
                            reconcileAfterRestart(r, true);
                            int c = code;
                            for (int step = 0; step < 4; ++step) {
                                Ev ev = events[c % base];
                                c /= base;
                                if (!withRetry && ev == Ev::Retry) ev = Ev::Restart;   // base 7: index 6 = Restart
                                std::vector<Action> a;
                                switch (ev) {
                                    case Ev::PreparedOk: a = onPrepared(r, true, "", connected()); break;
                                    case Ev::PreparedFail: a = onPrepared(r, false, "x", connected()); break;
                                    case Ev::ChooseNothing: a = onChoice(r, Choice::Nothing, connected()); break;
                                    case Ev::ChooseSave: a = onChoice(r, Choice::SaveLocal, connected()); break;
                                    case Ev::UploadOk: a = onUploadResult(r, true, ""); break;
                                    case Ev::UploadFail: a = onUploadResult(r, false, "x"); break;
                                    case Ev::Retry: a = onRetryUpload(r, connected()); break;
                                    case Ev::Restart: reconcileAfterRestart(r, true); break;
                                }
                                if (has(a, Action::StartUpload)) {
                                    ++reached;
                                    if (!priorSend) ++withoutSend;
                                    if (!withRetry) ++silent;
                                }
                            }
                            ++walks;
                        }
                    }
                }
            }
        }
    }
    CHECK(walks == 8 * 2 * 4 * 2 * (4096 + 2401));
    CHECK(reached > 0);   // a stored Send + the Retry press does upload
    CHECK_MSG(withoutSend == 0, "an upload started from a stored record that never carried a Send choice");
    CHECK_MSG(silent == 0, "an upload started with no Send / Retry press after a restart");
}

void testRestart() {
    SECTION("after a restart: interrupted work is reported, missing files end the record");
    ClipRecord prep = preparing();
    reconcileAfterRestart(prep, false);
    CHECK(prep.state == ClipState::Failed && contains(prep.error, "closed while the clip was being made"));

    ClipRecord up = ready();
    onChoice(up, Choice::Send, connected());
    reconcileAfterRestart(up, true);
    CHECK(up.state == ClipState::UploadFailed && contains(up.error, "closed during the upload"));
    CHECK(awaitingChoice(up));                       // offered again: Retry / Do nothing
    // and the retry is allowed (the Send choice was explicit)
    CHECK(has(onRetryUpload(up, connected()), Action::StartUpload));

    ClipRecord gone = ready();
    onChoice(gone, Choice::Send, connected());
    reconcileAfterRestart(gone, false);
    CHECK(gone.state == ClipState::Failed && gone.path.empty());

    ClipRecord waiting = ready();
    reconcileAfterRestart(waiting, true);
    CHECK(waiting.state == ClipState::Ready);         // still waiting for the choice
    reconcileAfterRestart(waiting, false);
    CHECK(waiting.state == ClipState::Failed);

    ClipRecord saved = ready();
    onChoice(saved, Choice::SaveLocal, connected());
    reconcileAfterRestart(saved, false);              // the player moved their file: still "saved"
    CHECK(saved.state == ClipState::Saved);
}

void testUploadRequest() {
    SECTION("POST /v1/me/evidence/uploads body (contracts.ts V1EvidenceUploadRequest)");
    ClipRecord r = ready();
    r.hasMicAudio = true;
    r.wholeAttempt = false;
    json::Value body = uploadRequestJson(r, "gprl-geode 0.6.0+win");
    CHECK(body.getString("clipId") == r.clipId);
    CHECK(body.getString("attemptId") == "s1-a7");
    CHECK(body.getString("sessionId") == r.serverSessionId);
    CHECK(body.getString("levelId") == "10565740" && body.getString("levelHash") == r.levelHash);
    CHECK(body.getString("kind") == "video" && body.getString("contentType") == "video/mp4");
    CHECK(body.getInt("sizeBytes") == 78ll * 1024 * 1024);
    CHECK(body.getString("sha256") == kSha);
    CHECK(body.getNumber("durationMs") == 100500.0);
    CHECK(body.getInt("width") == 1280 && body.getInt("height") == 720);
    CHECK(body.getBool("hasGameAudio") && body.getBool("hasMicAudio") && !body.getBool("wholeAttempt", true));
    // v0.9.0: one entry per audio track in the mux order (Game, Microphone, Desktop)
    CHECK(!body.getBool("hasDesktopAudio", true));
    CHECK(body["audioTracks"].isArray() && body["audioTracks"].asArray().size() == 2);
    CHECK(body["audioTracks"].asArray()[0].getString("name") == "Game" && body["audioTracks"].asArray()[0].getString("kind") == "game");
    CHECK(body["audioTracks"].asArray()[1].getInt("index") == 1 && body["audioTracks"].asArray()[1].getString("kind") == "mic");
    {
        ClipRecord three = r;
        three.hasDesktopAudio = true;
        json::Value b3 = uploadRequestJson(three, "gprl-geode 0.9.0+win");
        CHECK(b3.getBool("hasDesktopAudio") && b3["audioTracks"].asArray().size() == 3);
        CHECK(b3["audioTracks"].asArray()[2].getInt("index") == 2 && b3["audioTracks"].asArray()[2].getString("name") == "Desktop"
              && b3["audioTracks"].asArray()[2].getString("kind") == "desktop");
        three.hasMicAudio = false;
        json::Value b2 = uploadRequestJson(three, "gprl-geode 0.9.0+win");
        CHECK(b2["audioTracks"].asArray().size() == 2 && b2["audioTracks"].asArray()[1].getInt("index") == 1
              && b2["audioTracks"].asArray()[1].getString("kind") == "desktop");
    }
    CHECK(body.getString("visibility") == "private");            // always private from the mod (SPEC §29)
    CHECK(body.getString("clientBuild") == "gprl-geode 0.6.0+win");
    CHECK(body.asObject().size() == 19);
    // exactly the keys of contracts.ts V1EvidenceUploadRequest, in this order (the 17 of v0.6.0 + the v0.9.0
    // hasDesktopAudio / audioTracks after hasMicAudio; shared/test/evidence-contract.test.ts pins the 17)
    const char* keys[] = {"clipId", "attemptId", "sessionId", "levelId", "levelHash", "kind", "contentType", "sizeBytes", "sha256",
                          "durationMs", "width", "height", "hasGameAudio", "hasMicAudio", "hasDesktopAudio", "audioTracks",
                          "wholeAttempt", "visibility", "clientBuild"};
    for (size_t i = 0; i < 19; ++i) CHECK_MSG(body.asObject()[i].first == keys[i], keys[i]);
    // an attempt that ran while not connected has no server session: null, not ""
    r.serverSessionId.clear();
    body = uploadRequestJson(r, "b");
    CHECK(body.has("sessionId") && body["sessionId"].isNull());
    std::string canonical = json::canonical(body);
    CHECK(contains(canonical, "\"sessionId\":null") && contains(canonical, "\"visibility\":\"private\""));
    // nothing about the player's machine leaves with the request
    CHECK(!contains(canonical, "GD Clips") && !contains(canonical, "path"));
}

json::Value parseJson(char const* text) {
    json::Value v;
    json::ParseError err;
    bool ok = json::parse(text, v, &err);
    CHECK_MSG(ok, "json parse: " + err.message);
    return v;
}

void testUploadSession() {
    SECTION("upload session parsing (V1EvidenceUploadResponse)");
    UploadSession s;
    std::string err;
    CHECK(parseUploadSession(parseJson(R"({"uploadId":"up-1","uploadUrl":"/v1/me/evidence/uploads/up-1","expiresAt":"2026-09-30T05:00:00Z","maxBytes":104857600,
        "headers":{"Content-Type":"video/mp4","x-amz-meta-clip":"clip-1"}})"), s, &err));
    CHECK(s.uploadId == "up-1" && s.uploadUrl == "/v1/me/evidence/uploads/up-1" && s.maxBytes == 104857600 && s.headers.size() == 2);
    CHECK(s.headers[0].first == "Content-Type" && s.headers[1].second == "clip-1");
    CHECK(parseUploadSession(parseJson(R"({"uploadId":"up-2","uploadUrl":"https://bucket.example/x"})"), s, &err));
    CHECK(s.maxBytes == 0 && s.headers.empty() && s.expiresAt.empty());
    CHECK(!parseUploadSession(parseJson(R"({"uploadUrl":"/x"})"), s, &err) && contains(err, "uploadId"));
    CHECK(!parseUploadSession(parseJson(R"({"uploadId":"u"})"), s, &err) && contains(err, "uploadUrl"));
    CHECK(!parseUploadSession(parseJson(R"([1,2])"), s, &err));
    CHECK(!parseUploadSession(parseJson(R"({"uploadId":"u","uploadUrl":"/x","headers":{"a":1}})"), s, &err));
    CHECK(parseUploadSession(parseJson(R"({"uploadId":"u","uploadUrl":"/x","maxBytes":-5})"), s, &err) && s.maxBytes == 0);
}

void testPreserveHint() {
    SECTION("preserveEvidence hint: absent = old server (local rule), present = the server decides");
    auto none = parsePreserveHint(parseJson(R"({"accepted":true,"nextSeq":4,"eventCount":12})"));
    CHECK(!none.known && none.attemptIds.empty() && none.reason.empty());
    // present but empty / null: the server evaluated and named nothing
    auto empty = parsePreserveHint(parseJson(R"({"accepted":true,"preserveEvidence":{"attemptIds":[]}})"));
    CHECK(empty.known && empty.attemptIds.empty());
    auto null = parsePreserveHint(parseJson(R"({"accepted":true,"preserveEvidence":null})"));
    CHECK(null.known && null.attemptIds.empty());
    auto named = parsePreserveHint(parseJson(R"({"preserveEvidence":{"attemptIds":["s1-a7","s1-a9"],"reason":"completion of a 250+ sigma/s level"}})"));
    CHECK(named.known && named.attemptIds.size() == 2 && named.attemptIds[0] == "s1-a7" && named.attemptIds[1] == "s1-a9");
    CHECK(named.reason == "completion of a 250+ sigma/s level");

    SECTION("preserveEvidence hint is untrusted input: junk is skipped, sizes are capped");
    auto junk = parsePreserveHint(parseJson(R"({"preserveEvidence":{"attemptIds":["ok-1",7,null,"",{"a":1},"ok-1","ok-2"],"reason":42}})"));
    CHECK(junk.known && junk.attemptIds.size() == 2 && junk.attemptIds[0] == "ok-1" && junk.attemptIds[1] == "ok-2");   // no duplicates
    CHECK(junk.reason.empty());
    auto wrong = parsePreserveHint(parseJson(R"({"preserveEvidence":{"attemptIds":"s1-a7"}})"));
    CHECK(wrong.known && wrong.attemptIds.empty());
    auto text = parsePreserveHint(parseJson(R"({"preserveEvidence":"yes"})"));
    CHECK(text.known && text.attemptIds.empty());
    std::string many = R"({"preserveEvidence":{"attemptIds":[)";
    for (int i = 0; i < 40; ++i) many += (i ? ",\"a" : "\"a") + std::to_string(i) + "\"";
    many += R"(],"reason":")" + std::string(500, 'r') + R"("}})";
    auto capped = parsePreserveHint(parseJson(many.c_str()));
    CHECK(capped.attemptIds.size() == kFlow.maxPreserveAttempts && capped.attemptIds.size() == 8 && capped.attemptIds[7] == "a7");
    CHECK(capped.reason.size() == 160);
    std::string longId = R"({"preserveEvidence":{"attemptIds":[")" + std::string(513, 'x') + R"(","fine"]}})";
    auto longOne = parsePreserveHint(parseJson(longId.c_str()));
    CHECK(longOne.attemptIds.size() == 1 && longOne.attemptIds[0] == "fine");
    // the reason is reduced to printable ASCII (GD's bitmap fonts; no control characters in a notification)
    auto odd = parsePreserveHint(parseJson(R"({"preserveEvidence":{"attemptIds":[],"reason":"line\nbreak \u00e9t\u00e9 \u2013 ok"}})"));
    CHECK_MSG(odd.reason == "linebreak t  ok", odd.reason);
}

void testVerificationHint() {
    SECTION("verificationRequests (v0.9.0): absent = old server, present = the server reviews runs");
    auto none = parseVerificationHint(parseJson(R"({"accepted":true,"nextSeq":4,"eventCount":12})"));
    CHECK(!none.known && none.requests.empty());
    auto empty = parseVerificationHint(parseJson(R"({"accepted":true,"verificationRequests":[]})"));
    CHECK(empty.known && empty.requests.empty());
    auto null = parseVerificationHint(parseJson(R"({"accepted":true,"verificationRequests":null})"));
    CHECK(null.known && null.requests.empty());
    auto one = parseVerificationHint(parseJson(
        R"({"verificationRequests":[{"requestId":"req-1","caseId":"case-1","attemptId":"s1-a7","reason":"completion on attempt 3","progressStart":0,"progressEnd":100,"openedAt":"2026-10-01T10:00:00Z"}]})"));
    CHECK(one.known && one.requests.size() == 1);
    CHECK(one.requests[0].requestId == "req-1" && one.requests[0].caseId == "case-1" && one.requests[0].attemptId == "s1-a7");
    CHECK(one.requests[0].reason == "completion on attempt 3" && one.requests[0].progressStart == 0 && one.requests[0].progressEnd == 100);
    // no queue row yet: requestId null
    auto noRow = parseVerificationHint(parseJson(R"({"verificationRequests":[{"requestId":null,"caseId":"case-2","attemptId":"s1-a8","reason":null,"progressStart":0,"progressEnd":71}]})"));
    CHECK(noRow.requests.size() == 1 && noRow.requests[0].requestId.empty() && noRow.requests[0].reason.empty() && noRow.requests[0].progressEnd == 71);

    SECTION("verificationRequests is untrusted input: junk is skipped, duplicates and sizes are capped");
    auto junk = parseVerificationHint(parseJson(
        R"({"verificationRequests":[7,null,"x",{"caseId":"c","attemptId":""},{"caseId":"","attemptId":"a"},{"caseId":"c1","attemptId":"a1","reason":"li\nne \u00e9","progressStart":-4,"progressEnd":900},{"caseId":"c1","attemptId":"a2"}]})"));
    CHECK(junk.known && junk.requests.size() == 1 && junk.requests[0].attemptId == "a1");
    CHECK_MSG(junk.requests[0].reason == "line ", junk.requests[0].reason);
    CHECK(junk.requests[0].progressStart == 0 && junk.requests[0].progressEnd == 100);
    std::string many = R"({"verificationRequests":[)";
    for (int i = 0; i < 40; ++i) many += std::string(i ? "," : "") + R"({"caseId":"c)" + std::to_string(i) + R"(","attemptId":"a)" + std::to_string(i) + R"("})";
    many += "]}";
    auto capped = parseVerificationHint(parseJson(many.c_str()));
    CHECK(capped.requests.size() == kFlow.maxPreserveAttempts && capped.requests[7].attemptId == "a7");
    auto wrong = parseVerificationHint(parseJson(R"({"verificationRequests":{"caseId":"c","attemptId":"a"}})"));
    CHECK(wrong.known && wrong.requests.empty());
}

void testYouTubeLinks() {
    SECTION("v0.10.0: the YouTube video id of the forms players paste");
    for (char const* url : {"https://www.youtube.com/watch?v=dQw4w9WgXcQ", "https://youtube.com/watch?v=dQw4w9WgXcQ&t=12s&list=PLx",
                            "https://m.youtube.com/watch?feature=share&v=dQw4w9WgXcQ", "https://youtu.be/dQw4w9WgXcQ", "https://youtu.be/dQw4w9WgXcQ?si=abc",
                            "youtu.be/dQw4w9WgXcQ", "www.youtube.com/watch?v=dQw4w9WgXcQ", "https://www.youtube.com/shorts/dQw4w9WgXcQ",
                            "https://www.youtube.com/live/dQw4w9WgXcQ?feature=share", "https://www.youtube.com/embed/dQw4w9WgXcQ",
                            "  https://www.youtube.com/watch?v=dQw4w9WgXcQ  ", "HTTPS://WWW.YOUTUBE.COM/watch?v=dQw4w9WgXcQ#t=5"}) {
        CHECK_MSG(youtubeVideoId(url) == "dQw4w9WgXcQ", url);
    }
    for (char const* url : {"", "not a link", "https://example.com/watch?v=dQw4w9WgXcQ", "https://vimeo.com/12345", "https://www.youtube.com/",
                            "https://www.youtube.com/watch", "https://www.youtube.com/watch?v=short", "https://www.youtube.com/channel/UCabc",
                            "https://www.youtube.com/playlist?list=PLx", "ftp://youtu.be/dQw4w9WgXcQ", "https://evil.youtube.com.example/watch?v=dQw4w9WgXcQ",
                            "https://user@youtu.be/dQw4w9WgXcQ"}) {
        CHECK_MSG(youtubeVideoId(url).empty(), url);
    }

    SECTION("v0.10.0: the upload cap in seconds, and a clip over it");
    // 720p 5000 kbit/s + 3 tracks of 160 kbit/s = 5480 kbit/s: 95 MiB lasts about 145 s
    double seconds = maxUploadSeconds(5000, 3, 160, 95ll * 1024 * 1024);
    CHECK_MSG(seconds > 144.0 && seconds < 146.0, std::to_string(seconds));
    CHECK(maxUploadSeconds(2500, 0, 160, 95ll * 1024 * 1024) > 300.0);
    CHECK(maxUploadSeconds(5000, 1, 160, 0) == 0.0);
    ClipRecord big = ready();
    UploadGate g = connected();
    big.sizeBytes = g.maxBytes + 1;
    CHECK(overUploadCap(big, g));
    big.sizeBytes = g.maxBytes;
    CHECK(!overUploadCap(big, g));

    SECTION("v0.10.0: the link request body (contracts.ts V1EvidenceLinkRequest) and the status line");
    ClipRecord r = ready();
    json::Value body = linkRequestJson(r, "  https://youtu.be/dQw4w9WgXcQ ", "gprl-geode 0.10.0+win");
    CHECK(body.getString("attemptId") == r.attemptId && body.getString("sessionId") == r.serverSessionId && body.getString("levelId") == r.levelId);
    CHECK(body.getString("url") == "https://youtu.be/dQw4w9WgXcQ" && body.getString("visibility") == "private");
    CHECK(body.getString("clipId") == r.clipId && body.getString("clientBuild") == "gprl-geode 0.10.0+win");
    CHECK(body.asObject().size() == 7);
    r.serverSessionId.clear();
    CHECK(linkRequestJson(r, "x", "b")["sessionId"].isNull());
    ClipRecord saved = ready();
    saved.state = ClipState::Saved;
    saved.needsLink = true;
    CHECK(contains(statusLine(saved, 0.0), "send a YouTube link"));
    saved.linkSent = true;
    saved.linkUrl = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    CHECK(contains(statusLine(saved, 0.0), "YouTube link sent"));
    // the index keeps the link fields
    auto back = indexFromText(indexToText({saved}));
    CHECK(back.size() == 1 && back[0].needsLink && back[0].linkSent && back[0].linkUrl == saved.linkUrl);
    CHECK(contains(linkProblemText("youtube_private", ""), "Unlisted or Public"));
    CHECK(contains(linkProblemText("youtube_unavailable", ""), "no video"));
    CHECK(linkProblemText("", "server says no") == "server says no");
}

void testPutPlan() {
    SECTION("PUT plan: the device token only ever goes to the API's own origin");
    const std::string api = "https://gprl-api.gprl.workers.dev";
    UploadSession s;
    s.uploadId = "up-1";

    s.uploadUrl = "/v1/me/evidence/uploads/up-1";
    auto plan = planPut(api, s);
    CHECK(plan.ok && plan.sendBearer && plan.url == "https://gprl-api.gprl.workers.dev/v1/me/evidence/uploads/up-1");
    plan = planPut(api + "/", s);
    CHECK(plan.ok && plan.url == "https://gprl-api.gprl.workers.dev/v1/me/evidence/uploads/up-1");

    s.uploadUrl = "https://GPRL-API.gprl.workers.dev:443/v1/me/evidence/uploads/up-1";
    plan = planPut(api, s);
    CHECK(plan.ok && plan.sendBearer);                         // same origin (case, default port)

    s.uploadUrl = "https://abc123.r2.cloudflarestorage.com/gprl-evidence/evidence/x?X-Amz-Signature=abc";
    plan = planPut(api, s);
    CHECK(plan.ok && !plan.sendBearer && plan.url == s.uploadUrl);   // presigned storage URL: no Authorization header

    // look-alike hosts are other origins
    for (char const* url : {"https://gprl-api.gprl.workers.dev.evil.example/x", "https://evil.example/https://gprl-api.gprl.workers.dev/x",
                            "https://gprl-api.gprl.workers.dev:8443/x", "http://localhost:8787/x"}) {
        s.uploadUrl = url;
        plan = planPut(api, s);
        CHECK_MSG(!plan.sendBearer, url);
    }
    s.uploadUrl = "https://gprl-api.gprl.workers.dev@evil.example/x";   // userinfo trick: refused outright
    CHECK(!planPut(api, s).ok);
    s.uploadUrl = "//evil.example/x";                                   // scheme-relative: another host
    CHECK(!planPut(api, s).ok);
    s.uploadUrl = "http://storage.example/x";                           // plain http to the internet
    plan = planPut(api, s);
    CHECK(!plan.ok && contains(plan.error, "plain http"));
    s.uploadUrl = "ftp://storage.example/x";
    CHECK(!planPut(api, s).ok);
    s.uploadUrl = "";
    CHECK(!planPut(api, s).ok);
    // control characters, line breaks and spaces never reach the HTTP client
    for (char const* url : {"/v1/x\r\nX-Injected: 1", "/v1/x y", "https://storage.example/a\tb", "/v1/\x01", "/v1/\xc3\xa9"}) {
        s.uploadUrl = url;
        CHECK_MSG(!planPut(api, s).ok, "accepted a URL with a forbidden character");
    }
    s.uploadUrl = "/x";
    CHECK(!planPut("not a url", s).ok);

    // a local dev API (wrangler dev) may use http
    s.uploadUrl = "http://127.0.0.1:8787/v1/me/evidence/uploads/up-1";
    plan = planPut("http://127.0.0.1:8787", s);
    CHECK(plan.ok && plan.sendBearer);
    s.uploadUrl = "http://localhost:9000/bucket/x";
    plan = planPut("http://127.0.0.1:8787", s);
    CHECK(plan.ok && !plan.sendBearer);

    SECTION("PUT plan: session headers are passed through, credentials and framing headers are refused");
    s.uploadUrl = "/x";
    s.headers = {{"Content-Type", "video/mp4"}, {"x-amz-meta-clip", "clip-1"}};
    plan = planPut(api, s);
    CHECK(plan.ok && plan.headers.size() == 2);
    for (char const* forbidden : {"Authorization", "authorization", "Cookie", "Host", "Content-Length", "Transfer-Encoding", "Proxy-Authorization",
                                  "Connection"}) {
        s.headers = {{"Content-Type", "video/mp4"}, {forbidden, "x"}};
        plan = planPut(api, s);
        CHECK_MSG(!plan.ok && plan.headers.empty(), forbidden);
    }
    s.headers = {{"Bad Name", "x"}};
    CHECK(!planPut(api, s).ok);
    s.headers = {{"X-Ok", "line\r\nInjected: 1"}};           // header injection
    CHECK(!planPut(api, s).ok);
    s.headers = {{"X-Ok", std::string(2000, 'a')}};
    CHECK(!planPut(api, s).ok);
    s.headers.assign(17, {"X-A", "1"});
    CHECK(!planPut(api, s).ok);

    CHECK(originOf("HTTPS://Example.COM:443/a/b?c#d") == "https://example.com");
    CHECK(originOf("http://example.com:80") == "http://example.com");
    CHECK(originOf("https://example.com:8443/x") == "https://example.com:8443");
    CHECK(originOf("example.com").empty() && originOf("https://").empty() && originOf("https://a b/").empty());
}

void testRetryRules() {
    SECTION("upload session response: what is retried, what is final, what needs a reconnect");
    CHECK(classifySessionResponse(201, "").verdict == UploadVerdict::Done);
    CHECK(classifySessionResponse(200, "").verdict == UploadVerdict::Done);
    for (int status : {0, 408, 425, 429, 500, 502, 503, 504}) CHECK_MSG(classifySessionResponse(status, "").verdict == UploadVerdict::Retry, std::to_string(status));
    auto unauthorized = classifySessionResponse(401, "unauthorized");
    CHECK(unauthorized.verdict == UploadVerdict::Fatal && unauthorized.reconnect && contains(unauthorized.message, "Connect again"));
    // the endpoint does not exist yet on this server (stream V2 not deployed): a clear, final message
    for (int status : {404, 405, 501}) {
        auto step = classifySessionResponse(status, "not_found");
        CHECK(step.verdict == UploadVerdict::Fatal && !step.reconnect && contains(step.message, "does not accept evidence uploads yet"));
    }
    CHECK(classifySessionResponse(413, "").verdict == UploadVerdict::Fatal);
    CHECK(classifySessionResponse(403, "forbidden").verdict == UploadVerdict::Fatal);
    CHECK(contains(classifySessionResponse(422, "validation_failed").message, "validation_failed"));
    auto exists = classifySessionResponse(409, "evidence_exists");
    CHECK(exists.verdict == UploadVerdict::Done && exists.alreadyUploaded);
    CHECK(classifySessionResponse(409, "conflict").verdict == UploadVerdict::Fatal);

    SECTION("PUT response: hash echo checked, expired one-time URLs retried with a new session");
    CHECK(classifyPutResponse(200, "", "", kSha).verdict == UploadVerdict::Done);
    CHECK(classifyPutResponse(204, "", kSha, kSha).verdict == UploadVerdict::Done);
    CHECK(classifyPutResponse(200, "", "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", kSha).verdict == UploadVerdict::Done);
    auto mismatch = classifyPutResponse(200, "", std::string(64, '0'), kSha);
    CHECK(mismatch.verdict == UploadVerdict::Retry && contains(mismatch.message, "hash mismatch"));
    for (int status : {0, 403, 404, 408, 409, 410, 429, 500, 503}) CHECK_MSG(classifyPutResponse(status, "", "", kSha).verdict == UploadVerdict::Retry, std::to_string(status));
    CHECK(classifyPutResponse(409, "evidence_exists", "", kSha).verdict == UploadVerdict::Done);
    CHECK(classifyPutResponse(401, "", "", kSha).verdict == UploadVerdict::Fatal && classifyPutResponse(401, "", "", kSha).reconnect);
    CHECK(classifyPutResponse(413, "", "", kSha).verdict == UploadVerdict::Fatal);
    CHECK(classifyPutResponse(501, "", "", kSha).verdict == UploadVerdict::Fatal);
    CHECK(classifyPutResponse(400, "bad", "", kSha).verdict == UploadVerdict::Fatal);

    SECTION("backoff: the first try is immediate, then 2 s, 6 s, 15 s");
    CHECK(backoffMs(0) == 0 && backoffMs(1) == 0);
    CHECK(backoffMs(2) == 2000 && backoffMs(3) == 6000 && backoffMs(4) == 15000 && backoffMs(9) == 15000);
}

void testHashFile() {
    SECTION("streaming SHA-256 of a file = one-shot SHA-256 of its bytes (FIPS vectors, chunk boundaries)");
    std::filesystem::path dir = std::filesystem::path(g_scratch);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto write = [&](char const* name, std::string const& bytes) {
        auto path = dir / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return path;
    };
    std::string hex;
    int64_t size = -1;
    // FIPS 180-4 vectors
    CHECK(hashFile(write("gprl_clip_hash_abc.bin", "abc"), hex, size));
    CHECK(hex == kSha && size == 3);
    CHECK(hashFile(write("gprl_clip_hash_empty.bin", ""), hex, size));
    CHECK(hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" && size == 0);
    // one million 'a' (FIPS long message), which also crosses the 1 MiB chunk boundary when doubled
    std::string million(1000000, 'a');
    CHECK(hashFile(write("gprl_clip_hash_million.bin", million), hex, size));
    CHECK(hex == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0" && size == 1000000);
    // sizes around the chunk size: streaming must equal the one-shot digest
    for (size_t n : {kFlow.hashChunkBytes - 1, kFlow.hashChunkBytes, kFlow.hashChunkBytes + 1, 2 * kFlow.hashChunkBytes + 12345}) {
        std::string data(n, '\0');
        uint32_t x = 0x9e3779b9u;
        for (size_t i = 0; i < n; ++i) {
            x = x * 1664525u + 1013904223u;
            data[i] = static_cast<char>(x >> 24);
        }
        auto path = write("gprl_clip_hash_chunks.bin", data);
        CHECK(hashFile(path, hex, size));
        CHECK(static_cast<size_t>(size) == n);
        CHECK(hex == crypto::toHex(crypto::sha256(data)));
        CHECK(validSha256Hex(hex));
    }
    // a changed byte changes the hash (evidence integrity: the upload re-checks it)
    std::string data(4096, 'x');
    auto path = write("gprl_clip_hash_tamper.bin", data);
    std::string before;
    CHECK(hashFile(path, before, size));
    data[2048] = 'y';
    write("gprl_clip_hash_tamper.bin", data);
    CHECK(hashFile(path, hex, size) && hex != before);
    // missing file, cancel flag
    CHECK(!hashFile(dir / "gprl_clip_hash_missing.bin", hex, size));
    std::atomic<bool> cancel{true};
    CHECK(!hashFile(path, hex, size, &cancel));
    for (char const* name : {"gprl_clip_hash_abc.bin", "gprl_clip_hash_empty.bin", "gprl_clip_hash_million.bin", "gprl_clip_hash_chunks.bin",
                             "gprl_clip_hash_tamper.bin"})
        std::filesystem::remove(dir / name, ec);
}

void testIdsAndNames() {
    SECTION("clip ids: valid for clip_available, unique per attempt / time / counter, no player data");
    std::string id = makeClipId("s1-a7", 1790000000000, 1);
    CHECK_MSG(validClipId(id), id);
    CHECK(id.rfind("clip-1a0c4506c00-", 0) == 0 && id.size() == 25);   // 1790000000000 = 0x1a0c4506c00
    CHECK(makeClipId("s1-a7", 1790000000000, 1) == id);               // deterministic
    CHECK(makeClipId("s1-a7", 1790000000000, 2) != id);
    CHECK(makeClipId("s1-a8", 1790000000000, 1) != id);
    CHECK(makeClipId("s1-a7", 1790000000001, 1) != id);
    CHECK(validClipId(makeClipId("", -5, 0)));
    CHECK(validClipId("clip-19a0b1c2d3e-5f6a7b8c"));
    CHECK(!validClipId("short") && !validClipId("has space in it") && !validClipId(std::string(65, 'x')) && validClipId(std::string(64, 'x')));
    CHECK(!validClipId("clip/../../x"));
    CHECK(validSha256Hex(kSha));
    CHECK(!validSha256Hex("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
    CHECK(!validSha256Hex("ba7816bf") && !validSha256Hex(std::string(64, 'g')));

    SECTION("clip file names: Windows-safe, ASCII, bounded");
    CHECK(clipFileName("Bloodbath", "10565740", 100.0, true, 2026, 9, 30, 1, 2, 3) == "GPRL Bloodbath 100pct 2026-09-30 01-02-03.mp4");
    CHECK(clipFileName("Bloodbath", "10565740", 57.9, false, 2026, 9, 30, 23, 59, 59) == "GPRL Bloodbath 57pct 2026-09-30 23-59-59.mp4");
    CHECK(clipFileName("a<b>:c\"/\\|?*.", "1", 3.0, false, 2026, 1, 2, 3, 4, 5) == "GPRL a_b__c_______ 3pct 2026-01-02 03-04-05.mp4");
    CHECK(clipFileName("", "10565740", 0.0, false, 2026, 1, 2, 3, 4, 5) == "GPRL level 10565740 0pct 2026-01-02 03-04-05.mp4");
    CHECK(clipFileName("   ", "../x", 0.0, false, 2026, 1, 2, 3, 4, 5) == "GPRL level x 0pct 2026-01-02 03-04-05.mp4");
    std::string longName = clipFileName(std::string(300, 'N'), "1", 100.0, true, 2026, 9, 30, 1, 2, 3);
    CHECK(longName.size() <= 120 && longName.rfind(".mp4") == longName.size() - 4);
    std::string odd = clipFileName("\xc3\xa9t\xc3\xa9 ..", "1", 100.0, true, 2026, 9, 30, 1, 2, 3);
    for (char c : odd) CHECK(static_cast<unsigned char>(c) < 127 && c != '<' && c != '>' && c != ':' && c != '"' && c != '/' && c != '\\' && c != '|' && c != '?' && c != '*');
}

void testIndex() {
    SECTION("clip index: every field survives a round trip; junk is skipped");
    ClipRecord r = ready();
    r.hasMicAudio = true;
    r.wholeAttempt = false;
    r.levelCounts = false;
    r.uploadId = "up-9";
    r.uploadTries = 2;
    r.error = "upload failed: \"quoted\" \\ text";
    r.state = ClipState::UploadFailed;
    r.choiceMade = true;
    r.choice = Choice::SaveAndSend;
    r.savedLocal = true;
    r.eventSent = true;
    ClipRecord other = preparing();
    other.clipId = "clip-00000000001-aaaaaaaa";
    std::string text = indexToText({r, other});
    CHECK(contains(text, "\"version\": \"gprl-clip-flow/1\""));
    auto back = indexFromText(text);
    CHECK(back.size() == 2);
    auto const& b = back[0];
    CHECK(b.clipId == r.clipId && b.attemptId == r.attemptId && b.sessionLocalId == r.sessionLocalId && b.serverSessionId == r.serverSessionId);
    CHECK(b.levelId == r.levelId && b.levelName == r.levelName && b.levelHash == r.levelHash && b.attemptNo == r.attemptNo);
    CHECK(b.percent == r.percent && b.completed == r.completed && b.levelCounts == r.levelCounts && b.rule == r.rule);
    CHECK(b.state == r.state && b.choiceMade == r.choiceMade && b.choice == r.choice && b.path == r.path && b.sha256 == r.sha256);
    CHECK(b.sizeBytes == r.sizeBytes && b.durationMs == r.durationMs && b.width == r.width && b.height == r.height);
    CHECK(b.hasGameAudio == r.hasGameAudio && b.hasMicAudio == r.hasMicAudio && b.wholeAttempt == r.wholeAttempt);
    CHECK(b.savedLocal == r.savedLocal && b.eventSent == r.eventSent && b.uploaded == r.uploaded && b.uploadId == r.uploadId);
    CHECK(b.uploadTries == r.uploadTries && b.error == r.error && b.createdAtMs == r.createdAtMs && b.endT == r.endT && b.endTick == r.endTick);
    CHECK(back[1].state == ClipState::Preparing && back[1].clipId == other.clipId);
    CHECK(indexToText(back) == text);                         // stable
    CHECK(indexFromText("").empty() && indexFromText("not json").empty() && indexFromText("{\"clips\":5}").empty());
    // records with a bad id / state are skipped, the rest is kept
    auto partial = indexFromText(R"({"clips":[{"clipId":"bad id","attemptId":"a","state":"ready"},{"clipId":"clip-00000000001-aaaaaaaa","attemptId":"a","state":"nope"},
        {"clipId":"clip-00000000002-bbbbbbbb","attemptId":"a","state":"saved"}]})");
    CHECK(partial.size() == 1 && partial[0].state == ClipState::Saved && partial[0].levelCounts && partial[0].wholeAttempt);

    SECTION("trimIndex: at most 3 clips wait for a choice (oldest dropped), finished records are capped at 20");
    std::vector<ClipRecord> records;
    for (int i = 0; i < 5; ++i) {
        ClipRecord w = ready();
        w.clipId = "clip-0000000000" + std::to_string(i) + "-aaaaaaaa";
        w.createdAtMs = 1000 + i;
        records.push_back(w);
    }
    ClipRecord uploading = ready();
    uploading.clipId = "clip-uploading-aaaaaaaa";
    uploading.state = ClipState::Uploading;
    uploading.createdAtMs = 1;
    records.push_back(uploading);
    auto dropped = trimIndex(records);
    CHECK(dropped.size() == 2 && dropped[0].createdAtMs == 1000 && dropped[1].createdAtMs == 1001);
    CHECK(records.size() == 4);
    CHECK(std::count_if(records.begin(), records.end(), [](ClipRecord const& x) { return x.state == ClipState::Ready; }) == 3);
    CHECK(std::any_of(records.begin(), records.end(), [](ClipRecord const& x) { return x.state == ClipState::Uploading; }));   // never trimmed
    std::vector<ClipRecord> many;
    for (int i = 0; i < 30; ++i) {
        ClipRecord f = ready();
        f.state = i % 2 ? ClipState::Saved : ClipState::Discarded;
        f.createdAtMs = i;
        many.push_back(f);
    }
    CHECK(trimIndex(many).empty());                           // finished records own no pending file
    CHECK(many.size() == 20 && many.front().createdAtMs == 10);
}

void testText() {
    SECTION("sizes, durations and the Account tab line");
    CHECK(formatBytes(0) == "0 KB" && formatBytes(512 * 1024) == "512 KB");
    CHECK(formatBytes(78ll * 1024 * 1024 + 209715) == "78.2 MB");
    CHECK(formatBytes(512ll * 1024 * 1024) == "512 MB");
    CHECK(formatBytes(3ll * 1024 * 1024 * 1024 / 2) == "1.50 GB");
    CHECK(formatBytes(-5) == "0 KB");
    CHECK(formatDuration(0) == "0:00" && formatDuration(118400) == "1:58" && formatDuration(600000) == "10:00" && formatDuration(59499) == "0:59");

    ClipRecord r = preparing();
    CHECK(statusLine(r, 0.0) == "Clip: Bloodbath 100% - preparing...");
    onChoice(r, Choice::SaveLocal, connected());
    CHECK(statusLine(r, 0.0) == "Clip: Bloodbath 100% - preparing (Save to computer chosen)");
    r = ready();
    CHECK(statusLine(r, 0.0) == "Clip: Bloodbath 100% 1:41 78.0 MB - waiting for your choice");
    onChoice(r, Choice::Send, connected());
    CHECK(statusLine(r, 0.437) == "Clip: Bloodbath 100% 1:41 78.0 MB - uploading 43%");
    CHECK(statusLine(r, 7.0) == "Clip: Bloodbath 100% 1:41 78.0 MB - uploading 100%");
    onUploadResult(r, false, "network error");
    CHECK(statusLine(r, 0.0) == "Clip: Bloodbath 100% 1:41 78.0 MB - upload failed: network error");
    onRetryUpload(r, connected());
    onUploadResult(r, true, "");
    CHECK(statusLine(r, 0.0) == "Clip: Bloodbath 100% 1:41 78.0 MB - sent to GPRL moderators");
    ClipRecord both = ready();
    onChoice(both, Choice::SaveAndSend, connected());
    onUploadResult(both, true, "");
    CHECK(statusLine(both, 0.0) == "Clip: Bloodbath 100% 1:41 78.0 MB - sent to GPRL moderators + saved");
    ClipRecord saved = ready();
    onChoice(saved, Choice::SaveLocal, connected());
    CHECK(statusLine(saved, 0.0) == "Clip: Bloodbath 100% 1:41 78.0 MB - saved on this computer");
    ClipRecord death = ready();
    death.completed = false;
    death.percent = 87.9;
    death.levelName.clear();
    onChoice(death, Choice::Nothing, connected());
    CHECK(statusLine(death, 0.0) == "Clip: level 10565740 87% 1:41 78.0 MB - discarded");
}

}  // namespace

int main() {
    testNames();
    testFourChoices();
    testGate();
    testChoiceWhilePreparing();
    testUploadFailure();
    testNeverAutomaticUpload();
    testRestart();
    testUploadRequest();
    testUploadSession();
    testPreserveHint();
    testVerificationHint();
    testYouTubeLinks();
    testPutPlan();
    testRetryRules();
    testHashFile();
    testIdsAndNames();
    testIndex();
    testText();
    return gprl::test::finish("clip_flow_tests");
}
