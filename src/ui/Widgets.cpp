#include "Widgets.hpp"

#include <algorithm>
#include <cmath>

#include "../../core/vocab.hpp"

using namespace geode::prelude;

namespace gprl::ui {

CCLabelBMFont* text(CCNode* parent, std::string const& str, float x, float y, float maxWidth, ccColor3B color, float scale, char const* font, CCPoint anchor,
                    float minScale) {
    auto label = CCLabelBMFont::create(str.c_str(), font);
    label->setScale(scale);
    if (maxWidth > 0.f) label->limitLabelWidth(maxWidth, scale, minScale);
    label->setAnchorPoint(anchor);
    label->setPosition({x, y});
    label->setColor(color);
    parent->addChild(label);
    return label;
}

CCScale9Sprite* panel(CCNode* parent, float x, float y, float w, float h, ccColor3B tint, GLubyte opacity, int z) {
    auto bg = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
    bg->setContentSize({std::max(8.f, w), std::max(8.f, h)});
    bg->setAnchorPoint({0.f, 0.f});
    bg->setPosition({x, y});
    bg->setColor(tint);
    bg->setOpacity(opacity);
    parent->addChild(bg, z);
    return bg;
}

CCNode* card(CCNode* parent, float x, float y, float w, float h, char const* title, ccColor3B tint, GLubyte opacity) {
    auto node = CCNode::create();
    node->setContentSize({w, h});
    node->setAnchorPoint({0.f, 0.f});
    node->setPosition({x, y});
    panel(node, 0.f, 0.f, w, h, tint, opacity, -1);
    if (title && *title) text(node, title, theme::kPad, h - 5.f, w - 2.f * theme::kPad, theme::kGold, 0.42f, theme::kGoldFont);
    parent->addChild(node);
    return node;
}

CCSprite* modSprite(std::string const& file) {
    std::string name = Mod::get()->expandSpriteName(file);
    if (auto* s = CCSprite::create(name.c_str())) return s;
    if (auto* frame = CCSpriteFrameCache::sharedSpriteFrameCache()->spriteFrameByName(name.c_str())) return CCSprite::createWithSpriteFrame(frame);
    return nullptr;
}

CCSprite* frameSprite(char const* name) {
    if (!name || !*name) return nullptr;
    auto* frame = CCSpriteFrameCache::sharedSpriteFrameCache()->spriteFrameByName(name);
    if (!frame) return nullptr;
    return CCSprite::createWithSpriteFrame(frame);
}

CCSprite* icon(char const* name, float size) {
    auto* s = frameSprite(name);
    if (!s) return nullptr;
    float big = std::max(s->getContentSize().width, s->getContentSize().height);
    if (big > 0.f) s->setScale(size / big);
    return s;
}

CCNode* gamemodeIcon(int gamemode, float size, bool lit) {
    static char const* const kOff[kGamemodeCount] = {"gj_iconBtn_off_001.png", "gj_shipBtn_off_001.png",  "gj_ballBtn_off_001.png",   "gj_birdBtn_off_001.png",
                                                      "gj_dartBtn_off_001.png", "gj_robotBtn_off_001.png", "gj_spiderBtn_off_001.png", "gj_swingBtn_off_001.png"};
    static char const* const kOn[kGamemodeCount] = {"gj_iconBtn_on_001.png", "gj_shipBtn_on_001.png",  "gj_ballBtn_on_001.png",   "gj_birdBtn_on_001.png",
                                                     "gj_dartBtn_on_001.png", "gj_robotBtn_on_001.png", "gj_spiderBtn_on_001.png", "gj_swingBtn_on_001.png"};
    int i = std::clamp(gamemode, 0, kGamemodeCount - 1);
    if (auto* s = icon(lit ? kOn[i] : kOff[i], size)) {
        if (!lit) s->setOpacity(170);
        return s;
    }
    auto label = CCLabelBMFont::create(std::string(name(static_cast<Gamemode>(i))).c_str(), theme::kChat);
    label->setScale(size / 40.f);
    label->setColor(lit ? theme::kWhite : theme::kDim);
    return label;
}

CCNode* badge(ranks::Rank const* rank, int division, float size, bool dim) {
    auto node = CCNode::create();
    node->setContentSize({size, size});
    node->setAnchorPoint({0.5f, 0.5f});
    std::string file = rank ? "rank_" + rank->iconKey + ".png" : std::string("rank_locked.png");
    CCSprite* sprite = modSprite(file);
    if (!sprite && rank && rank->kind == ranks::Kind::Ascendant) sprite = modSprite("rank_ascendant.png");
    if (sprite) {
        float w = std::max(sprite->getContentSize().width, 1.f);
        sprite->setScale(size / w);
        sprite->setPosition({size / 2.f, size / 2.f});
        if (dim) sprite->setOpacity(110);
        node->addChild(sprite);
    }
    else {
        ranks::Color c = rank ? rank->color : ranks::Color{110, 110, 120};
        if (dim) c = ranks::scaled(c, 0.55);
        ranks::Color edge = ranks::scaled(c, 0.55);
        auto draw = CCDrawNode::create();
        float r = size * 0.47f;
        CCPoint verts[6];
        for (int i = 0; i < 6; ++i) {
            float a = theme::kPi / 3.f * static_cast<float>(i) + theme::kPi / 6.f;
            verts[i] = CCPoint{size / 2.f + r * std::cos(a), size / 2.f + r * std::sin(a)};
        }
        draw->drawPolygon(verts, 6, ccc4f(c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f), std::max(1.f, size * 0.03f),
                          ccc4f(edge.r / 255.f, edge.g / 255.f, edge.b / 255.f, 1.f));
        node->addChild(draw);
        if (!rank) {
            auto q = CCLabelBMFont::create("?", theme::kBig);
            q->setScale(size / 120.f * 0.9f);
            q->setPosition({size / 2.f, size / 2.f});
            q->setOpacity(180);
            node->addChild(q);
        }
    }
    std::string numeral = rank ? ranks::badgeNumeral(*rank, division) : std::string();
    if (!numeral.empty()) {
        auto label = CCLabelBMFont::create(numeral.c_str(), theme::kBig);
        float scale = size / 120.f * 0.5f;
        label->setScale(scale);
        auto pill = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        pill->setColor({0, 0, 0});
        pill->setOpacity(dim ? 90 : 160);
        pill->setContentSize({label->getScaledContentSize().width + size * 0.12f, label->getScaledContentSize().height + size * 0.05f});
        pill->setPosition({size / 2.f, size * 0.19f});
        label->setPosition({size / 2.f, size * 0.19f});
        if (dim) label->setOpacity(140);
        node->addChild(pill, 2);
        node->addChild(label, 3);
    }
    return node;
}

namespace {

/// Draws the ring part between angles a0 and a1 (radians, counter-clockwise from +x) as quads.
void arc(CCDrawNode* draw, float cx, float cy, float rIn, float rOut, float a0, float a1, ccColor4F color) {
    if (a1 <= a0) return;
    int segments = std::max(2, static_cast<int>(std::ceil((a1 - a0) / (theme::kPi / 36.f))));
    float step = (a1 - a0) / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        float s = a0 + step * static_cast<float>(i);
        float e = s + step + 0.004f;   // a hair of overlap hides seams between quads
        CCPoint v[4] = {{cx + rIn * std::cos(s), cy + rIn * std::sin(s)},
                        {cx + rOut * std::cos(s), cy + rOut * std::sin(s)},
                        {cx + rOut * std::cos(e), cy + rOut * std::sin(e)},
                        {cx + rIn * std::cos(e), cy + rIn * std::sin(e)}};
        draw->drawPolygon(v, 4, color, 0.f, color);
    }
}

ccColor4F toF(ccColor3B c, GLubyte a = 255) { return ccc4f(c.r / 255.f, c.g / 255.f, c.b / 255.f, a / 255.f); }

}  // namespace

