var VERSION = "1.3.0";
var API = {
  BASE_URL: "",
  PRODUCT: "llu.android",
  VERSION: "4.16.0"
};
var testCredentials = (function() {
  try {
    return require('./credentials.json');
  } catch (e) {
    return {};
  }
})();

var isReady = false;
var callbacks = [];
var fetchInFlight = null;

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

// Minimal SHA-256 implementation for environments without subtle crypto
function simpleSha256(str) {
  function rightRotate(value, amount) {
    return (value >>> amount) | (value << (32 - amount));
  }
  var mathPow = Math.pow;
  var maxWord = mathPow(2, 32);
  var lengthProperty = "length";
  var i, j;
  var result = "";
  var words = [];
  var strBitLength = str[lengthProperty] * 8;
  var hash = simpleSha256.h = simpleSha256.h || [];
  var k = simpleSha256.k = simpleSha256.k || [];
  var primeCounter = k[lengthProperty];

  var isComposite = {};
  for (var candidate = 2; primeCounter < 64; candidate++) {
    if (!isComposite[candidate]) {
      for (i = 0; i < 313; i += candidate) {
        isComposite[i] = candidate;
      }
      hash[primeCounter] = (mathPow(candidate, 0.5) * maxWord) | 0;
      k[primeCounter++] = (mathPow(candidate, 1 / 3) * maxWord) | 0;
    }
  }

  str += "\u0080";
  while (str[lengthProperty] % 64 - 56) {
    str += "\u0000";
  }
  for (i = 0; i < str[lengthProperty]; i++) {
    j = str.charCodeAt(i);
    if (j >> 8) {
      return null;
    }
    words[i >> 2] |= j << ((3 - i) % 4) * 8;
  }
  words[words[lengthProperty]] = (strBitLength / maxWord) | 0;
  words[words[lengthProperty]] = strBitLength;

  for (j = 0; j < words[lengthProperty]; ) {
    var w = words.slice(j, (j += 16));
    var oldHash = hash;
    hash = hash.slice(0, 8);

    for (i = 0; i < 64; i++) {
      var w15 = w[i - 15];
      var w2 = w[i - 2];
      var a = hash[0];
      var e = hash[4];
      var temp1 =
        hash[7] +
        (rightRotate(e, 6) ^ rightRotate(e, 11) ^ rightRotate(e, 25)) +
        ((e & hash[5]) ^ (~e & hash[6])) +
        k[i] +
        (w[i] =
          i < 16
            ? w[i]
            :
              (w[i - 16] +
                (rightRotate(w15, 7) ^ rightRotate(w15, 18) ^ (w15 >>> 3)) +
                w[i - 7] +
                (rightRotate(w2, 17) ^ rightRotate(w2, 19) ^ (w2 >>> 10))) |
              0);
      var temp2 =
        (rightRotate(a, 2) ^ rightRotate(a, 13) ^ rightRotate(a, 22)) +
        ((a & hash[1]) ^ (a & hash[2]) ^ (hash[1] & hash[2]));

      hash = [(temp1 + temp2) | 0].concat(hash);
      hash[4] = (hash[4] + temp1) | 0;
    }

    for (i = 0; i < 8; i++) {
      hash[i] = (hash[i] + oldHash[i]) | 0;
    }
  }

  for (i = 0; i < 8; i++) {
    for (j = 3; j + 1; j--) {
      var b = (hash[i] >> (j * 8)) & 255;
      result += (b < 16 ? 0 : "") + b.toString(16);
    }
  }
  return result;
}

function sha256Hex(str) {
  if (typeof crypto !== "undefined" && crypto.subtle && crypto.subtle.digest) {
    var enc = new TextEncoder();
    return crypto.subtle.digest("SHA-256", enc.encode(str)).then(function(buf) {
      var out = "";
      var view = new DataView(buf);
      for (var i = 0; i < view.byteLength; i += 4) {
        out += ("00000000" + view.getUint32(i).toString(16)).slice(-8);
      }
      return out;
    });
  }
  return Promise.resolve(simpleSha256(str));
}

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
        typeof options.lang === 'undefined' &&
        typeof options.email === 'undefined' &&
        typeof options.password === 'undefined') {
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
    fetchGlucoseFromLibreLinkUp().then(function() {
      sendGlucoseData();
    });
  }
}

// Retrieves stored configuration from localStorage.
function getOptions() {
  return localStorage.getItem("options") || ("{}");
}

