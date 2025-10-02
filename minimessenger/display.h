#ifndef DISPLAY_H
#define DISPLAY_H

class TextLine {
public:
  String ts;  // timestamp is optional
  uint16_t tsColor;
  const GFXfont* tsFont;
  byte tsFontSize;
  byte tsHeightWithBottomMargin;  // avec marge
  int16_t tsBounds[4];            // {x1, y1, w, h} renvoyés par getTextBounds
  uint16_t tsX;

  String msg;
  uint16_t msgColor;
  const GFXfont* msgFont;
  byte msgFontSize;
  byte msgHeightWithBottomMargin;  // avec marge
  int16_t msgBounds[4];            // {x1, y1, w, h} renvoyés par getTextBounds
  uint16_t msgX;

  TextLine() {}
  TextLine(String _ts, uint16_t _tsColor, const GFXfont* _tsFont, byte _tsFontSize, byte _tsHeightWithBottomMargin, uint16_t _tsX,
           int16_t _tsBox[4],
           String msg, uint16_t _msgColor, const GFXfont* _msgFont, byte _msgFontSize, byte _msgHeightWithBottomMargin, uint16_t _msgX,
           int16_t _msgBox[4])
    : ts(_ts), tsColor(_tsColor), tsFont(_tsFont), tsFontSize(_tsFontSize), tsHeightWithBottomMargin(_tsHeightWithBottomMargin), tsX(_tsX),
      msg(msg), msgColor(_msgColor), msgFont(_msgFont), msgFontSize(_msgFontSize), msgHeightWithBottomMargin(_msgHeightWithBottomMargin),    msgX(_msgX) {
    tsBounds[0] = _tsBox[0];
    tsBounds[1] = _tsBox[1];
    tsBounds[2] = _tsBox[2];
    tsBounds[3] = _tsBox[3];
    msgBounds[0] = _msgBox[0];
    msgBounds[1] = _msgBox[1];
    msgBounds[2] = _msgBox[2];
    msgBounds[3] = _msgBox[3];
  }
};

#endif