CCNode* ring(float radius, float thickness, double fraction, ccColor3B color, ccColor3B track, GLubyte trackOpacity) {
    auto node = CCNode::create();
    node->setContentSize({2.f * radius, 2.f * radius});
    node->setAnchorPoint({0.5f, 0.5f});
    auto draw = CCDrawNode::create();
    float rOut = radius, rIn = std::max(1.f, radius - thickness);
    arc(draw, radius, radius, rIn, rOut, 0.f, 2.f * theme::kPi, toF(track, trackOpacity));
    double f = std::clamp(fraction, 0.0, 1.0);
    if (f > 0.0) {
        // clockwise from the top: cocos angles run counter-clockwise, so draw from (90deg - sweep) to 90deg
        float sweep = static_cast<float>(f) * 2.f * theme::kPi;
        float top = theme::kPi / 2.f;
        arc(draw, radius, radius, rIn, rOut, top - sweep, top, toF(color));
    }
    node->addChild(draw);
    return node;
}

CCNode* bar(float w, float h, double fraction, ccColor3B fill, ccColor3B track, GLubyte trackOpacity) {
    auto node = CCNode::create();
    node->setContentSize({w, h});
    node->setAnchorPoint({0.f, 0.5f});
    auto bg = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
    bg->setContentSize({w, h});
    bg->setAnchorPoint({0.f, 0.f});
    bg->setPosition({0.f, 0.f});
    bg->setColor(track);
    bg->setOpacity(trackOpacity);
    node->addChild(bg);
    double f = std::clamp(fraction, 0.0, 1.0);
    if (f > 0.0) {
        auto fg = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        fg->setContentSize({std::max(h, static_cast<float>(w * f)), h});
        fg->setAnchorPoint({0.f, 0.f});
        fg->setPosition({0.f, 0.f});
        fg->setColor(fill);
        node->addChild(fg, 1);
    }
    return node;
}

