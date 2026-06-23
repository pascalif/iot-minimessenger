#ifndef SYMBOLS_H
#define SYMBOLS_H


// Origin of a payload landing in processMessage(): drives which side of the
// conversation it renders on and whether it gets republished to peers.
enum class MessageSource {
    REMOTE,  // arrived via MQTT — render LEFT, do NOT republish
    LOCAL,   // typed locally (serial today, BLE keyboard later) — render RIGHT,
             // publish
};


// Logging categories
#define TAG_MM   "MM__"
#define TAG_WIFI "WIFI"
#define TAG_MQTT "MQTT"
#define TAG_BTKB "BTKB"


#endif