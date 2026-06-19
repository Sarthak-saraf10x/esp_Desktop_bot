#include "app/callback_list.h"
#include "app/tasks.h"
#include "Speaker.h"

// Event callback for SR system
void sr_event_callback(void *arg, sr_event_t event, int command_id, int phrase_id) {
    switch (event) {
        case SR_EVENT_WAKEWORD:
            Serial.println(" Wake word 'Hi ESP' detected!");
            // Trigger conversation task
            if (convTaskHandle != nullptr) {
                xTaskNotifyGive(convTaskHandle);
            }
            break;

        case SR_EVENT_WAKEWORD_CHANNEL:
            Serial.printf(" Wake word detected on channel: %d\n", command_id);
            if (convTaskHandle != nullptr) {
                xTaskNotifyGive(convTaskHandle);
            }
            break;

        case SR_EVENT_COMMAND:
            Serial.printf("Command detected! ID=%d, Phrase=%d\n", command_id, phrase_id);
#ifdef ENABLE_SPEAKER_FEEDBACK
            GlobalSpeaker.beep();
#endif

            // Map phrase_id to actual voice command (since phrase_id indexes the voice_commands array)
            if (phrase_id >= 0 && phrase_id < (sizeof(voice_commands) / sizeof(sr_cmd_t))) {
                const SR::csr_cmd_t* cmd = &voice_commands[phrase_id];
                Serial.printf("    You said: '%s'\n", cmd->str);
                Serial.printf("    Phonetic: '%s'\n", cmd->phoneme);
                Serial.printf("    Command Group: %d, Phrase Index: %d\n", command_id, phrase_id);
            } else {
                Serial.println("   Unknown command mapping");
            }

            // Handle specific command groups based on command_id (from voice_commands array)
            // File: src/boot/constants.h:18
            switch (command_id) {
                case CMD_TIME:
                    Serial.printf(" Action: %s\n", voice_commands[CMD_TIME].str);
                    // Add your light ON control logic here
                    if (notification) {
                        notification->send(NOTIFICATION_DISPLAY, (void*)&voice_commands[CMD_TIME]);
                    }
                    break;
                case CMD_WEATHER:
                    Serial.printf(" Action: %s\n", voice_commands[CMD_WEATHER].str);
                    // Add your light OFF control logic here
                    if (notification) {
                        notification->send(NOTIFICATION_DISPLAY, (void*)&voice_commands[CMD_WEATHER]);
                    }
                    break;
                case CMD_RECORD_AUDIO:
                    Serial.printf(" Action: %s\n", voice_commands[CMD_RECORD_AUDIO].str);
                    // Add your light OFF control logic here
                    if (notification) {
                        notification->send(NOTIFICATION_DISPLAY, (void*)&voice_commands[CMD_RECORD_AUDIO].str);
                    }
                    break;
                default:
                    Serial.printf(" Unknown command ID: %d\n", command_id);
                    Serial.println("  Available commands:");
                    for (int i = 0; i < (sizeof(voice_commands) / sizeof(sr_cmd_t)); i++) {
                        Serial.printf("      [%d] Group %d: '%s' (%s)\n",
                                    i,
                                    voice_commands[i].command_id,
                                    voice_commands[i].str,
                                    voice_commands[i].phoneme);
                    }
                    break;
            }

            // Return to wake word mode after command
            SR::sr_set_mode(SR_MODE_WAKEWORD);
            Serial.println(" Returning to wake word detection mode");
            break;

        case SR_EVENT_TIMEOUT:
            Serial.println(" Command timeout - returning to wake word mode");
            Serial.println(" No command detected within timeout period");
            Serial.println(" Say 'Hi ESP' to activate again");
            SR::sr_set_mode(SR_MODE_WAKEWORD);
            break;

        default:
            Serial.printf(" Unknown SR event: %d\n", event);
            Serial.println(" Known events:");
            Serial.println("      SR_EVENT_WAKEWORD: Wake word detected");
            Serial.println("      SR_EVENT_WAKEWORD_CHANNEL: Multi-channel wake word");
            Serial.println("      SR_EVENT_COMMAND: Voice command detected");
            Serial.println("      SR_EVENT_TIMEOUT: Command timeout occurred");
            break;
    }
}