function parseOptions() {
  try {
    return JSON.parse(getOptions());
  } catch (e) {
    console.log('Error parsing stored options, using defaults');
    return {};
  }
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

function pickMeasurement(container) {
  if (!container) {
    return null;
  }
  var m = container.glucoseMeasurement || container.glucoseItem || container;
  if (m && m.measurementData && m.measurementData.length) {
    return m.measurementData[m.measurementData.length - 1];
  }
  return m;
}

// Fetch glucose data from LibreLinkUp API using stored credentials
function fetchGlucoseFromLibreLinkUp() {
  if (fetchInFlight) {
    return fetchInFlight;
  }

  var options = parseOptions();
  if (!options.email && testCredentials.email) {
    options.email = testCredentials.email;
  }
  if (!options.password && testCredentials.password) {
    options.password = testCredentials.password;
  }
  if (!options.email || !options.password) {
    console.log('LibreLinkUp credentials are missing');
    return Promise.resolve(null);
  }

  console.log('Starting LibreLinkUp fetch via ' + API.BASE_URL);

  var baseUrl = API.BASE_URL;
  var loginHeaders = {
    'content-type': 'application/json',
    product: API.PRODUCT,
    version: API.VERSION,
    'accept-encoding': 'gzip'
  };

  fetchInFlight = fetch(baseUrl + '/llu/auth/login', {
    method: 'POST',
    headers: loginHeaders,
    body: JSON.stringify({ email: options.email, password: options.password })
  })
  .then(function(resp) {
    if (!resp.ok) {
      throw new Error('Login failed: HTTP ' + resp.status);
    }
    return resp.json().then(function(json) {
      console.log('LibreLinkUp login status: ' + (json && json.status));
      return json;
    });
  })
  .then(function(json) {
    if (json && typeof json.status !== 'undefined' && json.status !== 0) {
      throw new Error('Login status ' + json.status);
    }
    var token = json && json.data && json.data.authTicket && json.data.authTicket.token;
    var userId = json && json.data && json.data.user && json.data.user.id;
    if (!token || !userId) {
      throw new Error('Login response missing token or user id');
    }
    return sha256Hex(userId).then(function(accountId) {
      return { token: token, accountId: accountId };
    });
  })
  .then(function(auth) {
    var headers = {
      'content-type': 'application/json',
      product: API.PRODUCT,
      version: API.VERSION,
      Authorization: 'Bearer ' + auth.token
    };
    if (auth.accountId) {
      headers['Account-Id'] = auth.accountId;
    }
    console.log('Fetching LibreLinkUp connections');
    return fetch(baseUrl + '/llu/connections', { headers: headers }).then(function(resp) {
      return { resp: resp, headers: headers };
    });
  })
  .then(function(result) {
    var resp = result.resp;
    if (!resp.ok) {
      throw new Error('Connections failed: HTTP ' + resp.status);
    }
    return resp.json().then(function(json) {
      var len = (json && json.data && json.data.length) || 0;
      console.log('Connections received: ' + len);
      return { json: json, headers: result.headers };
    });
  })
  .then(function(payload) {
    var json = payload.json;
    var headers = payload.headers;
    if (!json || !json.data || !json.data.length) {
      throw new Error('No LibreLinkUp connections found');
    }
    var connection = json.data[0];
    var patientId = connection.patientId;
    console.log('LibreLinkUp connection found for patient ' + patientId);
    var measurement = pickMeasurement(connection);
    if (measurement && (measurement.ValueInMgPerDl || measurement.Value)) {
      console.log('Using measurement from connections payload');
      return { measurement: measurement, headers: headers, patientId: patientId };
    }
    console.log('No measurement in connections payload, fetching graph');
    return fetch(baseUrl + '/llu/connections/' + patientId + '/graph', { headers: headers })
      .then(function(resp) {
        if (!resp.ok) {
          throw new Error('Graph failed: HTTP ' + resp.status);
        }
        return resp.json();
      })
      .then(function(graphJson) {
        var points = graphJson && graphJson.data && graphJson.data.graphData && graphJson.data.graphData.length;
        console.log('Graph data points: ' + (points || 0));
        var conn = graphJson && graphJson.data && graphJson.data.connection;
        var graphMeasurement = pickMeasurement(conn);
        return { measurement: graphMeasurement, headers: headers, patientId: patientId };
      });
  })
  .then(function(result) {
    var measurement = result && result.measurement;
    if (!measurement) {
      throw new Error('No glucose measurement available');
    }

    var value = measurement.ValueInMgPerDl || measurement.Value || 0;
    var trend = (typeof measurement.TrendArrow !== 'undefined') ? measurement.TrendArrow : (typeof measurement.Trend !== 'undefined' ? measurement.Trend : -1);
    var tsString = measurement.Timestamp || measurement.FactoryTimestamp;
    var ts = tsString ? Math.floor(new Date(tsString).getTime() / 1000) : Math.floor(Date.now() / 1000);

    updateGlucoseData(value, trend, ts);
    console.log('LibreLinkUp glucose updated: ' + value + ' mg/dL, trend ' + trend + ', ts ' + ts);
    return glucoseData;
  })
  .catch(function(err) {
    console.log('LibreLinkUp fetch failed: ' + err.message);
    return null;
  })
  .finally(function() {
    fetchInFlight = null;
  });

  return fetchInFlight;
}

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
  fetchGlucoseFromLibreLinkUp();
});

