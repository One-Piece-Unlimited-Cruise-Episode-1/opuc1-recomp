// A stroke font for the Enemy AI view's labels (not in the game): each glyph is a list of segments on a 4 x 6 grid
// (origin bottom left), four digits per segment "x0 y0 x1 y1". Lower case is drawn as upper case.
#pragma once

namespace OpucAiView {

inline const char* GlyphStrokes(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    switch (c) {
        case 'A': return "0004042626444440" "0343";
        case 'B': return "000606363645453303333342424141303000";
        case 'C': return "460606000040";
        case 'D': return "0006063636444442" "4230" "3000";
        case 'E': return "4606060000400333";
        case 'F': return "460606000333";
        case 'G': return "46060600004040434322";
        case 'H': return "000640460343";
        case 'I': return "064626200040";
        case 'J': return "464040000002";
        case 'K': return "000646030340";
        case 'L': return "06000040";
        case 'M': return "0006062323464640";
        case 'N': return "000606404046";
        case 'O': return "0006064646404000";
        case 'P': return "0006064646434303";
        case 'Q': return "0006064646404000" "2240";
        case 'R': return "0006064646434303" "1340";
        case 'S': return "4606060303434340" "4000";
        case 'T': return "06462620";
        case 'U': return "060000404046";
        case 'V': return "06202046";
        case 'W': return "0610102323303046";
        case 'X': return "00460640";
        case 'Y': return "062346232320";
        case 'Z': return "064646000040";
        case '0': return "0006064646404000" "0046";
        case '1': return "152626201030";
        case '2': return "0646464343030300" "0040";
        case '3': return "0646464040001343";
        case '4': return "060303434640";
        case '5': return "4606060303434340" "4000";
        case '6': return "4606060000404043" "4303";
        case '7': return "06464610";
        case '8': return "0006064646404000" "0343";
        case '9': return "4303030606464640" "4000";
        case '-': return "1333";
        case '.': return "2021";
        case '>': return "06434300";
        case '(': return "361414121230";
        case ')': return "163434323210";
        case '%': return "004605161041";
        case '/': return "0046";
        case ':': return "21222425";
        case '=': return "12321434";
        default: return "";
    }
}

}  // namespace OpucAiView
