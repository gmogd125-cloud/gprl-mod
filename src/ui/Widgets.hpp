#pragma once
// Small reusable pieces of the GPRL menu (v0.13.0 redesign): labels that never shrink into
// unreadable sizes, wrapped paragraphs, tinted rounded panels ("cards"), rank badges, ring gauges
// and bars, status chips, stat tiles, buttons of an exact height, and icon lookups that never
// dereference a missing sprite frame. Pure cocos / Geode drawing; nothing here reads game state.
#include <Geode/Geode.hpp>
#include <Geode/ui/TextArea.hpp>

#include <string>
#include <vector>

#include "../../core/ranks.hpp"
#include "Theme.hpp"

namespace gprl::ui {

using namespace cocos2d;
using namespace cocos2d::extension;

/// A one-line label in `parent`. `anchor` defaults to top-left. When the text is wider than
/// `maxWidth` it shrinks, but never below `minScale` (0 = 80 % of `scale`, theme::kMinShrink);
/// what still does not fit is cut with "..." so the text stays readable.
CCLabelBMFont* text(CCNode* parent, std::string const& str, float x, float y, float maxWidth, ccColor3B color = theme::kWhite, float scale = theme::kBody,
                    char const* font = theme::kChat, CCPoint anchor = {0.f, 1.f}, float minScale = 0.f);
/// The fitting rule of `text` for a label that already exists (labels updated in place).
void fitLabel(CCLabelBMFont* label, std::string const& str, float maxWidth, float scale, float minScale = 0.f);

/// A word-wrapped paragraph (never shrunk) whose top-left is at (x, topY); `maxLines` > 0 cuts
/// the rest with "...". getHeight() of the returned area is the rendered height.
geode::SimpleTextArea* paragraph(CCNode* parent, std::string const& str, float x, float topY, float width, float scale = theme::kSmall,
                                 ccColor3B color = theme::kWhite, size_t maxLines = 0, CCTextAlignment align = kCCTextAlignmentLeft,
                                 char const* font = theme::kChat);
/// The same paragraph without a parent yet: measure it (getHeight()), then `place` it.
geode::SimpleTextArea* makeParagraph(std::string const& str, float width, float scale = theme::kSmall, ccColor3B color = theme::kWhite, size_t maxLines = 0,
                                     CCTextAlignment align = kCCTextAlignmentLeft, char const* font = theme::kChat);
void place(CCNode* parent, geode::SimpleTextArea* area, float x, float topY);

/// A rounded rectangle of exactly (w, h) points in `tint`: the square02b_001.png nine-slice built
/// large and scaled down when a side is short, so its corners never overlap (a nine-slice smaller
/// than its own corners draws stray lines). Anchor (0, 0); not added to a parent.
CCScale9Sprite* roundRect(float w, float h, ccColor3B tint, GLubyte opacity = 255);
/// `roundRect` added to `parent` with its bottom-left at (x, y).
CCScale9Sprite* panel(CCNode* parent, float x, float y, float w, float h, ccColor3B tint, GLubyte opacity = 255, int z = 0);

/// A card: a tinted panel plus an optional gold title at its top-left (it takes theme::kTitleH
/// from the top). Returns the card node (content size w x h at (x, y), children in card-local
/// coordinates).
CCNode* card(CCNode* parent, float x, float y, float w, float h, char const* title = nullptr, ccColor3B tint = theme::kCard, GLubyte opacity = 230);

/// A sprite of a mod resource ("rank_gold.png", expanded with Mod::expandSpriteName) or a sheet
/// frame of that name; nullptr when neither exists.
CCSprite* modSprite(std::string const& file);
/// A sprite frame of GD's own sheets ("GJ_infoIcon_001.png"); nullptr when the frame is unknown
/// (never createWithSpriteFrameName on an unknown name: that dereferences the missing frame).
CCSprite* frameSprite(char const* name);
/// `frameSprite` scaled so its larger side is `size` points; nullptr when missing.
CCSprite* icon(char const* name, float size);
/// The gamemode's garage button icon (gj_shipBtn_off_001.png ...) at `size`, or a small label.
CCNode* gamemodeIcon(int gamemode, float size, bool lit);

/// Rank badge of `size` points (rank_<iconKey>.png; rank_locked.png without a rank; a hexagon in
/// the rank colour when the sprite is missing) with the division / tier numeral at its bottom.
/// Anchor (0.5, 0.5), content size (size, size). `dim` for unreached tiers.
CCNode* badge(ranks::Rank const* rank, int division, float size, bool dim = false);

/// A ring gauge: a dark track ring and a coloured arc covering `fraction` of it, clockwise from
/// the top. Anchor (0.5, 0.5), content size (2 radius)^2; put labels in its centre.
CCNode* ring(float radius, float thickness, double fraction, ccColor3B color, ccColor3B track = {0, 0, 0}, GLubyte trackOpacity = 150);

/// A horizontal bar: dark track, coloured fill of `fraction`. Anchor (0, 0.5), content size (w, h).
CCNode* bar(float w, float h, double fraction, ccColor3B fill, ccColor3B track = {0, 0, 0}, GLubyte trackOpacity = 150);

/// A small pill with a short word in it ("LIVE", "Connected", "Practice"). Anchor (0, 0.5);
/// content size follows the text.
CCNode* chip(std::string const& label, ccColor3B background, ccColor3B foreground = theme::kWhite, float scale = 0.3f, GLubyte opacity = 230);

/// A stat tile: a big value over a caption on a light card. Anchor (0, 0), size (w, h).
CCNode* tile(float w, float h, std::string const& value, std::string const& caption, ccColor3B valueColor = theme::kWhite, ccColor3B tint = theme::kCardLight);

// ---- buttons ----
// GD's ButtonSprite only scales its LABEL with the `scale` argument; the button itself stays
// about 30 points high. These helpers therefore scale the whole sprite to an exact `height`.

/// A GD text button `height` points high (the width follows the label). Anchor (0.5, 0.5).
CCMenuItemSpriteExtra* button(CCMenu* menu, char const* label, char const* texture, float height, CCObject* target, SEL_MenuHandler handler, int tag = 0);
/// A GD text button of exactly `width` x `height` points (the label shrinks to fit).
CCMenuItemSpriteExtra* wideButton(CCMenu* menu, char const* label, float width, char const* texture, float height, CCObject* target, SEL_MenuHandler handler,
                                  int tag = 0);
/// An icon-only button from a sheet frame (a "?" label when the frame is missing).
CCMenuItemSpriteExtra* iconButton(CCMenu* menu, char const* frame, float size, CCObject* target, SEL_MenuHandler handler, int tag = 0);
/// The size a button made here takes on screen.
CCSize sizeOf(CCMenuItemSpriteExtra* btn);

struct ButtonSpec {
    char const* label;
    char const* texture;
    SEL_MenuHandler handler;
    bool enabled = true;
};
/// A row of text buttons, all `height` high, their centres on `y`. They start at `x` and run
/// right, or end at `x` and run left when `fromRight` (first spec = rightmost). When the row
/// would be wider than `maxWidth` every button is scaled down together. Returns the buttons in
/// the order of `specs`; `usedWidth` (optional) receives the row's width.
std::vector<CCMenuItemSpriteExtra*> buttonRow(CCMenu* menu, std::vector<ButtonSpec> const& specs, float x, float y, float height, float maxWidth,
                                              CCObject* target, bool fromRight = false, float gap = 5.f, float* usedWidth = nullptr);
/// Dims and disables a button in one call.
void setButtonEnabled(CCMenuItemSpriteExtra* btn, bool enabled);

/// "42%" of a [0,1] value (floored, like the server's screens).
std::string percent(double v);
/// A duration in seconds as "1:05" / "12.3 s".
std::string seconds(double s);

}  // namespace gprl::ui
