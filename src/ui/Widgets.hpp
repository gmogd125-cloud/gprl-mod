#pragma once
// Small reusable pieces of the GPRL menu (v0.13.0 redesign): labels that clip to a width,
// tinted rounded panels ("cards"), rank badges, ring gauges and bars, status chips, stat tiles,
// icon lookups that never dereference a missing sprite frame, and wrapped paragraphs. Pure
// cocos / Geode drawing; nothing here reads game state.
#include <Geode/Geode.hpp>
#include <Geode/ui/TextArea.hpp>

#include <string>

#include "../../core/ranks.hpp"
#include "Theme.hpp"

namespace gprl::ui {

using namespace cocos2d;
using namespace cocos2d::extension;

/// A one-line label in `parent`. `anchor` defaults to top-left; the label is clipped to `maxWidth`
/// (shrinking down to `minScale` first, then cut with "..." by cocos).
CCLabelBMFont* text(CCNode* parent, std::string const& str, float x, float y, float maxWidth, ccColor3B color = theme::kWhite, float scale = 0.42f,
                    char const* font = theme::kChat, CCPoint anchor = {0.f, 1.f}, float minScale = 0.2f);

/// A rounded, tinted panel (square02b_001.png nine-slice) anchored bottom-left at (x, y).
CCScale9Sprite* panel(CCNode* parent, float x, float y, float w, float h, ccColor3B tint, GLubyte opacity = 255, int z = 0);

/// A card: a tinted panel plus an optional small gold title at its top-left. Returns the card node
/// (content size w x h, position (x, y), children in card-local coordinates).
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
CCNode* chip(std::string const& label, ccColor3B background, ccColor3B foreground = theme::kWhite, float scale = 0.28f, GLubyte opacity = 230);

/// A stat tile: a big value over a small caption on a light card. Anchor (0, 0), size (w, h).
CCNode* tile(float w, float h, std::string const& value, std::string const& caption, ccColor3B valueColor = theme::kWhite, ccColor3B tint = theme::kCardLight);

/// A word-wrapped paragraph whose top-left is at (x, topY). Returns the area (its getHeight() is
/// the rendered height).
geode::SimpleTextArea* paragraph(CCNode* parent, std::string const& str, float x, float topY, float width, float scale = 0.4f, ccColor3B color = theme::kWhite,
                                 char const* font = theme::kChat);

/// A GD text button (ButtonSprite on GJ_button_XX.png) added to `menu`. Anchor (0.5, 0.5).
CCMenuItemSpriteExtra* button(CCMenu* menu, char const* label, char const* texture, float scale, CCObject* target, SEL_MenuHandler handler, int tag = 0);
/// A fixed-width GD text button (the label shrinks to fit).
CCMenuItemSpriteExtra* wideButton(CCMenu* menu, char const* label, int width, char const* texture, float scale, CCObject* target, SEL_MenuHandler handler, int tag = 0);
/// An icon-only button from a sheet frame (a "?" label when the frame is missing).
CCMenuItemSpriteExtra* iconButton(CCMenu* menu, char const* frame, float size, CCObject* target, SEL_MenuHandler handler, int tag = 0);
/// Dims and disables a button in one call.
void setButtonEnabled(CCMenuItemSpriteExtra* btn, bool enabled);

/// "42%" of a [0,1] value (floored, like the server's screens).
std::string percent(double v);
/// A duration in seconds as "1:05" / "12.3 s".
std::string seconds(double s);

}  // namespace gprl::ui
