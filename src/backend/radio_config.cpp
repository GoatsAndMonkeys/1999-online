// radio_config.cpp — the Meshtastic settings table (see radio_config.h).
//
// Every row is `label, protobuf field name, protobuf field number, ...` copied
// from meshtastic/protobufs master (2026-11-20). Deprecated fields and a few
// types this table cannot hold yet are left out on purpose:
//   - float fields (LoRaConfig.frequency_offset / override_frequency,
//     PowerConfig.adc_multiplier_override),
//   - repeated / nested ones (LoRaConfig.ignore_incoming,
//     NetworkConfig.ipv4_config, SecurityConfig keys, RemoteHardware pins),
//   - the Audio, Remote Hardware, Status Message, Traffic Management, TAK and
//     Mesh Beacon module configs.
#include "radio_config.h"

#include "logger.h"

#include <algorithm>
#include <cstdio>

namespace mm {
namespace {

constexpr int64_t kU8 = 255;
constexpr int64_t kU16 = 65535;
constexpr int64_t kU32 = 4294967295LL;
constexpr int64_t kI32Min = -2147483647LL - 1;
constexpr int64_t kI32Max = 2147483647LL;
constexpr int64_t kGpioMax = 255;   // our limit; the protobufs give none

// ── Row builders ────────────────────────────────────────────────────────────

template <size_t N>
constexpr FieldDef En(const char* label, const char* pb, int num, const char* pb_type,
                      const EnumOption (&opts)[N], int def, const char* help = "") {
    FieldDef f{label, FieldType::Enum, pb, num, pb_type};
    f.options = opts;
    f.option_count = static_cast<int>(N);
    f.def_int = def;
    f.help = help;
    return f;
}

constexpr FieldDef Bo(const char* label, const char* pb, int num, bool def, const char* help = "") {
    FieldDef f{label, FieldType::Bool, pb, num, "bool"};
    f.max = 1;
    f.def_int = def ? 1 : 0;
    f.help = help;
    return f;
}

constexpr FieldDef In(const char* label, const char* pb, int num, const char* pb_type, int64_t min,
                      int64_t max, const char* unit, int64_t def, const char* help = "") {
    FieldDef f{label, FieldType::Int, pb, num, pb_type};
    f.min = min;
    f.max = max;
    f.unit = unit;
    f.def_int = def;
    f.help = help;
    return f;
}

constexpr FieldDef Gpio(const char* label, const char* pb, int num) {
    return In(label, pb, num, "uint32", 0, kGpioMax, "", 0, "GPIO pin number; 0 = board default / unused.");
}

constexpr FieldDef Tx(const char* label, const char* pb, int num, int max_len, const char* def,
                      const char* help = "", bool secret = false, const char* pb_type = "string") {
    FieldDef f{label, FieldType::Text, pb, num, pb_type};
    f.max_len = max_len;
    f.def_text = def;
    f.secret = secret;
    f.help = help;
    return f;
}

// AdminMessage command: `arg` is the value sent, `question` the confirmation.
constexpr FieldDef Ac(const char* label, const char* pb, int num, const char* pb_type, int64_t arg,
                      const char* question) {
    FieldDef f{label, FieldType::Action, pb, num, pb_type};
    f.def_int = arg;
    f.help = question;
    return f;
}

// The field lives in another message than the section's own.
constexpr FieldDef Via(FieldDef f, const char* carrier) {
    f.pb_carrier = carrier;
    return f;
}

// ── Enums (value, protobuf name, label) ─────────────────────────────────────

// Config.LoRaConfig.RegionCode
constexpr EnumOption kRegion[] = {
    {0, "UNSET", "Unset"},
    {1, "US", "United States"},
    {2, "EU_433", "EU 433 MHz"},
    {3, "EU_868", "EU 868 MHz"},
    {4, "CN", "China"},
    {5, "JP", "Japan"},
    {6, "ANZ", "Australia / Brazil / NZ"},
    {7, "KR", "Korea"},
    {8, "TW", "Taiwan"},
    {9, "RU", "Russia"},
    {10, "IN", "India"},
    {11, "NZ_865", "New Zealand 865 MHz"},
    {12, "TH", "Thailand"},
    {13, "LORA_24", "2.4 GHz"},
    {14, "UA_433", "Ukraine 433 MHz"},
    {15, "UA_868", "Ukraine 868 MHz (deprecated)"},
    {16, "MY_433", "Malaysia 433 MHz"},
    {17, "MY_919", "Malaysia 919 MHz"},
    {18, "SG_923", "Singapore 923 MHz"},
    {19, "PH_433", "Philippines 433 MHz"},
    {20, "PH_868", "Philippines 868 MHz"},
    {21, "PH_915", "Philippines 915 MHz"},
    {22, "ANZ_433", "Australia / NZ 433 MHz"},
    {23, "KZ_433", "Kazakhstan 433 MHz"},
    {24, "KZ_863", "Kazakhstan 863 MHz"},
    {25, "NP_865", "Nepal 865 MHz"},
    {26, "BR_902", "Brazil 902 MHz"},
    {27, "ITU1_2M", "ITU 1 / Amateur 2m"},
    {28, "ITU2_2M", "ITU 2 / Amateur 2m"},
    {29, "EU_866", "EU 866 MHz"},
    {30, "EU_874", "EU 874 MHz"},
    {31, "EU_917", "EU 917 MHz"},
    {32, "EU_N_868", "EU 868 MHz (narrow)"},
    {33, "ITU3_2M", "ITU 3 / Amateur 2m"},
    {34, "ITU1_70CM", "ITU 1 / Amateur 70cm"},
    {35, "ITU2_70CM", "ITU 2 / Amateur 70cm"},
    {36, "ITU3_70CM", "ITU 3 / Amateur 70cm"},
    {37, "ITU2_125CM", "ITU 2 / Amateur 1.25m"},
};

// Config.LoRaConfig.ModemPreset
constexpr EnumOption kModemPreset[] = {
    {0, "LONG_FAST", "Long range, fast"},
    {1, "LONG_SLOW", "Long range, slow (deprecated)"},
    {2, "VERY_LONG_SLOW", "Very long, slow (deprecated)"},
    {3, "MEDIUM_SLOW", "Medium range, slow"},
    {4, "MEDIUM_FAST", "Medium range, fast"},
    {5, "SHORT_SLOW", "Short range, slow"},
    {6, "SHORT_FAST", "Short range, fast"},
    {7, "LONG_MODERATE", "Long range, moderate"},
    {8, "SHORT_TURBO", "Short range, turbo"},
    {9, "LONG_TURBO", "Long range, turbo"},
    {10, "LITE_FAST", "Lite, fast"},
    {11, "LITE_SLOW", "Lite, slow"},
    {12, "NARROW_FAST", "Narrow, fast"},
    {13, "NARROW_SLOW", "Narrow, slow"},
    {14, "TINY_FAST", "Tiny, fast"},
    {15, "TINY_SLOW", "Tiny, slow"},
    {16, "MEDIUM_TURBO", "Medium range, turbo"},
};

// Config.LoRaConfig.FEM_LNA_Mode
constexpr EnumOption kFemLna[] = {
    {0, "DISABLED", "Disabled"},
    {1, "ENABLED", "Enabled"},
    {2, "NOT_PRESENT", "Not present"},
    {3, "AUTO", "Automatic"},
};

// Channel.Role
constexpr EnumOption kChannelRole[] = {
    {0, "DISABLED", "Disabled"},
    {1, "PRIMARY", "Primary"},
    {2, "SECONDARY", "Secondary"},
};

// Config.DeviceConfig.Role
constexpr EnumOption kDeviceRole[] = {
    {0, "CLIENT", "Client"},
    {1, "CLIENT_MUTE", "Client, no rebroadcast"},
    {2, "ROUTER", "Router (towers only)"},
    {3, "ROUTER_CLIENT", "Router Client (deprecated)"},
    {4, "REPEATER", "Repeater (deprecated)"},
    {5, "TRACKER", "Tracker"},
    {6, "SENSOR", "Sensor"},
    {7, "TAK", "TAK"},
    {8, "CLIENT_HIDDEN", "Client Hidden"},
    {9, "LOST_AND_FOUND", "Lost and Found"},
    {10, "TAK_TRACKER", "TAK Tracker"},
    {11, "ROUTER_LATE", "Router Late"},
    {12, "CLIENT_BASE", "Client Base"},
};

// Config.DeviceConfig.RebroadcastMode
constexpr EnumOption kRebroadcast[] = {
    {0, "ALL", "All"},
    {1, "ALL_SKIP_DECODING", "All, skip decoding"},
    {2, "LOCAL_ONLY", "Local channels only"},
    {3, "KNOWN_ONLY", "Known nodes only"},
    {4, "NONE", "None"},
    {5, "CORE_PORTNUMS_ONLY", "Core portnums only"},
};

// Config.DeviceConfig.BuzzerMode
constexpr EnumOption kBuzzerMode[] = {
    {0, "ALL_ENABLED", "All sounds"},
    {1, "DISABLED", "Silent"},
    {2, "NOTIFICATIONS_ONLY", "Notifications only"},
    {3, "SYSTEM_ONLY", "System sounds only"},
    {4, "DIRECT_MSG_ONLY", "Direct messages only"},
};

// Config.PositionConfig.GpsMode
constexpr EnumOption kGpsMode[] = {
    {0, "DISABLED", "Disabled"},
    {1, "ENABLED", "Enabled"},
    {2, "NOT_PRESENT", "Not present"},
};

// Config.NetworkConfig.AddressMode
constexpr EnumOption kAddressMode[] = {
    {0, "DHCP", "DHCP"},
    {1, "STATIC", "Static"},
};

// Config.NetworkConfig.ProtocolFlags (enabled_protocols is a uint32 bit mask
// with a single bit defined today)
constexpr EnumOption kProtocolFlags[] = {
    {0, "NO_BROADCAST", "None"},
    {1, "UDP_BROADCAST", "UDP broadcast"},
};

// Config.DisplayConfig.DisplayUnits
constexpr EnumOption kDisplayUnits[] = {
    {0, "METRIC", "Metric"},
    {1, "IMPERIAL", "Imperial"},
};

// Config.DisplayConfig.OledType
constexpr EnumOption kOledType[] = {
    {0, "OLED_AUTO", "Detect automatically"},
    {1, "OLED_SSD1306", "SSD1306"},
    {2, "OLED_SH1106", "SH1106"},
    {3, "OLED_SH1107", "SH1107"},
    {4, "OLED_SH1107_128_128", "SH1107 128x128"},
    {5, "OLED_SH1107_ROTATED", "SH1107 rotated"},
};

// Config.DisplayConfig.DisplayMode
constexpr EnumOption kDisplayMode[] = {
    {0, "DEFAULT", "Default 128x64 layout"},
    {1, "TWOCOLOR", "Two-colour display"},
    {2, "INVERTED", "Inverted top bar"},
    {3, "COLOR", "Full-colour TFT"},
};

// Config.DisplayConfig.CompassOrientation
constexpr EnumOption kCompass[] = {
    {0, "DEGREES_0", "0 degrees"},
    {1, "DEGREES_90", "90 degrees"},
    {2, "DEGREES_180", "180 degrees"},
    {3, "DEGREES_270", "270 degrees"},
    {4, "DEGREES_0_INVERTED", "0 degrees, inverted"},
    {5, "DEGREES_90_INVERTED", "90 degrees, inverted"},
    {6, "DEGREES_180_INVERTED", "180 degrees, inverted"},
    {7, "DEGREES_270_INVERTED", "270 degrees, inverted"},
};

// Config.BluetoothConfig.PairingMode
constexpr EnumOption kPairingMode[] = {
    {0, "RANDOM_PIN", "Random PIN"},
    {1, "FIXED_PIN", "Fixed PIN"},
    {2, "NO_PIN", "No PIN (just works)"},
};

// Config.SecurityConfig.PacketSignaturePolicy
constexpr EnumOption kSignaturePolicy[] = {
    {0, "PACKET_SIGNATURE_POLICY_COMPATIBLE", "Compatible: no signing"},
    {1, "PACKET_SIGNATURE_POLICY_BALANCED", "Balanced: sign, accept unsigned"},
    {2, "PACKET_SIGNATURE_POLICY_STRICT", "Strict: require signatures"},
};

// ModuleConfig.CannedMessageConfig.InputEventChar (numbers are key codes)
constexpr EnumOption kInputEvent[] = {
    {0, "NONE", "None"},
    {17, "UP", "Up"},
    {18, "DOWN", "Down"},
    {19, "LEFT", "Left"},
    {20, "RIGHT", "Right"},
    {10, "SELECT", "Select"},
    {27, "BACK", "Back"},
    {24, "CANCEL", "Cancel"},
};

// ModuleConfig.SerialConfig.Serial_Baud
constexpr EnumOption kSerialBaud[] = {
    {0, "BAUD_DEFAULT", "Default"},
    {1, "BAUD_110", "110"},
    {2, "BAUD_300", "300"},
    {3, "BAUD_600", "600"},
    {4, "BAUD_1200", "1200"},
    {5, "BAUD_2400", "2400"},
    {6, "BAUD_4800", "4800"},
    {7, "BAUD_9600", "9600"},
    {8, "BAUD_19200", "19200"},
    {9, "BAUD_38400", "38400"},
    {10, "BAUD_57600", "57600"},
    {11, "BAUD_115200", "115200"},
    {12, "BAUD_230400", "230400"},
    {13, "BAUD_460800", "460800"},
    {14, "BAUD_576000", "576000"},
    {15, "BAUD_921600", "921600"},
};

// ModuleConfig.SerialConfig.Serial_Mode
constexpr EnumOption kSerialMode[] = {
    {0, "DEFAULT", "Default"},
    {1, "SIMPLE", "Simple"},
    {2, "PROTO", "Protobufs"},
    {3, "TEXTMSG", "Text message"},
    {4, "NMEA", "NMEA positions"},
    {5, "CALTOPO", "CalTopo"},
    {6, "WS85", "WS85 weather station"},
    {7, "VE_DIRECT", "VE.Direct"},
    {8, "MS_CONFIG", "MS config"},
    {9, "LOG", "Log (all packets)"},
    {10, "LOGTEXT", "Log (text only)"},
    {11, "MODBUS", "Modbus"},
};

// ModuleConfig.DetectionSensorConfig.TriggerType
constexpr EnumOption kTriggerType[] = {
    {0, "LOGIC_LOW", "Low"},
    {1, "LOGIC_HIGH", "High"},
    {2, "FALLING_EDGE", "Falling edge"},
    {3, "RISING_EDGE", "Rising edge"},
    {4, "EITHER_EDGE_ACTIVE_LOW", "Either edge, active low"},
    {5, "EITHER_EDGE_ACTIVE_HIGH", "Either edge, active high"},
};

// ── Sections ────────────────────────────────────────────────────────────────

// mesh.proto User, sent with AdminMessage.set_owner.
constexpr FieldDef kUser[] = {
    Tx("Long name", "long_name", 2, 39, "Meshtastic 1999", "Your full node name."),
    Tx("Short name", "short_name", 3, 4, "1999", "Up to 4 characters, shown on small screens."),
    Bo("Licensed ham", "is_licensed", 6, false, "Licensed operator: turns encryption off."),
    Bo("Unmessagable", "is_unmessagable", 9, false, "Node is not monitored for messages."),
};

constexpr FieldDef kLora[] = {
    En("Region", "region", 7, "RegionCode", kRegion, 0, "Where the radio is used; sets the band."),
    Bo("Use preset", "use_preset", 1, true, "Off: use bandwidth / SF / coding rate below."),
    En("Modem preset", "modem_preset", 2, "ModemPreset", kModemPreset, 0),
    In("Hop limit", "hop_limit", 8, "uint32", 0, 7, "", 3, "How many times a message is repeated."),
    In("TX power", "tx_power", 10, "int32", 0, 36, "dBm", 0, "0 = the most the region allows."),
    Bo("TX enabled", "tx_enabled", 9, true, "Off while swapping antennas or bench testing."),
    In("Frequency slot", "channel_num", 11, "uint32", 0, kU16, "", 0, "0 = picked from the primary channel name."),
    Bo("Override duty cycle", "override_duty_cycle", 12, false, "May break your region's rules."),
    Bo("RX boosted gain", "sx126x_rx_boosted_gain", 13, true, "SX126x radios only."),
    In("Bandwidth", "bandwidth", 3, "uint32", 0, kU16, "kHz", 250, "Only when 'Use preset' is off."),
    In("Spread factor", "spread_factor", 4, "uint32", 5, 12, "", 11, "Only when 'Use preset' is off."),
    // 4/5 .. 4/8; the protobuf itself gives no range (nanopb int_size 8).
    In("Coding rate 4/", "coding_rate", 5, "uint32", 5, 8, "", 5, "Only when 'Use preset' is off."),
    Bo("Ignore MQTT", "ignore_mqtt", 104, false, "Drop packets that travelled via MQTT."),
    Bo("OK to MQTT", "config_ok_to_mqtt", 105, false, "Let others uplink your packets to MQTT."),
    Bo("PA fan disabled", "pa_fan_disabled", 15, false),
    En("FEM LNA mode", "fem_lna_mode", 106, "FEM_LNA_Mode", kFemLna, 0),
};

// channel.proto Channel, sent with AdminMessage.set_channel. `index` (1) and
// `role` (3) are Channel's own; the rest sit in Channel.settings.
#define MM_CHANNEL_FIELDS(ROLE, NAME, PSK)                                                              \
    En("Role", "role", 3, "Channel.Role", kChannelRole, ROLE),                                          \
    Via(Tx("Name", "name", 3, 11, NAME, "Empty on the primary = the preset's name."),                   \
        "Channel.settings(2): ChannelSettings"),                                                        \
    Via(Tx("PSK (base64)", "psk", 2, 44, PSK, "0, 16 or 32 bytes as base64. AQ== is the default key.",  \
           true, "bytes"),                                                                              \
        "Channel.settings(2): ChannelSettings"),                                                        \
    Via(Bo("MQTT uplink", "uplink_enabled", 5, false), "Channel.settings(2): ChannelSettings"),         \
    Via(Bo("MQTT downlink", "downlink_enabled", 6, false), "Channel.settings(2): ChannelSettings"),     \
    Via(In("Position precision", "position_precision", 1, "uint32", 0, 32, "bits", 0,                   \
           "0 = never share position here, 32 = exact."),                                               \
        "Channel.settings(2).module_settings(7): ModuleSettings"),                                      \
    Via(Bo("Muted", "is_muted", 2, false), "Channel.settings(2).module_settings(7): ModuleSettings")

constexpr FieldDef kChannelPrimary[] = {MM_CHANNEL_FIELDS(1, "", "AQ==")};
constexpr FieldDef kChannelSecondary[] = {MM_CHANNEL_FIELDS(0, "", "")};
#undef MM_CHANNEL_FIELDS

constexpr FieldDef kDevice[] = {
    En("Role", "role", 1, "Role", kDeviceRole, 0),
    En("Rebroadcast", "rebroadcast_mode", 6, "RebroadcastMode", kRebroadcast, 0),
    In("Node info every", "node_info_broadcast_secs", 7, "uint32", 3600, kU32, "s", 10800,
       "Minimum 1 hour."),
    Bo("LED heartbeat off", "led_heartbeat_disabled", 12, false),
    En("Buzzer", "buzzer_mode", 13, "BuzzerMode", kBuzzerMode, 0),
    Bo("No triple click", "disable_triple_click", 10, false),
    Bo("Double tap = button", "double_tap_as_button_press", 8, false),
    Gpio("Button GPIO", "button_gpio", 4),
    Gpio("Buzzer GPIO", "buzzer_gpio", 5),
    Tx("Time zone", "tzdef", 11, 64, "", "POSIX TZ string, e.g. EST5EDT,M3.2.0,M11.1.0"),
};

constexpr FieldDef kPosition[] = {
    En("GPS mode", "gps_mode", 13, "GpsMode", kGpsMode, 1),
    In("Broadcast every", "position_broadcast_secs", 1, "uint32", 0, kU32, "s", 900,
       "Longest gap between position broadcasts."),
    Bo("Smart position", "position_broadcast_smart_enabled", 2, true, "Send sooner when you move."),
    In("Smart min distance", "broadcast_smart_minimum_distance", 10, "uint32", 0, kU32, "m", 100),
    In("Smart min interval", "broadcast_smart_minimum_interval_secs", 11, "uint32", 0, kU32, "s", 30),
    In("GPS update every", "gps_update_interval", 5, "uint32", 0, kU32, "s", 120),
    Bo("Fixed position", "fixed_position", 3, false, "Broadcast the coordinates below, not a GPS fix."),
    // The coordinates are not in PositionConfig: they travel as a Position in
    // AdminMessage.set_fixed_position (41); remove_fixed_position (42) clears.
    Via(Tx("Fixed latitude", "latitude_i", 1, 11, "0.0", "Decimal degrees; sent as degrees x 1e7.", false,
           "sfixed32"),
        "AdminMessage.set_fixed_position(41): Position"),
    Via(Tx("Fixed longitude", "longitude_i", 2, 12, "0.0", "Decimal degrees; sent as degrees x 1e7.", false,
           "sfixed32"),
        "AdminMessage.set_fixed_position(41): Position"),
    Via(In("Fixed altitude", "altitude", 3, "int32", kI32Min, kI32Max, "m", 0, "Above mean sea level."),
        "AdminMessage.set_fixed_position(41): Position"),
    // PositionFlags bit mask: ALTITUDE 1, ALTITUDE_MSL 2, GEOIDAL_SEPARATION 4,
    // DOP 8, HVDOP 16, SATINVIEW 32, SEQ_NO 64, TIMESTAMP 128, HEADING 256,
    // SPEED 512.
    In("Position flags", "position_flags", 7, "uint32", 0, 1023, "", 811,
       "Bit mask: 1 alt, 2 MSL, 8 DOP, 32 sats, 256 heading, 512 speed."),
    Gpio("GPS RX GPIO", "rx_gpio", 8),
    Gpio("GPS TX GPIO", "tx_gpio", 9),
    Gpio("GPS enable GPIO", "gps_en_gpio", 12),
};

constexpr FieldDef kPower[] = {
    Bo("Power saving", "is_power_saving", 1, false, "Sleeps as much as possible."),
    In("Shutdown after", "on_battery_shutdown_after_secs", 2, "uint32", 0, kU32, "s", 0,
       "After external power is lost. 0 = never."),
    In("Wait for Bluetooth", "wait_bluetooth_secs", 4, "uint32", 0, kU32, "s", 60),
    In("Super deep sleep", "sds_secs", 6, "uint32", 0, kU32, "s", 0, "0 = firmware default (one year)."),
    In("Light sleep", "ls_secs", 7, "uint32", 0, kU32, "s", 300, "ESP32 only."),
    In("Min wake", "min_wake_secs", 8, "uint32", 0, kU32, "s", 10),
    In("Battery INA address", "device_battery_ina_address", 9, "uint32", 0, kU8, "", 0, "I2C address."),
};

constexpr FieldDef kNetwork[] = {
    Bo("WiFi enabled", "wifi_enabled", 1, false, "Turns Bluetooth off on ESP32."),
    Tx("WiFi SSID", "wifi_ssid", 3, 32, ""),
    Tx("WiFi password", "wifi_psk", 4, 64, "", "", true),
    Tx("NTP server", "ntp_server", 5, 32, "meshtastic.pool.ntp.org"),
    Bo("Ethernet enabled", "eth_enabled", 6, false),
    En("Address mode", "address_mode", 7, "AddressMode", kAddressMode, 0),
    Tx("Rsyslog server", "rsyslog_server", 9, 32, ""),
    En("Mesh via UDP", "enabled_protocols", 10, "uint32 (ProtocolFlags)", kProtocolFlags, 0),
    Bo("IPv6 enabled", "ipv6_enabled", 11, false),
};

constexpr FieldDef kDisplay[] = {
    In("Screen on for", "screen_on_secs", 1, "uint32", 0, kU32, "s", 600, "0 = firmware default."),
    En("Units", "units", 6, "DisplayUnits", kDisplayUnits, 0),
    Bo("Flip screen", "flip_screen", 5, false),
    In("Carousel every", "auto_screen_carousel_secs", 3, "uint32", 0, kU32, "s", 0, "0 = off."),
    Bo("12 hour clock", "use_12h_clock", 12, false),
    Bo("Bold heading", "heading_bold", 9, false),
    Bo("Wake on tap/motion", "wake_on_tap_or_motion", 10, false),
    Bo("Long node names", "use_long_node_name", 13, false),
    Bo("Message bubbles", "enable_message_bubbles", 14, false),
    En("OLED type", "oled", 7, "OledType", kOledType, 0),
    En("Display mode", "displaymode", 8, "DisplayMode", kDisplayMode, 0),
    En("Compass", "compass_orientation", 11, "CompassOrientation", kCompass, 0),
};

constexpr FieldDef kBluetooth[] = {
    Bo("Enabled", "enabled", 1, true),
    En("Pairing mode", "mode", 2, "PairingMode", kPairingMode, 1),
    In("Fixed PIN", "fixed_pin", 3, "uint32", 0, 999999, "", 123456, "Used when pairing mode is FIXED_PIN."),
};

constexpr FieldDef kSecurity[] = {
    Bo("Serial console", "serial_enabled", 5, true, "Serial console over the stream API."),
    Bo("Debug log API", "debug_log_api_enabled", 6, false),
    Bo("Managed device", "is_managed", 4, false, "Only an admin key may change settings."),
    Bo("Legacy admin channel", "admin_channel_enabled", 8, false),
    En("Packet signing", "packet_signature_policy", 9, "PacketSignaturePolicy", kSignaturePolicy, 0),
};

// ── Module configs (AdminMessage.set_module_config) ─────────────────────────

constexpr FieldDef kMqtt[] = {
    Bo("Enabled", "enabled", 1, false),
    Tx("Server", "address", 2, 63, "mqtt.meshtastic.org"),
    Tx("Username", "username", 3, 63, "meshdev"),
    Tx("Password", "password", 4, 31, "large4cats", "", true),
    Bo("Encryption", "encryption_enabled", 5, true, "Send encrypted packets to MQTT."),
    Bo("TLS", "tls_enabled", 7, false),
    Tx("Root topic", "root", 8, 31, "msh"),
    Bo("Proxy via client", "proxy_to_client_enabled", 9, false, "Use the connected app's network."),
    Bo("Map reporting", "map_reporting_enabled", 10, false, "Publishes an unencrypted map report."),
    Via(In("Map report every", "publish_interval_secs", 1, "uint32", 3600, kU32, "s", 3600, "Minimum 1 hour."),
        "MQTTConfig.map_report_settings(11): MapReportSettings"),
    Via(In("Map precision", "position_precision", 2, "uint32", 0, 32, "bits", 14),
        "MQTTConfig.map_report_settings(11): MapReportSettings"),
    Via(Bo("Share location", "should_report_location", 3, false, "Consent to publish your location."),
        "MQTTConfig.map_report_settings(11): MapReportSettings"),
};

constexpr FieldDef kTelemetry[] = {
    Bo("Send device metrics", "device_telemetry_enabled", 14, true),
    In("Device every", "device_update_interval", 1, "uint32", 0, kU32, "s", 1800),
    Bo("Environment", "environment_measurement_enabled", 3, false),
    In("Environment every", "environment_update_interval", 2, "uint32", 0, kU32, "s", 1800),
    Bo("Env. on screen", "environment_screen_enabled", 4, false),
    Bo("Fahrenheit", "environment_display_fahrenheit", 5, false),
    Bo("Air quality", "air_quality_enabled", 6, false),
    In("Air quality every", "air_quality_interval", 7, "uint32", 0, kU32, "s", 1800),
    Bo("Air q. on screen", "air_quality_screen_enabled", 15, false),
    Bo("Power metrics", "power_measurement_enabled", 8, false),
    In("Power every", "power_update_interval", 9, "uint32", 0, kU32, "s", 1800),
    Bo("Power on screen", "power_screen_enabled", 10, false),
    Bo("Health metrics", "health_measurement_enabled", 11, false),
    In("Health every", "health_update_interval", 12, "uint32", 0, kU32, "s", 1800),
    Bo("Health on screen", "health_screen_enabled", 13, false),
};

constexpr FieldDef kCannedMessage[] = {
    // The message list is not in CannedMessageConfig; it has its own
    // AdminMessage field (36), a '|' separated string of up to 200 bytes.
    Via(Tx("Messages", "set_canned_message_module_messages", 36, 200, "Hi|Bye|Yes|No|Ok",
           "Separate messages with |", false, "string"),
        "AdminMessage"),
    Bo("Send bell", "send_bell", 11, false),
    Bo("Rotary encoder", "rotary1_enabled", 1, false),
    Bo("Up/down/select", "updown1_enabled", 8, false),
    Gpio("Encoder pin A", "inputbroker_pin_a", 2),
    Gpio("Encoder pin B", "inputbroker_pin_b", 3),
    Gpio("Encoder press pin", "inputbroker_pin_press", 4),
    En("Clockwise event", "inputbroker_event_cw", 5, "InputEventChar", kInputEvent, 0),
    En("Counter-cw event", "inputbroker_event_ccw", 6, "InputEventChar", kInputEvent, 0),
    En("Press event", "inputbroker_event_press", 7, "InputEventChar", kInputEvent, 0),
};

constexpr FieldDef kRangeTest[] = {
    Bo("Enabled", "enabled", 1, false),
    In("Send every", "sender", 2, "uint32", 0, kU32, "s", 0, "0 = receive only."),
    Bo("Save CSV", "save", 3, false, "ESP32 with a web server only."),
    Bo("Clear on reboot", "clear_on_reboot", 4, false),
};

constexpr FieldDef kStoreForward[] = {
    Bo("Enabled", "enabled", 1, false),
    Bo("Server", "is_server", 6, false, "Needs an ESP32 with PSRAM."),
    Bo("Heartbeat", "heartbeat", 2, false),
    In("Records", "records", 3, "uint32", 0, kU32, "", 0, "0 = 3/4 of free PSRAM."),
    In("History max", "history_return_max", 4, "uint32", 0, kU32, "", 0, "0 = 25 messages."),
    In("History window", "history_return_window", 5, "uint32", 0, kU32, "min", 0, "0 = 240 minutes."),
};

constexpr FieldDef kNeighborInfo[] = {
    Bo("Enabled", "enabled", 1, false),
    In("Update every", "update_interval", 2, "uint32", 14400, kU32, "s", 21600, "Minimum 4 hours."),
    Bo("Send over LoRa", "transmit_over_lora", 3, false, "Not on the default channel."),
};

constexpr FieldDef kExtNotification[] = {
    Bo("Enabled", "enabled", 1, false),
    Bo("Alert on message", "alert_message", 5, false),
    Bo("Message: vibra", "alert_message_vibra", 10, false),
    Bo("Message: buzzer", "alert_message_buzzer", 11, false),
    Bo("Alert on bell", "alert_bell", 6, false),
    Bo("Bell: vibra", "alert_bell_vibra", 12, false),
    Bo("Bell: buzzer", "alert_bell_buzzer", 13, false),
    In("Output for", "output_ms", 2, "uint32", 0, kU32, "ms", 1000),
    In("Nag timeout", "nag_timeout", 14, "uint32", 0, kU16, "s", 0),
    Bo("Active high", "active", 4, false),
    Bo("PWM buzzer", "use_pwm", 7, false),
    Bo("I2S as buzzer", "use_i2s_as_buzzer", 15, false),
    Gpio("Output GPIO", "output", 3),
    Gpio("Vibra GPIO", "output_vibra", 8),
    Gpio("Buzzer GPIO", "output_buzzer", 9),
};

constexpr FieldDef kSerial[] = {
    Bo("Enabled", "enabled", 1, false),
    En("Mode", "mode", 7, "Serial_Mode", kSerialMode, 0),
    En("Baud", "baud", 5, "Serial_Baud", kSerialBaud, 0),
    Bo("Echo", "echo", 2, false),
    Gpio("RX GPIO", "rxd", 3),
    Gpio("TX GPIO", "txd", 4),
    In("Timeout", "timeout", 6, "uint32", 0, kU32, "ms", 0, "0 = 250 ms."),
    Bo("Use console port", "override_console_serial_port", 8, false),
};

constexpr FieldDef kDetectionSensor[] = {
    Bo("Enabled", "enabled", 1, false),
    Tx("Name", "name", 5, 19, "", "Sensor name used in the alert text."),
    Gpio("Monitor GPIO", "monitor_pin", 6),
    En("Trigger", "detection_trigger_type", 7, "TriggerType", kTriggerType, 0),
    Bo("Internal pull-up", "use_pullup", 8, false),
    In("Min broadcast gap", "minimum_broadcast_secs", 2, "uint32", 0, kU32, "s", 45),
    In("State every", "state_broadcast_secs", 3, "uint32", 0, kU32, "s", 0, "0 = only on detection."),
    Bo("Send bell", "send_bell", 4, false),
};

constexpr FieldDef kAmbientLighting[] = {
    Bo("LED on", "led_state", 1, false),
    In("Current", "current", 2, "uint32", 0, 31, "", 10),
    In("Red", "red", 3, "uint32", 0, 255, "", 0),
    In("Green", "green", 4, "uint32", 0, 255, "", 0),
    In("Blue", "blue", 5, "uint32", 0, 255, "", 0),
};

constexpr FieldDef kPaxcounter[] = {
    Bo("Enabled", "enabled", 1, false, "WiFi and Bluetooth must both be off."),
    In("Update every", "paxcounter_update_interval", 2, "uint32", 0, kU32, "s", 900),
    In("WiFi threshold", "wifi_threshold", 3, "int32", kI32Min, kI32Max, "dBm", -80),
    In("BLE threshold", "ble_threshold", 4, "int32", kI32Min, kI32Max, "dBm", -80),
};

// ── Actions (each is one AdminMessage field) ────────────────────────────────

constexpr FieldDef kActions[] = {
    Ac("Reboot", "reboot_seconds", 97, "int32", 5, "Reboot the radio in 5 seconds?"),
    Ac("Shut down", "shutdown_seconds", 98, "int32", 5, "Shut the radio down in 5 seconds?"),
    Ac("Reset node DB", "nodedb_reset", 100, "bool", 0, "Forget every node the radio has heard, favorites too?"),
    Ac("Reset DB, keep favs", "nodedb_reset", 100, "bool", 1,
       "Forget every node the radio has heard, except favorites?"),
    Ac("Factory reset config", "factory_reset_config", 99, "int32", 1,
       "Reset ALL radio settings to factory defaults? Bluetooth bonds are kept."),
    Ac("Factory reset device", "factory_reset_device", 94, "int32", 1,
       "Erase EVERYTHING on the radio: settings, keys, nodes and Bluetooth bonds?"),
};

struct SectionTable {
    SectionDef def;
    const FieldDef* fields;
    int count;
};

template <size_t N>
constexpr SectionTable Cfg(const char* id, const char* name, const char* title, const char* msg,
                           const char* oneof, int oneof_num, const FieldDef (&f)[N]) {
    return {{id, name, title, SectionKind::Config, msg, "set_config", 34, oneof, oneof_num, -1}, f,
            static_cast<int>(N)};
}

template <size_t N>
constexpr SectionTable Mod(const char* id, const char* name, const char* title, const char* msg,
                           const char* oneof, int oneof_num, const FieldDef (&f)[N]) {
    return {{id, name, title, SectionKind::Module, msg, "set_module_config", 35, oneof, oneof_num, -1}, f,
            static_cast<int>(N)};
}

template <size_t N>
constexpr SectionTable Chan(const char* id, const char* name, const char* title, int index,
                            const FieldDef (&f)[N]) {
    return {{id, name, title, SectionKind::Channel, "Channel", "set_channel", 33, "", 0, index}, f,
            static_cast<int>(N)};
}

template <size_t N>
constexpr int countof(const FieldDef (&)[N]) {
    return static_cast<int>(N);
}

// Order = the order of the section list on screen.
constexpr SectionTable kSections[] = {
    {{"user", "User", "User", SectionKind::Owner, "User", "set_owner", 32, "", 0, -1}, kUser, countof(kUser)},
    Cfg("lora", "LoRa", "LoRa", "Config.LoRaConfig", "lora", 6, kLora),
    Chan("channel0", "Channel 0", "Channel 0 (primary)", 0, kChannelPrimary),
    Chan("channel1", "Channel 1", "Channel 1 (secondary)", 1, kChannelSecondary),
    Cfg("device", "Device", "Device", "Config.DeviceConfig", "device", 1, kDevice),
    Cfg("position", "Position", "Position", "Config.PositionConfig", "position", 2, kPosition),
    Cfg("power", "Power", "Power", "Config.PowerConfig", "power", 3, kPower),
    Cfg("network", "Network", "Network", "Config.NetworkConfig", "network", 4, kNetwork),
    Cfg("display", "Display", "Display", "Config.DisplayConfig", "display", 5, kDisplay),
    Cfg("bluetooth", "Bluetooth", "Bluetooth", "Config.BluetoothConfig", "bluetooth", 7, kBluetooth),
    Cfg("security", "Security", "Security", "Config.SecurityConfig", "security", 8, kSecurity),
    Mod("mqtt", "MQTT", "MQTT", "ModuleConfig.MQTTConfig", "mqtt", 1, kMqtt),
    Mod("telemetry", "Telemetry", "Telemetry", "ModuleConfig.TelemetryConfig", "telemetry", 6, kTelemetry),
    Mod("canned_message", "Canned Msg", "Canned Message", "ModuleConfig.CannedMessageConfig",
        "canned_message", 7, kCannedMessage),
    Mod("range_test", "Range Test", "Range Test", "ModuleConfig.RangeTestConfig", "range_test", 5, kRangeTest),
    Mod("store_forward", "Store & Fwd", "Store & Forward", "ModuleConfig.StoreForwardConfig",
        "store_forward", 4, kStoreForward),
    Mod("neighbor_info", "Neighbors", "Neighbor Info", "ModuleConfig.NeighborInfoConfig", "neighbor_info", 10,
        kNeighborInfo),
    Mod("external_notification", "Ext. Notify", "External Notification",
        "ModuleConfig.ExternalNotificationConfig", "external_notification", 3, kExtNotification),
    Mod("serial", "Serial", "Serial", "ModuleConfig.SerialConfig", "serial", 2, kSerial),
    Mod("detection_sensor", "Detection", "Detection Sensor", "ModuleConfig.DetectionSensorConfig",
        "detection_sensor", 12, kDetectionSensor),
    Mod("ambient_lighting", "Ambient LED", "Ambient Lighting", "ModuleConfig.AmbientLightingConfig",
        "ambient_lighting", 11, kAmbientLighting),
    Mod("paxcounter", "Paxcounter", "Paxcounter", "ModuleConfig.PaxcounterConfig", "paxcounter", 13,
        kPaxcounter),
    {{"actions", "Actions", "Actions", SectionKind::Actions, "AdminMessage", "", 0, "", 0, -1}, kActions,
     countof(kActions)},
};

} // namespace

// ── RadioConfig ─────────────────────────────────────────────────────────────

RadioConfig::RadioConfig() {
    for (const SectionTable& t : kSections) {
        Section s;
        s.def = t.def;
        for (int i = 0; i < t.count; ++i) {
            Field f;
            f.def = &t.fields[i];
            f.ival = f.saved_ival = f.def->def_int;
            f.sval = f.saved_sval = f.def->def_text;
            s.fields.push_back(std::move(f));
        }
        sections_.push_back(std::move(s));
    }
}

int RadioConfig::find_section(const char* id) const {
    for (int i = 0; i < section_count(); ++i) {
        if (std::string(sections_[static_cast<size_t>(i)].def.id) == id) return i;
    }
    return -1;
}

int RadioConfig::find_field(int section, const char* pb_field) const {
    if (section < 0 || section >= section_count()) return -1;
    const Section& s = sections_[static_cast<size_t>(section)];
    for (int i = 0; i < static_cast<int>(s.fields.size()); ++i) {
        if (std::string(s.fields[static_cast<size_t>(i)].def->pb_field) == pb_field) return i;
    }
    return -1;
}

void RadioConfig::refresh_dirty(Section& s) {
    s.dirty = std::any_of(s.fields.begin(), s.fields.end(), [](const Field& f) { return f.changed(); });
    ++revision_;
}

bool RadioConfig::set_int(int section, int field, int64_t v) {
    if (section < 0 || section >= section_count()) return false;
    Section& s = sections_[static_cast<size_t>(section)];
    if (field < 0 || field >= static_cast<int>(s.fields.size())) return false;
    Field& f = s.fields[static_cast<size_t>(field)];
    const FieldDef& d = *f.def;
    switch (d.type) {
        case FieldType::Bool:
            v = v ? 1 : 0;
            break;
        case FieldType::Int:
            v = std::clamp(v, d.min, d.max);
            break;
        case FieldType::Enum: {
            bool known = false;
            for (int i = 0; i < d.option_count; ++i) known = known || d.options[i].value == v;
            if (!known) return false;
            break;
        }
        default:
            return false;
    }
    f.ival = v;
    refresh_dirty(s);
    return true;
}

bool RadioConfig::set_text(int section, int field, const std::string& v) {
    if (section < 0 || section >= section_count()) return false;
    Section& s = sections_[static_cast<size_t>(section)];
    if (field < 0 || field >= static_cast<int>(s.fields.size())) return false;
    Field& f = s.fields[static_cast<size_t>(field)];
    if (f.def->type != FieldType::Text) return false;
    f.sval = v.substr(0, static_cast<size_t>(f.def->max_len));
    refresh_dirty(s);
    return true;
}

std::string RadioConfig::display(int section, int field, bool reveal) const {
    if (section < 0 || section >= section_count()) return {};
    const Section& s = sections_[static_cast<size_t>(section)];
    if (field < 0 || field >= static_cast<int>(s.fields.size())) return {};
    const Field& f = s.fields[static_cast<size_t>(field)];
    const FieldDef& d = *f.def;
    switch (d.type) {
        case FieldType::Bool:
            return f.ival ? "on" : "off";
        case FieldType::Enum:
            for (int i = 0; i < d.option_count; ++i) {
                if (d.options[i].value == f.ival) return d.options[i].name;
            }
            return "?";
        case FieldType::Int: {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%lld%s%s", static_cast<long long>(f.ival), d.unit[0] ? " " : "",
                          d.unit);
            return buf;
        }
        case FieldType::Text:
            if (d.secret && !reveal && !f.sval.empty()) return "****";
            return f.sval;
        default:
            return {};
    }
}

