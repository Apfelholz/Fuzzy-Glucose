#pragma once

#include <pebble.h>

// Message keys for communication with Android app
#define KEY_USERNAME 0
#define KEY_PASSWORD 1
#define KEY_GLUCOSE_VALUE 2
#define KEY_TREND_VALUE 3

// Callback type for receiving glucose data
typedef void (*GlucoseDataCallback)(int glucose_value, int trend_value);

// Initialisiert die App Message Kommunikation
void pebble_messenger_init(GlucoseDataCallback callback);

// Get the current glucose values (returns last received values)
void pebble_messenger_get_glucose(int *glucose_value, int *trend_value);

// Sendet Username und Password an die Android App
void pebble_messenger_send_credentials(const char *username, const char *password);

// Cleanup-Funktion (optional, falls benötigt)
void pebble_messenger_deinit(void);