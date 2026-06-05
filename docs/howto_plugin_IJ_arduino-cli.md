
https://plugins.jetbrains.com/plugin/31027-flexible-for-arduino


❯ arduino-cli board listall esp32
...
ESP32 Dev Module                            esp32:esp32:esp32
...



# arduino-cli


pascal@valhalla  ~/.cache/arduino/sketches/67D5B1C4B0DD1CD7745F63423F613F25 ························································································· 16:34:44
❯ cat partitions.csv
───────┬─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
│ File: partitions.csv
───────┼─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
1   │ # Name,   Type, SubType, Offset,  Size, Flags
2   │ nvs,      data, nvs,     0x9000,  0x5000,
3   │ otadata,  data, ota,     0xe000,  0x2000,
4   │ app0,     app,  ota_0,   0x10000, 0x300000,
5   │ spiffs,   data, spiffs,  0x310000,0xE0000,
6   │ coredump, data, coredump,0x3F0000,0x10000,
───────┴─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────


## Compile
❯ /home/pascal/Cmd/arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app --verbose /home/pascal/Dev/workspace_pascal/iot-minimessenger/minimessenger


Avec fichier partitions.csv
❯ /home/pascal/Cmd/arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=custom --verbose /home/pascal/Dev/workspace_pascal/iot-minimessenger/minimessenger


TODO
Using library Hash at version 3.3.8 in folder: /home/pascal/.arduino15/packages/esp32/hardware/esp32/3.3.8/libraries/Hash
/home/pascal/.arduino15/packages/esp32/tools/esp-x32/2601/bin/xtensa-esp32-elf-size -A /home/pascal/.cache/arduino/sketches/67D5B1C4B0DD1CD7745F63423F613F25/minimessenger.ino.elf
Sketch uses 1634793 bytes (9%) of program storage space. Maximum is 16777216 bytes.
Global variables use 64400 bytes (19%) of dynamic memory, leaving 263280 bytes for local variables. Maximum is 327680 bytes.


## Flash

/home/pascal/Cmd/arduino-cli compile --upload --fqbn esp32:esp32:esp32:PartitionScheme=custom --port /dev/ttyUSB0 /home/pascal/Dev/workspace_pascal/iot-minimessenger/minimessenger
