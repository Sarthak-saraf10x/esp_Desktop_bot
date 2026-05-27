#include <WiFi.h>
#include <esp_log.h>

static const char* TAG = "WIFI";

const char* ssid = "ajay-admin2.4";
const char* password = "sush@mush";

void setupWiFi() {
    ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        ESP_LOGI(TAG, ".");
        retries++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        ESP_LOGI(TAG, "WiFi connected! IP address: %s", WiFi.localIP().toString().c_str());
    } else {
        ESP_LOGE(TAG, "WiFi connection failed!");
    }
}
