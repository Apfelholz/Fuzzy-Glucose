#include "AppRequests.h"

// Callback for receiving glucose data from Android app
static GlucoseDataCallback s_glucose_callback = NULL;

// Stored glucose values (updated when received from Android)
static int s_glucose_value = 120;  // Default value
static int s_trend_value = 0;      // Default trend (0 = up arrow)

// Callback when message received from Android app
static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Nachricht von Android empfangen!");
  
  // Check for glucose value
  Tuple *glucose_tuple = dict_find(iterator, KEY_GLUCOSE_VALUE);
  if (glucose_tuple) {
    s_glucose_value = (int)glucose_tuple->value->int32;
    APP_LOG(APP_LOG_LEVEL_INFO, "Glucose empfangen: %d", s_glucose_value);
  }
  
  // Check for trend value
  Tuple *trend_tuple = dict_find(iterator, KEY_TREND_VALUE);
  if (trend_tuple) {
    s_trend_value = (int)trend_tuple->value->int32;
    APP_LOG(APP_LOG_LEVEL_INFO, "Trend empfangen: %d", s_trend_value);
  }
  
  // Notify via callback if registered
  if (s_glucose_callback) {
    s_glucose_callback(s_glucose_value, s_trend_value);
  }
}

// Callback when inbox message dropped
static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Nachricht verworfen: %d", (int)reason);
}

// Callback wenn Nachricht erfolgreich gesendet wurde
static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Nachricht erfolgreich gesendet!");
}

// Callback wenn Senden fehlgeschlagen ist
static void outbox_failed_callback(DictionaryIterator *iterator, 
                                   AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Senden fehlgeschlagen: %d", (int)reason);
}

// Get the current glucose values
void pebble_messenger_get_glucose(int *glucose_value, int *trend_value) {
  if (glucose_value) {
    *glucose_value = s_glucose_value;
  }
  if (trend_value) {
    *trend_value = s_trend_value;
  }
}

// Initialisiert die App Message Kommunikation
void pebble_messenger_init(GlucoseDataCallback callback) {
  s_glucose_callback = callback;
  
  // Callbacks registrieren
  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_sent(outbox_sent_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
  
  // Buffer-Größe festlegen (anpassen falls mehr Daten gesendet werden)
  app_message_open(128, 128);
  
  APP_LOG(APP_LOG_LEVEL_INFO, "Pebble Messenger initialisiert");
}

// Sendet Username und Password an die Android App
void pebble_messenger_send_credentials(const char *username, const char *password) {
  if (!username || !password) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Username oder Password ist NULL");
    return;
  }
  
  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);
  
  if (result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Fehler beim Beginnen der Nachricht: %d", (int)result);
    return;
  }
  
  // Keys für die Daten (müssen mit Android App übereinstimmen)
  dict_write_cstring(iter, 0, username);  // Key 0 für username
  dict_write_cstring(iter, 1, password);  // Key 1 für password
  
  // Nachricht senden
  result = app_message_outbox_send();
  
  if (result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Fehler beim Senden: %d", (int)result);
  } else {
    APP_LOG(APP_LOG_LEVEL_INFO, "Credentials werden gesendet...");
  }
}


