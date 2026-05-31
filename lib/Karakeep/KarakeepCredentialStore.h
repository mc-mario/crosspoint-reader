#pragma once

#include <cstdint>
#include <string>

/**
 * Singleton credential store for the Karakeep proxy connection.
 * Stores proxy URL and API key in JSON on the SD card.
 */
class KarakeepCredentialStore {
 private:
  static KarakeepCredentialStore instance;
  std::string proxyUrl;
  std::string apiKey;

  KarakeepCredentialStore() = default;

 public:
  KarakeepCredentialStore(const KarakeepCredentialStore&) = delete;
  KarakeepCredentialStore& operator=(const KarakeepCredentialStore&) = delete;

  static KarakeepCredentialStore& getInstance() { return instance; }

  bool hasCredentials() const { return !proxyUrl.empty(); }

  const std::string& getProxyUrl() const { return proxyUrl; }
  const std::string& getApiKey() const { return apiKey; }

  void setCredentials(const std::string& url, const std::string& key);

  bool saveToFile() const;
  bool loadFromFile();
};

#define KARAKEEP_STORE KarakeepCredentialStore::getInstance()
