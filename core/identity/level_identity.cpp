// identity::compute + toJson + the similarity helpers (docs/LEVEL_FAMILY_DESIGN.md §1-§3).
// Basename is level_identity.cpp (not identity.cpp): tests/run_tests.ps1 compiles every core
// source into one folder and cl.exe names objects by basename; core/identity.cpp already exists.
#include "identity.hpp"

#include <algorithm>
#include <cmath>

#include "gameplay_fingerprint.hpp"
#include "presentation.hpp"
#include "sections.hpp"

namespace gprl::identity {

namespace {

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

double norm(double value, double scale) {
    if (!(scale > 0.0) || !std::isfinite(value)) return 0.0;
    return clamp01(value / scale);
}

json::Value presentationJson(PresentationVector const& p) {
    json::Value o = json::Value::object();
    o.set("decoDensity", p.decoDensity);
    o.set("obstruction", p.obstruction);
    o.set("clutter", p.clutter);
    o.set("fakeObjects", p.fakeObjects);
    o.set("hiddenGameplay", p.hiddenGameplay);
    o.set("flashes", p.flashes);
    o.set("cameraEffects", p.cameraEffects);
    o.set("glowShare", p.glowShare);
    o.set("opacityLow", p.opacityLow);
    o.set("previewSeconds", p.previewSeconds);
    o.set("indicators", p.indicators);
    return o;
}

}  // namespace

std::array<double, kPresentationFeatures> normalise(PresentationVector const& v, IdentityConfig const& cfg) {
    return {
        norm(v.decoDensity, cfg.decoDensityScale),
        norm(v.obstruction, cfg.obstructionScale),
        norm(v.clutter, cfg.clutterScale),
        norm(v.fakeObjects, cfg.fakeObjectsScale),
        norm(v.hiddenGameplay, cfg.hiddenGameplayScale),
        norm(v.flashes, cfg.flashesScale),
        norm(v.cameraEffects, cfg.cameraEffectsScale),
        norm(v.glowShare, cfg.glowShareScale),
        norm(v.opacityLow, cfg.opacityLowScale),
        norm(v.previewSeconds, cfg.previewCapSeconds),
        norm(v.indicators, cfg.indicatorsScale),
    };
}

double presentationSimilarity(PresentationVector const& a, PresentationVector const& b, IdentityConfig const& cfg) {
    auto na = normalise(a, cfg), nb = normalise(b, cfg);
    double sum = 0.0;
    for (int i = 0; i < kPresentationFeatures; ++i) sum += std::fabs(na[static_cast<size_t>(i)] - nb[static_cast<size_t>(i)]);
    return clamp01(1.0 - sum / kPresentationFeatures);
}

double readability(PresentationVector const& v, IdentityConfig const& cfg) {
    auto n = normalise(v, cfg);
    double lowPreview = 1.0 - n[9];
    double r = cfg.wObstruction * n[1] + cfg.wFlashes * n[5] + cfg.wFakeObjects * n[3] + cfg.wHiddenGameplay * n[4]
             + cfg.wCameraEffects * n[6] + cfg.wClutter * n[2] + cfg.wLowPreview * lowPreview;
    return clamp01(r);
}

double jaccardEstimate(MinHash const& a, MinHash const& b) {
    int equal = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == b[i]) ++equal;
    }
    return static_cast<double>(equal) / static_cast<double>(a.size());
}

LevelIdentity compute(World const& world, DecoSet const& deco, IdentityInput const& input, IdentityConfig const& cfg) {
    LevelIdentity id;
    id.levelHash = input.levelHash.empty() ? world.levelHash : input.levelHash;
    id.gdLevelId = input.gdLevelId != 0 ? input.gdLevelId : world.gdLevelId;
    id.copiedFromGdId = input.copiedFromGdId;
    id.nameHint = input.nameHint;
    id.gameplayFingerprint = gameplayFingerprint(world, cfg);

    auto cuts = cutSections(world, cfg);
    auto assignment = assignSections(world, cuts);
    id.sections = sectionsFor(world, cuts, assignment, cfg);

    std::vector<std::vector<SimObject const*>> perSection(cuts.size());
    int hashed = 0;
    for (size_t i = 0; i < world.objects.size(); ++i) {
        int s = assignment[i];
        if (s < 0 || s >= static_cast<int>(cuts.size())) continue;
        perSection[static_cast<size_t>(s)].push_back(&world.objects[i]);
        ++hashed;
    }
    for (size_t s = 0; s < id.sections.size(); ++s) {
        id.sections[s].presentation = presentationOf(world, deco, cuts, static_cast<int>(s), perSection[s], cfg);
        id.sections[s].readability = readability(id.sections[s].presentation, cfg);
    }
    id.presentationFingerprint = presentationFingerprint(id.sections, cfg);

    double end = static_cast<double>(levelEnd(world));
    auto percentOf = [end](float x) { return end > 0.0 ? static_cast<double>(x) / end * 100.0 : 0.0; };
    if (!world.startPositions.empty()) {
        for (auto const& sp : world.startPositions) {
            StartPosEntry e;
            e.x = sp.x;
            e.percent = percentOf(sp.x);
            e.mode = sp.mode;
            e.mini = sp.mini;
            e.speed = sp.speed;
            e.upsideDown = sp.upsideDown;
            e.dual = sp.dual;
            id.startPos.push_back(e);
        }
    } else {
        for (auto const& o : world.objects) {
            if (hashedForIdentity(o)) continue;
            StartPosEntry e;
            e.x = o.x;
            e.percent = percentOf(o.x);
            e.mode = o.mode;
            e.mini = o.flag;
            e.speed = o.speed;
            id.startPos.push_back(e);
        }
    }
    std::stable_sort(id.startPos.begin(), id.startPos.end(), [](StartPosEntry const& a, StartPosEntry const& b) { return a.x < b.x; });

    id.gameplayObjects = hashed;
    id.decorationObjects = world.decorationObjects > 0 ? world.decorationObjects : static_cast<int>(deco.deco.size());
    id.lengthX = world.lengthX > 0.f ? world.lengthX : static_cast<float>(end);
    return id;
}

