#include "presentation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "../geometry_hash.hpp"
#include "gameplay_fingerprint.hpp"

namespace gprl::identity {

namespace {

// Source: D:\DecoForge-research\sources\opstic_gdclone\assets\data\object.json (GD 2.2 object table
// of the opstic gdclone project): every id with a "hitbox" entry, grouped by hitbox type. Sorted.
constexpr int kBoxIds[] = {
    1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 34, 35, 36, 39, 40, 45, 46, 47, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 74, 75, 76,
    77, 78, 81, 82, 83, 84, 90, 91, 92, 93, 94, 95, 96, 99, 101, 103, 111, 116, 117, 118, 119, 121, 122, 135, 140, 141, 144, 145, 146,
    147, 160, 161, 162, 163, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 192, 194, 195, 196, 197, 200,
    201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 212, 213, 215, 216, 217, 218, 219, 220, 243, 244, 247, 248, 249, 250, 252, 253,
    254, 255, 256, 257, 258, 260, 261, 263, 264, 265, 267, 268, 269, 270, 271, 272, 274, 275, 286, 287, 328, 329, 365, 368, 369, 370,
    392, 421, 422, 446, 447, 458, 459, 467, 468, 469, 470, 471, 475, 660, 661, 662, 663, 664, 667, 720, 745, 768, 989, 991, 1022, 1154,
    1155, 1156, 1157, 1202, 1203, 1204, 1208, 1209, 1210, 1220, 1221, 1222, 1226, 1227, 1260, 1262, 1264, 1329, 1330, 1331, 1332, 1333,
    1334, 1340, 1343, 1561, 1562, 1563, 1564, 1565, 1566, 1567, 1568, 1569, 1704, 1711, 1712, 1713, 1714, 1715, 1716, 1719, 1720, 1721,
    1722, 1725, 1726, 1727, 1728, 1729, 1730, 1731, 1732, 1733, 1751, 1816, 1903, 1904, 1905, 1910,
};
constexpr int kSlopeIds[] = {
    289, 291, 294, 295, 299, 301, 305, 307, 309, 311, 315, 317, 321, 323, 326, 327, 331, 333, 337, 339, 343, 345, 349, 351, 353, 355,
    363, 364, 366, 367, 371, 372, 483, 484, 492, 493, 651, 652, 665, 666, 673, 674, 709, 710, 711, 712, 726, 727, 728, 729, 886, 887,
    1338, 1339, 1341, 1342, 1344, 1345, 1717, 1718, 1723, 1724, 1743, 1744, 1745, 1746, 1747, 1748, 1749, 1750, 1906, 1907,
};
constexpr int kCircleIds[] = {
    88, 89, 98, 183, 184, 185, 186, 187, 188, 397, 398, 399, 675, 676, 677, 678, 679, 680, 740, 741, 742, 1582, 1583, 1619, 1620, 1701,
    1702, 1703, 1705, 1706, 1707, 1708, 1709, 1710, 1734, 1735, 1736,
};

template <size_t N>
bool contains(int const (&ids)[N], int id) {
    return std::binary_search(ids, ids + N, id);
}

double round6(double v, int decimals) {
    if (!std::isfinite(v)) return 0.0;
    double p = std::pow(10.0, decimals);
    return std::round(v * p) / p;
}

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

Rect decoRect(DecoObject const& d) {
    return {static_cast<double>(d.rx), static_cast<double>(d.ry), static_cast<double>(d.rx) + static_cast<double>(d.rw),
            static_cast<double>(d.ry) + static_cast<double>(d.rh)};
}

Rect objRect(SimObject const& o) {
    return {static_cast<double>(o.rx), static_cast<double>(o.ry), static_cast<double>(o.rx) + static_cast<double>(o.rw),
            static_cast<double>(o.ry) + static_cast<double>(o.rh)};
}

bool clip(Rect& r, Rect const& to) {
    r.x0 = std::max(r.x0, to.x0);
    r.y0 = std::max(r.y0, to.y0);
    r.x1 = std::min(r.x1, to.x1);
    r.y1 = std::min(r.y1, to.y1);
    return r.x1 > r.x0 && r.y1 > r.y0;
}

bool finiteRect(Rect const& r) {
    return std::isfinite(r.x0) && std::isfinite(r.y0) && std::isfinite(r.x1) && std::isfinite(r.y1) && r.x1 > r.x0 && r.y1 > r.y0;
}

}  // namespace

SpriteKind collidableSpriteKind(int objectId) {
    if (contains(kBoxIds, objectId)) return SpriteKind::Box;
    if (contains(kSlopeIds, objectId)) return SpriteKind::Slope;
    if (contains(kCircleIds, objectId)) return SpriteKind::Circle;
    return SpriteKind::None;
}

bool isFakeObject(DecoObject const& d) {
    if (!d.passable) return false;
    return d.hazardLook || collidableSpriteKind(d.objectId) != SpriteKind::None;
}

void gameplayZ(SimObject const& o, int& zLayer, int& zOrder) {
    switch (o.kind) {
        case ObjKind::Orb:
        case ObjKind::Pad: zLayer = 3; zOrder = 12; return;
        case ObjKind::SpeedChange: zLayer = 1; zOrder = -6; return;
        case ObjKind::GamemodePortal:
        case ObjKind::GravityPortal:
        case ObjKind::SizePortal: zLayer = 5; zOrder = 10; return;
        default: zLayer = 5; zOrder = 2; return;
    }
}

bool drawnInFront(DecoObject const& d, SimObject const& o) {
    int gl = 5, go = 2;
    gameplayZ(o, gl, go);
    int dl = d.zLayer == 0 ? 5 : d.zLayer;
    return dl > gl || (dl == gl && d.zOrder > go);
}

double unionArea(std::vector<Rect> rects) {
    rects.erase(std::remove_if(rects.begin(), rects.end(), [](Rect const& r) { return !finiteRect(r); }), rects.end());
    if (rects.empty()) return 0.0;
    std::vector<double> xs;
    xs.reserve(rects.size() * 2);
    for (auto const& r : rects) {
        xs.push_back(r.x0);
        xs.push_back(r.x1);
    }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    double area = 0.0;
    std::vector<std::pair<double, double>> ys;
    for (size_t i = 0; i + 1 < xs.size(); ++i) {
        double x0 = xs[i], x1 = xs[i + 1];
        if (x1 <= x0) continue;
        ys.clear();
        for (auto const& r : rects) {
            if (r.x0 <= x0 && r.x1 >= x1) ys.emplace_back(r.y0, r.y1);
        }
        if (ys.empty()) continue;
        std::sort(ys.begin(), ys.end());
        double covered = 0.0, curLo = ys[0].first, curHi = ys[0].second;
        for (size_t j = 1; j < ys.size(); ++j) {
            if (ys[j].first <= curHi) {
                curHi = std::max(curHi, ys[j].second);
            } else {
                covered += curHi - curLo;
                curLo = ys[j].first;
                curHi = ys[j].second;
            }
        }
        covered += curHi - curLo;
        area += covered * (x1 - x0);
    }
    return area;
}

PresentationVector presentationOf(World const& world, DecoSet const& deco, std::vector<SectionCut> const& cuts, int index,
                                  std::vector<SimObject const*> const& sectionObjects, IdentityConfig const& cfg) {
    PresentationVector v;
    if (index < 0 || index >= static_cast<int>(cuts.size())) return v;
    SectionCut const& cut = cuts[static_cast<size_t>(index)];
    double xFrom = static_cast<double>(cut.xFrom), xTo = static_cast<double>(cut.xTo);
    double len = std::max(1.0, xTo - xFrom);
    double per100 = 100.0 / len;

    // decoration membership (centre x) and the rects overlapping the x range
    int members = 0, glow = 0, lowOpacity = 0, fakes = 0, indicators = 0;
    std::vector<DecoObject const*> overlapping;
    for (auto const& d : deco.deco) {
        double cx = static_cast<double>(d.rx) + static_cast<double>(d.rw) * 0.5;
        if (std::isfinite(cx) && sectionIndexForX(cuts, cx) == index) {
            ++members;
            if (!d.noGlow) ++glow;
            if (static_cast<double>(d.opacityHint) < cfg.opacityLowThreshold) ++lowOpacity;
            if (isFakeObject(d)) ++fakes;
            if (d.indicatorLook || std::find(cfg.indicatorIds.begin(), cfg.indicatorIds.end(), d.objectId) != cfg.indicatorIds.end()) ++indicators;
        }
        Rect r = decoRect(d);
        if (finiteRect(r) && r.x1 > xFrom && r.x0 < xTo) overlapping.push_back(&d);
    }
    v.decoDensity = members * per100;
    v.glowShare = members > 0 ? static_cast<double>(glow) / members : 0.0;
    v.opacityLow = members > 0 ? static_cast<double>(lowOpacity) / members : 0.0;
    v.fakeObjects = fakes * per100;
    v.indicators = indicators * per100;

    // obstruction: share of gameplay rect area covered by opaque decoration drawn in front;
    // hidden gameplay: hidden objects + objects obstructed by >= hiddenObstructionShare
    double gameplayArea = 0.0, coveredArea = 0.0;
    int hidden = 0;
    double yLo = static_cast<double>(world.groundY), yHi = yLo;
    bool anyY = false;
    std::vector<Rect> clipped;
    for (SimObject const* o : sectionObjects) {
        Rect r = objRect(*o);
        if (!finiteRect(r)) continue;
        if (!anyY) {
            yLo = std::min(yLo, r.y0);
            yHi = r.y1;
            anyY = true;
        } else {
            yLo = std::min(yLo, r.y0);
            yHi = std::max(yHi, r.y1);
        }
        if (o->hidden) {
            ++hidden;
            continue;
        }
        double area = (r.x1 - r.x0) * (r.y1 - r.y0);
        clipped.clear();
        for (DecoObject const* d : overlapping) {
            if (static_cast<double>(d->opacityHint) < cfg.opacityLowThreshold) continue;
            if (!drawnInFront(*d, *o)) continue;
            Rect c = decoRect(*d);
            if (clip(c, r)) clipped.push_back(c);
        }
        double covered = clipped.empty() ? 0.0 : std::min(area, unionArea(clipped));
        gameplayArea += area;
        coveredArea += covered;
        if (area > 0.0 && covered / area >= cfg.hiddenObstructionShare) ++hidden;
    }
    v.obstruction = gameplayArea > 0.0 ? coveredArea / gameplayArea : 0.0;
    v.hiddenGameplay = hidden * per100;

    // clutter: share of the section's bounding box (x range x [yLo, max(yHi, yLo + screen)])
    // covered by decoration, union approximated on a grid (a cell counts when a rect holds its centre)
    double grid = cfg.clutterGrid > 0.f ? static_cast<double>(cfg.clutterGrid) : 15.0;
    yHi = std::max(yHi, yLo + static_cast<double>(cfg.minSectionHeight));
    if (!overlapping.empty() && yHi > yLo) {
        int cols = std::max(1, static_cast<int>(std::ceil((xTo - xFrom) / grid - 1e-9)));
        int rows = std::max(1, static_cast<int>(std::ceil((yHi - yLo) / grid - 1e-9)));
        double cellW = (xTo - xFrom) / cols, cellH = (yHi - yLo) / rows;
        long long coveredCells = 0;
        std::vector<Rect> rects;
        rects.reserve(overlapping.size());
        for (DecoObject const* d : overlapping) rects.push_back(decoRect(*d));
        for (int cy = 0; cy < rows; ++cy) {
            double py = yLo + (cy + 0.5) * cellH;
            for (int cx = 0; cx < cols; ++cx) {
                double px = xFrom + (cx + 0.5) * cellW;
                for (auto const& r : rects) {
                    if (px >= r.x0 && px < r.x1 && py >= r.y0 && py < r.y1) {
                        ++coveredCells;
                        break;
                    }
                }
            }
        }
        v.clutter = static_cast<double>(coveredCells) / (static_cast<double>(cols) * static_cast<double>(rows));
    }

    // triggers by centre x
    int flashes = 0, camera = 0;
    for (auto const& t : deco.triggers) {
        if (!std::isfinite(t.x) || sectionIndexForX(cuts, static_cast<double>(t.x)) != index) continue;
        switch (t.kind) {
            case TriggerObject::Color:
            case TriggerObject::Pulse:
            case TriggerObject::Alpha:
                if (static_cast<double>(t.duration) <= cfg.flashMaxDuration) ++flashes;
                break;
            case TriggerObject::Shake: ++flashes; break;
            case TriggerObject::CameraZoom:
            case TriggerObject::CameraOffset:
            case TriggerObject::CameraRotate:
            case TriggerObject::CameraStatic:
            case TriggerObject::CameraEdge:
            case TriggerObject::CameraMode: ++camera; break;
            default: break;
        }
    }
    v.flashes = flashes * per100;
    v.cameraEffects = camera * per100;

    // preview: seconds from a forced section start (portal / speed change / level start) to the
    // first hazard or orb at the section's speed; a length-split section is fully previewed
    double cap = cfg.previewCapSeconds;
    double preview = cap;
    if (cut.forced) {
        double first = -1.0;
        for (SimObject const* o : sectionObjects) {
            if (o->kind != ObjKind::Hazard && o->kind != ObjKind::Orb) continue;
            double dx = static_cast<double>(o->rx) - xFrom;
            if (!std::isfinite(dx)) continue;
            if (first < 0.0 || dx < first) first = dx;
        }
        if (first >= 0.0) {
            int si = static_cast<int>(cut.speed);
            if (si < 0 || si > 4) si = 1;
            double units = cfg.speedUnitsPerSecond[si] > 0.0 ? cfg.speedUnitsPerSecond[si] : 311.58;
            preview = std::min(cap, std::max(0.0, first) / units);
        }
    }
    v.previewSeconds = preview;

    v.decoDensity = round6(v.decoDensity, cfg.featureDecimals);
    v.obstruction = round6(clamp01(v.obstruction), cfg.featureDecimals);
    v.clutter = round6(clamp01(v.clutter), cfg.featureDecimals);
    v.fakeObjects = round6(v.fakeObjects, cfg.featureDecimals);
    v.hiddenGameplay = round6(v.hiddenGameplay, cfg.featureDecimals);
    v.flashes = round6(v.flashes, cfg.featureDecimals);
    v.cameraEffects = round6(v.cameraEffects, cfg.featureDecimals);
    v.glowShare = round6(clamp01(v.glowShare), cfg.featureDecimals);
    v.opacityLow = round6(clamp01(v.opacityLow), cfg.featureDecimals);
    v.previewSeconds = round6(v.previewSeconds, cfg.featureDecimals);
    v.indicators = round6(v.indicators, cfg.featureDecimals);
    return v;
}

std::string presentationFingerprint(std::vector<SectionIdentity> const& sections, IdentityConfig const& cfg) {
    uint64_t h = 14695981039346656037ull;
    auto mix = [&h](int64_t v) {
        unsigned char bytes[8];
        uint64_t u = static_cast<uint64_t>(v);
        for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>((u >> (8 * i)) & 0xff);
        h = fnv1a64(bytes, 8, h);
    };
    mix(static_cast<int64_t>(sections.size()));
    for (auto const& s : sections) {
        auto const& p = s.presentation;
        double f[kPresentationFeatures] = {p.decoDensity, p.obstruction, p.clutter, p.fakeObjects, p.hiddenGameplay, p.flashes,
                                           p.cameraEffects, p.glowShare, p.opacityLow, p.previewSeconds, p.indicators};
        for (double x : f) mix(quantize(x, 1.0 / cfg.digestQuantum));
    }
    return hex16(h);
}

}  // namespace gprl::identity
