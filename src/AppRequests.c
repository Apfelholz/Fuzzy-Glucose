#include "AppRequests.h"
#include <time.h>

// Settings keys (must match TextWatch.c and package.json)
#define TEXT_ALIGN_KEY 0
#define INVERT_KEY 1
#define LANGUAGE_KEY 2

// Callback for receiving glucose data from phone
static GlucoseDataCallback s_glucose_callback = NULL;

// Callback for receiving settings from phone
static SettingsCallback s_settings_callback = NULL;

// Stored glucose values (updated when received)
static int s_glucose_value = 0;      // Default: no data
static int s_trend_value = -1;       // Default: unknown trend (-1)
static time_t s_last_glucose_timestamp = 0;  // Unix time of last valid data
static time_t s_last_request_timestamp = 0;  // Unix time of last request sent
static const time_t GLUCOSE_STALE_SECONDS = 15 * 60;  // Consider data stale after 15 minutes
static const time_t GLUCOSE_REQUEST_THROTTLE_SECONDS = 300;  // 5 minutes between requests
static bool s_initialized = false;

// Forward declaration for AppSync callback compatibility
static AppMessageInboxReceived s_original_inbox_handler = NULL;

// Determine if the currently stored glucose data is stale
static bool glucose_data_stale(void) {
  if (s_last_glucose_timestamp == 0) {
    return true;
  }

  time_t now = time(NULL);
  if (now == (time_t)-1) {
    // If time retrieval failed, err on the side of keeping data to avoid flicker
    return false;
  }

  return (now - s_last_glucose_timestamp) > GLUCOSE_STALE_SECONDS;
}

// Process glucose data from received message
static void process_glucose_message(DictionaryIterator *iterator) {
  bool data_updated = false;
  
  // Check for glucose value
  Tuple *glucose_tuple = dict_find(iterator, KEY_GLUCOSE_VALUE);
  if (glucose_tuple) {
    s_glucose_value = (int)glucose_tuple->value->int32;
    APP_LOG(APP_LOG_LEVEL_INFO, "Glucose received: %d mg/dL", s_glucose_value);
    data_updated = true;
  }
  
  // Check for trend value (0-6 for different arrow directions)
  Tuple *trend_tuple = dict_find(iterator, KEY_TREND_VALUE);
  if (trend_tuple) {
    s_trend_value = (int)trend_tuple->value->int32;
    APP_LOG(APP_LOG_LEVEL_INFO, "Trend received: %d", s_trend_value);
    data_updated = true;
  }

  // Track when this data was recorded (from phone if available, otherwise now)
  Tuple *timestamp_tuple = dict_find(iterator, KEY_TIMESTAMP);
  if (timestamp_tuple) {
    s_last_glucose_timestamp = (time_t)timestamp_tuple->value->int32;
  } else if (data_updated) {
    s_last_glucose_timestamp = time(NULL);
  }
  
  // Notify via callback if data was updated and callback is registered
  if (data_updated && s_glucose_callback) {
    s_glucose_callback(s_glucose_value, s_trend_value);
  }
}

// Process settings from received message and forward to main app
static void process_settings_message(DictionaryIterator *iterator) {
  if (!s_settings_callback) {
    return;
  }

  // Check for TEXT_ALIGN_KEY
  Tuple *align_tuple = dict_find(iterator, TEXT_ALIGN_KEY);
  if (align_tuple) {
    int value = (int)align_tuple->value->int32;
    APP_LOG(APP_LOG_LEVEL_INFO, "Settings: TEXT_ALIGN=%d", value);
    s_settings_callback(TEXT_ALIGN_KEY, value);
  }

  // Check for INVERT_KEY
  Tuple *invert_tuple = dict_find(iterator, INVERT_KEY);
  if (invert_tuple) {
    int value = (int)invert_tuple->value->int32;
    APP_LOG(APP_LOG_LEVEL_INFO, "Settings: INVERT=%d", value);
    s_settings_callback(INVERT_KEY, value);
  }

  // Check for LANGUAGE_KEY
  Tuple *lang_tuple = dict_find(iterator, LANGUAGE_KEY);
  if (lang_tuple) {
    int value = (int)lang_tuple->value->int32;
    APP_LOG(APP_LOG_LEVEL_INFO, "Settings: LANGUAGE=%d", value);
    s_settings_callback(LANGUAGE_KEY, value);
  }
}

// Callback when message received - handles both glucose and config messages
static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Message received from phone");
  
  // Process glucose data from this message
  process_glucose_message(iterator);
  
  // Process settings from this message (bypasses AppSync)
  process_settings_message(iterator);
  
  // Forward to original handler (AppSync) if it exists
  // Note: This may not work reliably with AppSync, but we handle settings above
  if (s_original_inbox_handler) {
    s_original_inbox_handler(iterator, context);
  }
}

