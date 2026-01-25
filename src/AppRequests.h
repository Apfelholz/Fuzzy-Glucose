#pragma once

#include <pebble.h>

// Message keys for communication with phone/companion app
// Keys 0-2 are reserved for AppSync (TEXT_ALIGN_KEY, INVERT_KEY, LANGUAGE_KEY)
#define KEY_GLUCOSE_VALUE 10
#define KEY_TREND_VALUE 11
#define KEY_REQUEST_DATA 12
#define KEY_TIMESTAMP 13

// Trend direction values (matching CGM conventions)
typedef enum {
  TREND_DOUBLE_UP = 0,      // Rising rapidly
  TREND_SINGLE_UP = 1,      // Rising
  TREND_FORTY_FIVE_UP = 2,  // Rising slowly
  TREND_FLAT = 3,           // Stable
  TREND_FORTY_FIVE_DOWN = 4,// Falling slowly
  TREND_SINGLE_DOWN = 5,    // Falling
  TREND_DOUBLE_DOWN = 6,    // Falling rapidly
  TREND_UNKNOWN = -1        // Unknown/no data
} GlucoseTrend;

// Callback type for receiving glucose data
typedef void (*GlucoseDataCallback)(int glucose_value, int trend_value);

// Callback type for receiving settings (key, value)
// This allows the main app to handle settings without AppSync conflicts
typedef void (*SettingsCallback)(uint32_t key, int value);

// Initialize message communication
// Note: Call this BEFORE app_message_open() and AppSync init
void pebble_messenger_init(GlucoseDataCallback glucose_callback, SettingsCallback settings_callback);

// Allow re-registering handlers after AppSync sets its callbacks
void pebble_messenger_register_handlers(void);

// Open app message with appropriate buffer sizes
void pebble_messenger_open(uint32_t inbox_size, uint32_t outbox_size);

// Get the current glucose values (returns last received values)
// Returns values via pointers; pass NULL for values you don't need
void pebble_messenger_get_glucose(int *glucose_value, int *trend_value);

// Check if glucose data has been received
bool pebble_messenger_has_glucose_data(void);

// Request glucose data from phone
void pebble_messenger_request_glucose(void);

// Cleanup function
void pebble_messenger_deinit(void);