// analyzer_worker_tests: the pure rules behind the analyzer's worker thread and its local result
// store (core/analyzer_worker_rules.hpp; review fixes 2026-10-02): the session map (MEDIUM-5), the
// backlog of levels waiting for the simulator (MEDIUM-8), the store's version key (MEDIUM-9) and LRU
// cap (MEDIUM-10), the 429 Retry-After clamp, the exact upload body size and the revisit re-arm (LOW).
#include "../core/analyzer_worker_rules.hpp"
#include "test_util.hpp"

#include <string>
#include <vector>

using namespace gprl::analyzer::rules;

namespace {

struct Payload {
    int level = 0;
    std::vector<int> attempts;
};

}  // namespace

int main(int, char**) {
    SECTION("SessionMap: only the published pair, newer generation wins, empty ignored");
    {
        SessionMap m(4);
        CHECK(!m.note("", "s1", 1));
        CHECK(!m.note("h1", "", 1));
        CHECK(m.size() == 0);
        CHECK(m.note("h1", "s1", 3));
        CHECK(m.find("h1") == "s1");
        CHECK(m.find("h2").empty());
        // a late publish of an OLDER session on the same version never overwrites the newer one
        CHECK(!m.note("h1", "s0", 2));
        CHECK(m.find("h1") == "s1");
        // the same or a newer generation replaces it (a new session on a revisit)
        CHECK(m.note("h1", "s4", 4));
        CHECK(m.find("h1") == "s4");
        CHECK(!m.note("h1", "s4", 4));   // unchanged
    }

    SECTION("SessionMap: the bounded map evicts the OLDEST entry, not the smallest key");
    {
        SessionMap m(3);
        m.note("ffff", "a", 1);   // oldest, but the LARGEST key
        m.note("0000", "b", 2);   // smallest key
        m.note("8888", "c", 3);
        m.note("4444", "d", 4);   // over capacity: "ffff" goes
        CHECK(m.size() == 3);
        CHECK(!m.contains("ffff"));
        CHECK(m.find("0000") == "b");
        CHECK(m.find("8888") == "c");
        CHECK(m.find("4444") == "d");
        // a used entry becomes the newest: "0000" is refreshed, so "8888" is the next to go
        m.note("0000", "b2", 5);
        m.note("cccc", "e", 6);
        CHECK(!m.contains("8888"));
        CHECK(m.find("0000") == "b2");
        CHECK(m.contains("4444") && m.contains("cccc"));
    }

    SECTION("LevelBacklog: ordered, bounded (oldest dropped), attempts and exits attach to the waiting visit");
    {
        LevelBacklog<Payload> b(2);
        CHECK(b.empty());
        CHECK(b.push(1, Payload{10, {}}) == 0);
        CHECK(b.push(2, Payload{20, {}}) == 0);
        CHECK(b.push(3, Payload{30, {}}) == 1);   // capacity 2: visit 1 dropped
        CHECK(b.size() == 2);
        CHECK(b.find(1) == nullptr);
        auto* i2 = b.find(2);
        CHECK(i2 != nullptr);
        if (i2) i2->payload.attempts.push_back(7);
        CHECK(b.markClosed(2));
        CHECK(!b.markClosed(9));   // not waiting: the caller closes the handled visit itself
        auto first = b.take();
        CHECK(first.visitId == 2 && first.payload.level == 20 && first.closed);
        CHECK(first.payload.attempts.size() == 1 && first.payload.attempts[0] == 7);
        auto second = b.take();
        CHECK(second.visitId == 3 && !second.closed && second.payload.attempts.empty());
        CHECK(b.empty());
    }

    SECTION("store versions: the tag changes with every version, the check compares all four");
    {
        StoreVersions v{"gprl-analyzer/2", "gprl-sim/2", "gprl-gameplay-hash/2", "gprl-extract/2"};
        std::string tag = versionTag(v);
        CHECK(tag.size() == 8);
        for (char c : tag) CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        CHECK(versionTag(v) == tag);   // deterministic
        StoreVersions a = v;
        a.analyzer = "gprl-analyzer/1";
        StoreVersions s = v;
        s.sim = "gprl-sim/1";
        StoreVersions g = v;
        g.gameplayHash = "gprl-gameplay-hash/1";
        StoreVersions x = v;
        x.extract = "gprl-extract/1";
        for (auto const* o : {&a, &s, &g, &x}) {
            CHECK(versionTag(*o) != tag);
            CHECK(!versionsMatch(*o, v));
        }
        CHECK(versionsMatch(v, v));
        // the field boundary counts: moving a character between two versions changes the tag
        StoreVersions shifted{"ab", "c", "d", "e"};
        StoreVersions shifted2{"a", "bc", "d", "e"};
        CHECK(versionTag(shifted) != versionTag(shifted2));
        CHECK(!versionsMatch(StoreVersions{}, v));   // a record without versions (v0.12.0 before the rule) never matches
        CHECK(resultFileName(20, "00112233aabbccdd", v) == "20-00112233aabbccdd-" + tag + ".json");
    }

    SECTION("lruEvict: least recently used first, until under the cap, the protected file kept");
    {
        std::vector<CacheFile> files = {{100, 50}, {100, 10}, {100, 30}, {100, 20}};
        CHECK(lruEvict(files, 400).empty());
        auto d = lruEvict(files, 250);
        CHECK(d.size() == 2 && d[0] == 1 && d[1] == 3);   // stamps 10 and 20 go
        auto p = lruEvict(files, 250, 1);                 // the oldest is the one just written: kept
        CHECK(p.size() == 2 && p[0] == 3 && p[1] == 2);
        auto all = lruEvict(files, 0, 0);
        CHECK(all.size() == 3);                           // everything but the protected file
        std::vector<CacheFile> ties = {{10, 5}, {10, 5}, {10, 5}};
        auto t = lruEvict(ties, 15);
        CHECK(t.size() == 2 && t[0] == 0 && t[1] == 1);   // ties: lower index first (deterministic)
        CHECK(kCacheCapBytes == 200ull * 1024ull * 1024ull);
    }

    SECTION("429: Retry-After seconds clamped to [60 s, 60 min], else 10 min");
    {
        CHECK(rateLimitDelaySeconds(-1) == 600);
        CHECK(rateLimitDelaySeconds(0) == 60);
        CHECK(rateLimitDelaySeconds(5) == 60);
        CHECK(rateLimitDelaySeconds(120) == 120);
        CHECK(rateLimitDelaySeconds(599) == 599);
        CHECK(rateLimitDelaySeconds(86400) == 3600);
        CHECK(parseRetryAfter("120") == 120);
        CHECK(parseRetryAfter(" 45 ") == 45);
        CHECK(parseRetryAfter("") == -1);
        CHECK(parseRetryAfter("Wed, 21 Oct 2026 07:28:00 GMT") == -1);   // an HTTP date: the default applies
        CHECK(parseRetryAfter("12abc") == -1);
        CHECK(parseRetryAfter("99999999999999") == 86400);              // no overflow
    }

    SECTION("upload body: the exact text Api.cpp sends and its size (the 2 MB check covers the envelope)");
    {
        std::string result = "{\"a\":1}";
        CHECK(levelSimUploadBody("", result) == "{\"result\":{\"a\":1}}");
        CHECK(levelSimUploadBody("abc-123", result) == "{\"sessionId\":\"abc-123\",\"result\":{\"a\":1}}");
        for (std::string sid : {std::string(), std::string("abc-123"), std::string("we\"ird\\id"), std::string(36, 'f')}) {
            for (std::string r : {std::string("{}"), result, std::string(5000, ' ')}) {
                CHECK_MSG(levelSimUploadBodyBytes(sid, r) == levelSimUploadBody(sid, r).size(), sid);
            }
        }
        // a result just under 2 MB is over the limit once the envelope is added
        std::string big(2u * 1024u * 1024u - 10u, 'x');
        CHECK(big.size() < 2u * 1024u * 1024u);
        CHECK(levelSimUploadBodyBytes("0123456789abcdef0123456789abcdef", big) > 2u * 1024u * 1024u);
    }

    SECTION("revisit re-arm: waited for a session / promised a later visit -> sent again; never sent / refused / off");
    {
        CHECK(rearmOnRevisit("kept on this computer (no session on this level yet)"));
        CHECK(rearmOnRevisit("waiting for the session"));
        CHECK(rearmOnRevisit("waiting for the session (retry)"));
        CHECK(rearmOnRevisit("not sent: the server does not know this level version yet (sent next visit)"));
        CHECK(rearmOnRevisit("not sent: rate limited, sent on a later visit"));
        CHECK(rearmOnRevisit("send failed (internal)"));
        CHECK(!rearmOnRevisit("sent (Partial)"));
        CHECK(!rearmOnRevisit("sent, not stored (better)"));
        CHECK(!rearmOnRevisit("kept on this computer (sending is off)"));
        CHECK(!rearmOnRevisit("kept on this computer (local-only)"));
        CHECK(!rearmOnRevisit("kept on this computer (not connected)"));
        CHECK(!rearmOnRevisit("too large to send"));
        CHECK(!rearmOnRevisit("refused by the server (session)"));
        CHECK(!rearmOnRevisit("not sent: the session is on another version of this level"));
        CHECK(!rearmOnRevisit(""));
    }

    return gprl::test::finish("analyzer_worker_tests");
}