bool RadioConfig::apply(int section) {
    if (section < 0 || section >= section_count()) return false;
    Section& s = sections_[static_cast<size_t>(section)];
    if (s.def.kind == SectionKind::Actions) return false;

    // TODO(daemon): send the section to the radio here. Nothing is sent today.
    // The daemon (mmd) owns the serial link, so this needs a new IPC request
    // (see IpcProtocol.h) that makes it emit AdminMessages (PortNum ADMIN_APP)
    // to the local node, wrapped in begin_edit_settings (64) /
    // commit_edit_settings (65):
    //   Owner   -> AdminMessage.set_owner (32) = User
    //   Config  -> AdminMessage.set_config (34) = Config{ <def.oneof_field> }
    //   Channel -> AdminMessage.set_channel (33) = Channel{ index, settings, role }
    //   Module  -> AdminMessage.set_module_config (35) = ModuleConfig{ <def.oneof_field> }
    // Fields with FieldDef::pb_carrier go in the message named there instead
    // (set_fixed_position 41, set_canned_message_module_messages 36, nested
    // ChannelSettings / ModuleSettings / MapReportSettings). Only mark the
    // section saved once the radio has acknowledged; until this exists the
    // values below are also never read back from the device.
    int changed = 0;
    for (Field& f : s.fields) {
        if (!f.changed()) continue;
        ++changed;
        const FieldDef& d = *f.def;
        LOG_INFO("radio_config: {} {}.{} (#{}) = {}", s.def.id, d.pb_carrier ? d.pb_carrier : s.def.pb_message,
                 d.pb_field, d.pb_number,
                 d.type == FieldType::Text ? (d.secret ? std::string("<secret>") : f.sval)
                                           : std::to_string(f.ival));
        f.saved_ival = f.ival;
        f.saved_sval = f.sval;
    }
    LOG_INFO("radio_config: apply '{}' via AdminMessage.{} (#{}): {} field(s) saved in memory only, "
             "NOT sent to the radio",
             s.def.id, s.def.admin_field, s.def.admin_number, changed);
    refresh_dirty(s);
    return true;
}