// Callback when inbox message dropped
static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  const char *reason_str;
  switch (reason) {
    case APP_MSG_OK: reason_str = "OK"; break;
    case APP_MSG_SEND_TIMEOUT: reason_str = "Send timeout"; break;
    case APP_MSG_SEND_REJECTED: reason_str = "Send rejected"; break;
    case APP_MSG_NOT_CONNECTED: reason_str = "Not connected"; break;
    case APP_MSG_APP_NOT_RUNNING: reason_str = "App not running"; break;
    case APP_MSG_INVALID_ARGS: reason_str = "Invalid args"; break;
    case APP_MSG_BUSY: reason_str = "Busy"; break;
    case APP_MSG_BUFFER_OVERFLOW: reason_str = "Buffer overflow"; break;
    case APP_MSG_ALREADY_RELEASED: reason_str = "Already released"; break;
    case APP_MSG_CALLBACK_ALREADY_REGISTERED: reason_str = "Callback registered"; break;
    case APP_MSG_CALLBACK_NOT_REGISTERED: reason_str = "Callback not registered"; break;
    case APP_MSG_OUT_OF_MEMORY: reason_str = "Out of memory"; break;
    case APP_MSG_CLOSED: reason_str = "Closed"; break;
    case APP_MSG_INTERNAL_ERROR: reason_str = "Internal error"; break;
    default: reason_str = "Unknown"; break;
  }
  APP_LOG(APP_LOG_LEVEL_WARNING, "Message dropped: %s (%d)", reason_str, (int)reason);
}

// Callback when message sent successfully
static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Message sent successfully");
}

// Callback when sending failed
static void outbox_failed_callback(DictionaryIterator *iterator, 
                                   AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Message send failed: %d", (int)reason);
}

// Register message callbacks and capture any existing inbox handler for forwarding
static void register_message_handlers(void) {
  s_original_inbox_handler = app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_sent(outbox_sent_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
}

// Get the current glucose values
void pebble_messenger_get_glucose(int *glucose_value, int *trend_value) {
  const bool stale = glucose_data_stale();

  if (glucose_value) {
    *glucose_value = stale ? 0 : s_glucose_value;
  }
  if (trend_value) {
    *trend_value = stale ? TREND_UNKNOWN : s_trend_value;
  }
}

// Check if glucose data has been received
bool pebble_messenger_has_glucose_data(void) {
  return s_glucose_value > 0 && !glucose_data_stale();
}

// Initialize message communication
// Note: Call this BEFORE app_message_open() and AppSync init
void pebble_messenger_init(GlucoseDataCallback glucose_callback, SettingsCallback settings_callback) {
  if (s_initialized) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Messenger already initialized");
    return;
  }
  
  s_glucose_callback = glucose_callback;
  s_settings_callback = settings_callback;
  s_glucose_value = 0;
  s_trend_value = -1;
  s_last_glucose_timestamp = 0;
  s_last_request_timestamp = 0;
  
  // Register callbacks - may be overridden by AppSync later; can re-register after AppSync
  register_message_handlers();
  
  s_initialized = true;
  APP_LOG(APP_LOG_LEVEL_INFO, "Pebble Messenger initialized");
}

// Allow re-registering after other components (e.g., AppSync) set their handlers
void pebble_messenger_register_handlers(void) {
  if (!s_initialized) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Messenger not initialized; cannot register handlers");
    return;
  }
  register_message_handlers();
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Messenger handlers re-registered");
}

// Open app message with appropriate buffer sizes
void pebble_messenger_open(uint32_t inbox_size, uint32_t outbox_size) {
  // Ensure minimum buffer size for glucose data
  if (inbox_size < 256) inbox_size = 256;
  if (outbox_size < 128) outbox_size = 128;
  
  app_message_open(inbox_size, outbox_size);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "App message opened: inbox=%lu, outbox=%lu", 
          (unsigned long)inbox_size, (unsigned long)outbox_size);
}

// Request glucose data from phone (sends a request message)
void pebble_messenger_request_glucose(void) {
  // Throttle requests to prevent spamming the message queue
  time_t now = time(NULL);
  if (now != (time_t)-1 && s_last_request_timestamp != 0) {
    if ((now - s_last_request_timestamp) < GLUCOSE_REQUEST_THROTTLE_SECONDS) {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Glucose request throttled (last request %ld seconds ago)", 
              (long)(now - s_last_request_timestamp));
      return;
    }
  }
  
  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);
  
  if (result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to begin message: %d", (int)result);
    return;
  }
  
  // Send request flag
  dict_write_uint8(iter, KEY_REQUEST_DATA, 1);
  dict_write_end(iter);
  
  result = app_message_outbox_send();
  if (result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to send request: %d", (int)result);
  } else {
    s_last_request_timestamp = now;
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Glucose data requested");
  }
}

// Cleanup function
void pebble_messenger_deinit(void) {
  if (!s_initialized) return;
  
  s_glucose_callback = NULL;
  s_original_inbox_handler = NULL;
  s_last_request_timestamp = 0;
  s_last_glucose_timestamp = 0;
  s_initialized = false;
  
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Pebble Messenger deinitialized");
}
