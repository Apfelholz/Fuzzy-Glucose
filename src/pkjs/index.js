var VERSION = "1.3.0";

var isReady = false;
var callbacks = [];

// Message keys - must match AppRequests.h
var KEYS = {
  // Settings keys (0-2 for AppSync)
  INVERT: 0,
  TEXT_ALIGN: 1,
  LANGUAGE: 2,
  // Glucose data keys (10+)
  GLUCOSE_VALUE: 10,
  TREND_VALUE: 11,
  REQUEST_DATA: 12,
  TIMESTAMP: 13
};

var alignments = {
  center: 0,
  left:   1,
  right:  2
};

var langs = {
  ca:    0,
  de:    1,
  en_GB: 2,
  en_US: 3,
  es:    4,
  fr:    5,
  no:    6,
  sv:    7
};

// Store latest glucose data
var glucoseData = {
  value: 0,
  trend: -1,
  timestamp: 0
};

function readyCallback(event) {
  isReady = true;
  console.log("Pebble JS ready");
  var callback;
  while (callbacks.length > 0) {
    callback = callbacks.shift();
    callback(event);
  }
}

function showConfiguration(event) {
  onReady(function() {
    var opts = getOptions();
    var url  = "http://static.sitr.us.s3-website-us-west-2.amazonaws.com/configure-fuzzy-text.html";
    Pebble.openURL(url + "#v=" + encodeURIComponent(VERSION) + "&options=" + encodeURIComponent(opts));
  });
}

function webviewclosed(event) {
  var resp = event.response;
  console.log('configuration response: '+ resp + ' ('+ typeof resp +')');

  if (!resp || resp === 'CANCELLED') {
    console.log('Configuration cancelled');
    return;
  }

  try {
    var options = JSON.parse(resp);
    if (typeof options.invert === 'undefined' &&
        typeof options.text_align === 'undefined' &&
        typeof options.lang === 'undefined') {
      return;
    }

    onReady(function() {
      setOptions(resp);
      var message = prepareConfiguration(resp);
      transmitConfiguration(message);
    });
  } catch (e) {
    console.log('Error parsing configuration: ' + e.message);
  }
}

// Handle messages from watch
function appmessage(event) {
  console.log('Received message from watch');
  var payload = event.payload;
  
  // Check if watch is requesting glucose data
  if (payload && payload[KEYS.REQUEST_DATA]) {
    console.log('Watch requested glucose data');
    sendGlucoseData();
  }
}

// Retrieves stored configuration from localStorage.
function getOptions() {
  return localStorage.getItem("options") || ("{}");
}

// Stores options in localStorage.
function setOptions(options) {
  localStorage.setItem("options", options);
}

// Takes a string containing serialized JSON as input.  This is the
// format that is sent back from the configuration web UI.  Produces
// a JSON message to send to the watch face.
function prepareConfiguration(serialized_settings) {
  var settings = JSON.parse(serialized_settings);
  var message = {};
  message[KEYS.INVERT] = settings.invert ? 1 : 0;
  message[KEYS.TEXT_ALIGN] = alignments[settings.text_align] || 0;
  message[KEYS.LANGUAGE] = langs[settings.lang] || 3;
  return message;
}

// Takes a JSON message as input.  Sends the message to the watch.
function transmitConfiguration(settings) {
  console.log('Sending configuration: '+ JSON.stringify(settings));
  Pebble.sendAppMessage(settings, function(event) {
    console.log('Configuration delivered successfully');
  }, logError);
}

// Send glucose data to watch
function sendGlucoseData() {
  if (glucoseData.value <= 0) {
    console.log('No glucose data to send');
    return;
  }
  
  var message = {};
  message[KEYS.GLUCOSE_VALUE] = glucoseData.value;
  message[KEYS.TREND_VALUE] = glucoseData.trend;
  message[KEYS.TIMESTAMP] = glucoseData.timestamp;
  
  console.log('Sending glucose data: ' + JSON.stringify(message));
  Pebble.sendAppMessage(message, function(event) {
    console.log('Glucose data delivered');
  }, logError);
}

// Update glucose data (called from companion app or external source)
function updateGlucoseData(value, trend, timestamp) {
  glucoseData.value = value || 0;
  glucoseData.trend = (typeof trend !== 'undefined') ? trend : -1;
  glucoseData.timestamp = timestamp || Math.floor(Date.now() / 1000);
  
  console.log('Glucose updated: ' + glucoseData.value + ' mg/dL, trend: ' + glucoseData.trend);
  
  // Automatically send to watch when connected
  onReady(function() {
    sendGlucoseData();
  });
}

// Expose function for external apps to push glucose data
// Usage: Pebble.sendAppMessage with glucose keys, or companion app integration
Pebble.updateGlucose = updateGlucoseData;

function logError(event) {
  console.log('Unable to deliver message with transactionId=' +
              event.data.transactionId + '; Error: ' + JSON.stringify(event.error));
}

function onReady(callback) {
  if (isReady) {
    callback();
  }
  else {
    callbacks.push(callback);
  }
}

// Register event listeners
Pebble.addEventListener("ready", readyCallback);
Pebble.addEventListener("showConfiguration", showConfiguration);
Pebble.addEventListener("webviewclosed", webviewclosed);
Pebble.addEventListener("appmessage", appmessage);

// Send initial configuration on ready
onReady(function(event) {
  var message = prepareConfiguration(getOptions());
  transmitConfiguration(message);
});