void RadioConfig::revert(int section) {
    if (section < 0 || section >= section_count()) return;
    Section& s = sections_[static_cast<size_t>(section)];
    // TODO(daemon): once apply() talks to the radio, re-read the section here
    // (AdminMessage.get_config_request 5 / get_module_config_request 7 /
    // get_channel_request / get_owner_request) instead of restoring the last
    // locally saved values.
    for (Field& f : s.fields) {
        f.ival = f.saved_ival;
        f.sval = f.saved_sval;
    }
    LOG_INFO("radio_config: revert '{}' to the last saved values (local only)", s.def.id);
    refresh_dirty(s);
}

bool RadioConfig::run_action(int section, int field) {
    if (section < 0 || section >= section_count()) return false;
    const Section& s = sections_[static_cast<size_t>(section)];
    if (field < 0 || field >= static_cast<int>(s.fields.size())) return false;
    const FieldDef& d = *s.fields[static_cast<size_t>(field)].def;
    if (d.type != FieldType::Action) return false;
    // TODO(daemon): send AdminMessage{ <d.pb_field> (#d.pb_number) = d.def_int }
    // to the local node through the daemon. Nothing is sent today.
    LOG_INFO("radio_config: action '{}' = AdminMessage.{} (#{}) = {}: logged only, NOT sent to the radio",
             d.label, d.pb_field, d.pb_number, d.def_int);
    ++revision_;
    return true;
}

bool RadioConfig::any_dirty() const {
    return std::any_of(sections_.begin(), sections_.end(), [](const Section& s) { return s.dirty; });
}

RadioConfig& radio_config() {
    static RadioConfig instance;
    return instance;
}

} // namespace mm
