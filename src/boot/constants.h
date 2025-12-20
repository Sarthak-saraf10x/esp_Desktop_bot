#ifndef BOOT_CONSTANTS_H
#define BOOT_CONSTANTS_H

#include "csr.h"

#define SR_CMD_STR_LEN_MAX     64
#define SR_CMD_PHONEME_LEN_MAX 64

// Voice command IDs
enum CommandId {
    CMD_TIME = 0,
    CMD_WEATHER,
    CMD_RECORD_AUDIO,
    CMD_MAX
};

// Define voice commands (phonetic representations)
static const SR::csr_cmd_t voice_commands[] = {
	{CMD_TIME, "time",    "TiM"},
	{CMD_WEATHER, "weather", "Wfjk"},
	{CMD_RECORD_AUDIO, "record audio", "RfKkD nDmb"}
};

static const char* NOTIFICATION_WAKEWORD = "wakeword";
static const char* NOTIFICATION_DISPLAY = "display";
static const char* NOTIFICATION_SPEAKER = "speaker";
static const char* NOTIFICATION_COMMAND = "command";

// Display Events
static const char* EVENT_DISPLAY_WAKEWORD = "display_wakeword";
static const char* EVENT_DISPLAY_COMMAND = "display_command";
static const char* EVENT_DISPLAY_LISTENING = "display_listening";

// SR Events
static const char* EVENT_SR_WAKEWORD = "sr_wakeword";
static const char* EVENT_SR_COMMAND = "sr_command";
static const char* EVENT_SR_TIMEOUT = "sr_timeout";

#endif