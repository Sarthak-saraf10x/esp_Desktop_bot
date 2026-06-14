#include <WiFi.h>
#include <esp_log.h>

static const char* TAG = "WIFI";

const char* ssid = "Nothing10x";
const char* password = "qwert123";

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
 