CCNode* chip(std::string const& label, ccColor3B background, ccColor3B foreground, float scale, GLubyte opacity) {
    auto node = CCNode::create();
    auto l = CCLabelBMFont::create(label.c_str(), theme::kBig);
    l->setScale(scale);
    l->setColor(foreground);
    float w = l->getScaledContentSize().width + 10.f;
    float h = l->getScaledContentSize().height + 4.f;
    node->setContentSize({w, h});
    node->setAnchorPoint({0.f, 0.5f});
    auto bg = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
    bg->setContentSize({w, h});
    bg->setAnchorPoint({0.f, 0.f});
    bg->setPosition({0.f, 0.f});
    bg->setColor(background);
    bg->setOpacity(opacity);
    node->addChild(bg);
    l->setPosition({w / 2.f, h / 2.f});
    node->addChild(l, 1);
    return node;
}

CCNode* tile(float w, float h, std::string const& value, std::string const& caption, ccColor3B valueColor, ccColor3B tint) {
    auto node = CCNode::create();
    node->setContentSize({w, h});
    node->setAnchorPoint({0.f, 0.f});
    panel(node, 0.f, 0.f, w, h, tint, 220, -1);
    text(node, value, w / 2.f, h * 0.62f, w - 8.f, valueColor, 0.44f, theme::kBig, {0.5f, 0.5f});
    text(node, caption, w / 2.f, h * 0.24f, w - 6.f, theme::kGrey, 0.32f, theme::kChat, {0.5f, 0.5f});
    return node;
}

SimpleTextArea* paragraph(CCNode* parent, std::string const& str, float x, float topY, float width, float scale, ccColor3B color, char const* font) {
    auto area = SimpleTextArea::create(str, font, scale, width);
    area->setAnchorPoint({0.f, 1.f});
    area->setPosition({x, topY});
    area->setColor({color.r, color.g, color.b, 255});
    parent->addChild(area);
    return area;
}

CCMenuItemSpriteExtra* button(CCMenu* menu, char const* label, char const* texture, float scale, CCObject* target, SEL_MenuHandler handler, int tag) {
    auto spr = ButtonSprite::create(label, theme::kGoldFont, texture, scale);
    auto btn = CCMenuItemSpriteExtra::create(spr, target, handler);
    btn->setTag(tag);
    menu->addChild(btn);
    return btn;
}

CCMenuItemSpriteExtra* wideButton(CCMenu* menu, char const* label, int width, char const* texture, float scale, CCObject* target, SEL_MenuHandler handler, int tag) {
    auto spr = ButtonSprite::create(label, width, true, theme::kGoldFont, texture, 28.f, scale);
    auto btn = CCMenuItemSpriteExtra::create(spr, target, handler);
    btn->setTag(tag);
    menu->addChild(btn);
    return btn;
}

CCMenuItemSpriteExtra* iconButton(CCMenu* menu, char const* frame, float size, CCObject* target, SEL_MenuHandler handler, int tag) {
    CCNode* spr = icon(frame, size);
    if (!spr) {
        auto l = CCLabelBMFont::create("?", theme::kBig);
        l->setScale(size / 30.f);
        spr = l;
    }
    auto btn = CCMenuItemSpriteExtra::create(spr, target, handler);
    btn->setTag(tag);
    menu->addChild(btn);
    return btn;
}

void setButtonEnabled(CCMenuItemSpriteExtra* btn, bool enabled) {
    if (!btn) return;
    btn->setEnabled(enabled);
    GLubyte const a = enabled ? 255 : 110;
    btn->setOpacity(a);
    // the ButtonSprite's parts do not follow the item's opacity on their own
    if (auto* spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
        spr->setOpacity(a);
        if (spr->m_BGSprite) spr->m_BGSprite->setOpacity(a);
        if (spr->m_label) spr->m_label->setOpacity(a);
        if (spr->m_subSprite) spr->m_subSprite->setOpacity(a);
    }
    else if (auto* spr = typeinfo_cast<CCSprite*>(btn->getNormalImage())) spr->setOpacity(a);
}

std::string percent(double v) {
    if (!std::isfinite(v)) v = 0.0;
    return fmt::format("{}%", static_cast<int>(std::floor(std::clamp(v, 0.0, 1.0) * 100.0 + 1e-9)));
}

std::string seconds(double s) {
    if (!std::isfinite(s) || s < 0.0) s = 0.0;
    if (s < 60.0) return fmt::format("{:.1f} s", s);
    int total = static_cast<int>(std::floor(s));
    int m = total / 60, sec = total % 60;
    if (m >= 60) return fmt::format("{}:{:02}:{:02}", m / 60, m % 60, sec);
    return fmt::format("{}:{:02}", m, sec);
}

}  // namespace gprl::ui
