#include "KarakeepCredentialStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

namespace {
constexpr char KARAKEEP_FILE[] = "/.crosspoint/karakeep.json";

constexpr char JSON_URL_KEY[] = "\"proxyUrl\":\"";
constexpr char JSON_KEY_KEY[] = "\"apiKey\":\"";

void writeEscapedJsonString(HalFile& file, const char* key, const std::string& value) {
  file.write(key, strlen(key));
  file.write(value.c_str(), value.size());
  file.write("\"", 1);
}

bool readJsonStringValue(const std::string& json, const char* key, std::string& out) {
  size_t pos = json.find(key);
  if (pos == std::string::npos) return false;
  pos += strlen(key);
  size_t end = json.find('"', pos);
  if (end == std::string::npos) return false;
  out = json.substr(pos, end - pos);
  return true;
}
}  // namespace

void KarakeepCredentialStore::setCredentials(const std::string& url, const std::string& key) {
  proxyUrl = url;
  apiKey = key;
  LOG_DBG("KCP", "Set proxy URL: %s", url.empty() ? "(empty)" : url.c_str());
}

bool KarakeepCredentialStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  HalFile file;
  if (!Storage.openFileForWrite("KCP", KARAKEEP_FILE, file)) {
    LOG_ERR("KCP", "Failed to open file for writing");
    return false;
  }

  file.write("{", 1);
  writeEscapedJsonString(file, JSON_URL_KEY, proxyUrl);
  file.write(",", 1);
  writeEscapedJsonString(file, JSON_KEY_KEY, apiKey);
  file.write("}", 1);

  LOG_DBG("KCP", "Saved credentials to %s", KARAKEEP_FILE);
  return true;
}

bool KarakeepCredentialStore::loadFromFile() {
  if (!Storage.exists(KARAKEEP_FILE)) {
    LOG_DBG("KCP", "No credentials file found");
    return false;
  }

  String json = Storage.readFile(KARAKEEP_FILE);
  if (json.isEmpty()) {
    LOG_DBG("KCP", "Empty credentials file");
    return false;
  }

  std::string url, key;
  bool hasUrl = readJsonStringValue(json.c_str(), JSON_URL_KEY, url);
  bool hasKey = readJsonStringValue(json.c_str(), JSON_KEY_KEY, key);

  if (hasUrl) proxyUrl = url;
  if (hasKey) apiKey = key;

  LOG_DBG("KCP", "Loaded credentials: url=%s key=%s", hasUrl ? "yes" : "no", hasKey ? "yes" : "no");
  return hasUrl;
}
