#pragma once
// The GPRL menu's look (v0.13.0 redesign): one place for the sizes, colours and fonts every
// screen uses. GD's bitmap fonts have no Greek glyphs or en dashes, so every text here writes
// "sigma/s" and "0-20".
//
// Layout (m_mainLayer coordinates of the 480 x 300 popup, y up):
//   sidebar   x 8..84   : five icon tiles (Home, Ranks, Board, Level, Account) + the version
//   header    y 268..296: the screen title (left) and the status chips (right)
//   content   x 92..472, y 12..264 (380 x 252): the screen, drawn in its own local coordinates
//
// Type scale (owner feedback 2026-10-03: "some text is too small"). GD's fonts at scale 1 are
// chatFont 18 px tall, goldFont 29, bigFont 35, so chatFont below 0.4 is under 7 px and hard to
// read. Body text is 9 px; nothing a player has to read is smaller than 7 px, and a label that
// does not fit shrinks to 80 % at most before it is cut with "..." (Widgets `text`); longer texts
// wrap (`paragraph`).
#include <Geode/Geode.hpp>

namespace gprl::ui::theme {

using cocos2d::ccColor3B;

constexpr float kWidth = 480.f;
constexpr float kHeight = 300.f;

constexpr float kSideX = 8.f;
constexpr float kSideW = 76.f;
constexpr float kSideTop = 266.f;     // top of the first tile (clear of the close button)
constexpr float kTileW = 64.f;
constexpr float kTileH = 42.f;
constexpr float kTileGap = 4.f;

constexpr float kContentX = 92.f;
constexpr float kContentY = 12.f;
constexpr float kContentW = 380.f;
constexpr float kContentH = 252.f;
constexpr float kHeaderY = 281.f;     // centre line of the header strip
constexpr float kPad = 8.f;           // inner padding of cards
constexpr float kGap = 5.f;           // margin around / between cards on a screen

// chatFont scales (18 px at 1.0)
constexpr float kBody = 0.5f;         // 9 px: what the player reads first
constexpr float kSmall = 0.44f;       // 8 px: supporting lines
constexpr float kTiny = 0.4f;         // 7 px: the least important line of a card
constexpr float kLine = 11.f;         // distance between two body lines
constexpr float kLineSmall = 10.f;
constexpr float kTitle = 0.44f;       // goldFont card titles (13 px)
constexpr float kTitleH = 18.f;       // room a card title takes from the card's top
constexpr float kMinShrink = 0.8f;    // a label that does not fit shrinks to this share, then is cut

// colours (the popup background is GD's blue square; panels are dark navy)
constexpr ccColor3B kWhite{255, 255, 255};
constexpr ccColor3B kGrey{178, 184, 200};
constexpr ccColor3B kDim{118, 124, 142};
constexpr ccColor3B kGreen{120, 232, 140};
constexpr ccColor3B kCyan{92, 206, 255};
constexpr ccColor3B kGold{255, 216, 120};
constexpr ccColor3B kOrange{255, 160, 100};
constexpr ccColor3B kRed{255, 110, 110};
constexpr ccColor3B kBlue{110, 150, 230};
constexpr ccColor3B kPurple{190, 140, 255};
constexpr ccColor3B kGreyBlue{150, 175, 215};   // the player's own private sigma/s (not a rating)
constexpr ccColor3B kPanel{12, 16, 32};         // sidebar / content background
constexpr ccColor3B kCard{30, 38, 66};          // cards inside the content
constexpr ccColor3B kCardLight{44, 54, 90};
constexpr ccColor3B kCardSelected{40, 118, 172};
constexpr ccColor3B kLiveRed{230, 40, 40};

constexpr char const* kBig = "bigFont.fnt";
constexpr char const* kGoldFont = "goldFont.fnt";
constexpr char const* kChat = "chatFont.fnt";

constexpr float kPi = 3.14159265358979f;

}  // namespace gprl::ui::theme