json::Value configJson(IdentityConfig const& cfg) {
    json::Value o = json::Value::object();
    o.set("maxSectionLength", static_cast<double>(cfg.maxSectionLength));
    o.set("minSectionLength", static_cast<double>(cfg.minSectionLength));
    o.set("positionQuantum", cfg.positionQuantum);
    json::Value scales = json::Value::object();
    scales.set("decoDensity", cfg.decoDensityScale);
    scales.set("obstruction", cfg.obstructionScale);
    scales.set("clutter", cfg.clutterScale);
    scales.set("fakeObjects", cfg.fakeObjectsScale);
    scales.set("hiddenGameplay", cfg.hiddenGameplayScale);
    scales.set("flashes", cfg.flashesScale);
    scales.set("cameraEffects", cfg.cameraEffectsScale);
    scales.set("glowShare", cfg.glowShareScale);
    scales.set("opacityLow", cfg.opacityLowScale);
    scales.set("previewSeconds", cfg.previewCapSeconds);
    scales.set("indicators", cfg.indicatorsScale);
    o.set("normalisationScales", std::move(scales));
    json::Value weights = json::Value::object();
    weights.set("obstruction", cfg.wObstruction);
    weights.set("flashes", cfg.wFlashes);
    weights.set("fakeObjects", cfg.wFakeObjects);
    weights.set("hiddenGameplay", cfg.wHiddenGameplay);
    weights.set("cameraEffects", cfg.wCameraEffects);
    weights.set("clutter", cfg.wClutter);
    weights.set("lowPreview", cfg.wLowPreview);
    o.set("readabilityWeights", std::move(weights));
    o.set("flashMaxDuration", cfg.flashMaxDuration);
    o.set("opacityLowThreshold", cfg.opacityLowThreshold);
    o.set("hiddenObstructionShare", cfg.hiddenObstructionShare);
    o.set("clutterGrid", static_cast<double>(cfg.clutterGrid));
    o.set("minSectionHeight", static_cast<double>(cfg.minSectionHeight));
    json::Value speeds = json::Value::array();
    for (double s : cfg.speedUnitsPerSecond) speeds.push(s);
    o.set("speedUnitsPerSecond", std::move(speeds));
    o.set("digestQuantum", cfg.digestQuantum);
    o.set("featureDecimals", cfg.featureDecimals);
    json::Value ind = json::Value::array();
    for (int id : cfg.indicatorIds) ind.push(id);
    o.set("indicatorIds", std::move(ind));
    o.set("minhashSize", kMinhashSize);
    return o;
}

json::Value toJson(LevelIdentity const& id) {
    json::Value o = json::Value::object();
    o.set("gameplayFingerprintVersion", id.gameplayFingerprintVersion);
    o.set("sectionVersion", id.sectionVersion);
    o.set("presentationVersion", id.presentationVersion);
    o.set("levelHash", id.levelHash);
    o.set("gameplayFingerprint", id.gameplayFingerprint);
    o.set("presentationFingerprint", id.presentationFingerprint);
    o.set("gdLevelId", id.gdLevelId);
    o.set("copiedFromGdId", id.copiedFromGdId);
    o.set("nameHint", id.nameHint);
    json::Value sections = json::Value::array();
    for (auto const& s : id.sections) {
        json::Value so = json::Value::object();
        so.set("index", s.index);
        so.set("xFrom", static_cast<double>(s.xFrom));
        so.set("xTo", static_cast<double>(s.xTo));
        so.set("pctFrom", s.pctFrom);
        so.set("pctTo", s.pctTo);
        so.set("hash", s.hash);
        json::Value mh = json::Value::array();
        for (uint32_t v : s.minhash) mh.push(v);
        so.set("minhash", std::move(mh));
        so.set("gamemode", sim::gamemodeName(s.gamemode));
        so.set("speed", static_cast<int>(s.speed));
        so.set("mini", s.mini);
        so.set("objects", s.objects);
        so.set("presentation", presentationJson(s.presentation));
        so.set("readability", s.readability);
        sections.push(std::move(so));
    }
    o.set("sections", std::move(sections));
    json::Value starts = json::Value::array();
    for (auto const& sp : id.startPos) {
        json::Value e = json::Value::object();
        e.set("x", static_cast<double>(sp.x));
        e.set("percent", sp.percent);
        e.set("mode", sim::gamemodeName(sp.mode));
        e.set("mini", sp.mini);
        e.set("speed", static_cast<int>(sp.speed));
        e.set("upsideDown", sp.upsideDown);
        e.set("dual", sp.dual);
        starts.push(std::move(e));
    }
    o.set("startPos", std::move(starts));
    o.set("gameplayObjects", id.gameplayObjects);
    o.set("decorationObjects", id.decorationObjects);
    o.set("lengthX", static_cast<double>(id.lengthX));
    return o;
}

}  // namespace gprl::identity
