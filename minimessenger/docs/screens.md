# Splash screen


# Info screen


# Conversation screen

Screen layout (portrait, 240×320, see docs/howto_hardware_scrolling.md):
VSCRDEF = Vertical Scroll Definition =  TFA (Top Fixed Area) + VSA (Vertical Scroll Area) + (BFA Bottom Fixed Area)

+---------------------------------------------+   ← y=0
|  ● ● ●                                ●     |   STATUS_BAR (25 px, VSCRDEF TFA)
|  WiFi BT MQTT                       Contact |   filled/empty per state
|  (wh) (bl) (yel)                    (red)   |
|.............................................|   ← y=23 (light gray hairline)
|                                             |   ← y=24 (1 px black breathing gap)
+---------------------------------------------+   ← y=25
|                                             |
|                                             |
|   Conversation messages — HW scroll up      |   SCROLL_AREA (272 px, VSCRDEF VSA)
|   (newest at the bottom, older above)       |   VSCSAD bumped on each new line
|                                             |
|                                             |
|                                             |
|                                             |         FOOTER (19 px, VSCRDEF BFA)
|                                             |   ← y=301 (1 px black, aerates against scroll area)
|---------------------------------------------|   ← y=302 (light gray hairline)
|                                             |   ← y=303 (1 px black margin)
+                     typed_msg_buffer_here | +   ← y=304..319 (size-2 text = 16 pixels ; yellow cursor bar)
