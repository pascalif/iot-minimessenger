#ifndef DISPLAY_H
#define DISPLAY_H

// Fixed-size storage for one conversation entry. Putting these in char[] (BSS)
// instead of String (heap) is what makes the conversation buffer immune to
// heap fragmentation — see MEM-001 in docs/audit_claude.md.
#define CONVO_TS_MAX_LEN  20   // "YYYY-MM-DD|HH:MM:SS\0" is 20 bytes
#define CONVO_MSG_MAX_LEN 128  // a bit above MAX_SERIAL_MSG_LENGTH (100)

class TextLine {
public:
  char ts[CONVO_TS_MAX_LEN];   // empty when ts[0] == '\0'
  uint16_t tsColor;
  const GFXfont* tsFont;
  byte tsFontSize;
  byte tsHeightWithBottomMargin;  // avec marge
  int16_t tsBounds[4];            // {x1, y1, w, h} renvoyés par getTextBounds
  uint16_t tsX;

  char msg[CONVO_MSG_MAX_LEN];
  uint16_t msgColor;
  const GFXfont* msgFont;
  byte msgFontSize;
  byte msgHeightWithBottomMargin;  // avec marge
  int16_t msgBounds[4];            // {x1, y1, w, h} renvoyés par getTextBounds
  uint16_t msgX;

  TextLine() {
    ts[0] = '\0';
    msg[0] = '\0';
  }

  TextLine(const String& _ts, uint16_t _tsColor, const GFXfont* _tsFont, byte _tsFontSize, byte _tsHeightWithBottomMargin, uint16_t _tsX,
           int16_t _tsBox[4],
           const String& _msg, uint16_t _msgColor, const GFXfont* _msgFont, byte _msgFontSize, byte _msgHeightWithBottomMargin, uint16_t _msgX,
           int16_t _msgBox[4])
    : tsColor(_tsColor), tsFont(_tsFont), tsFontSize(_tsFontSize), tsHeightWithBottomMargin(_tsHeightWithBottomMargin), tsX(_tsX),
      msgColor(_msgColor), msgFont(_msgFont), msgFontSize(_msgFontSize), msgHeightWithBottomMargin(_msgHeightWithBottomMargin), msgX(_msgX) {
    strncpy(ts, _ts.c_str(), CONVO_TS_MAX_LEN - 1);
    ts[CONVO_TS_MAX_LEN - 1] = '\0';
    strncpy(msg, _msg.c_str(), CONVO_MSG_MAX_LEN - 1);
    msg[CONVO_MSG_MAX_LEN - 1] = '\0';

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
