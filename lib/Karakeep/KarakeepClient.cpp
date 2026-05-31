#include "KarakeepClient.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <StreamingJsonParser.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <cstring>

#include "KarakeepCredentialStore.h"

namespace {
// Small TLS buffers to fit ESP32-C3's limited heap (~46KB free after WiFi).
constexpr int HTTP_BUF_SIZE = 2048;
constexpr uint32_t MIN_HEAP_FOR_TLS = 55000;
constexpr int HTTP_TIMEOUT_MS = 15000;

struct ResponseBuffer {
  char* data = nullptr;
  int len = 0;
  int capacity = 0;

  ~ResponseBuffer() { free(data); }

  bool ensure(int size) {
    if (size <= capacity) return true;
    char* newData = (char*)realloc(data, size);
    if (!newData) return false;
    data = newData;
    capacity = size;
    return true;
  }
};

esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
    if (buf->ensure(buf->len + evt->data_len + 1)) {
      memcpy(buf->data + buf->len, evt->data, evt->data_len);
      buf->len += evt->data_len;
      buf->data[buf->len] = '\0';
    } else {
      LOG_ERR("KCP", "Response buffer allocation failed (%d bytes)", evt->data_len);
    }
  }
  return ESP_OK;
}

esp_http_client_handle_t createClient(const char* url, ResponseBuffer* buf,
                                     esp_http_client_method_t method = HTTP_METHOD_GET) {
  esp_http_client_config_t config = {};
  config.url = url;
  config.event_handler = httpEventHandler;
  config.user_data = buf;
  config.method = method;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.buffer_size = HTTP_BUF_SIZE;
  config.buffer_size_tx = HTTP_BUF_SIZE;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return nullptr;

  if (esp_http_client_set_header(client, "Authorization",
                                 ("Bearer " + KARAKEEP_STORE.getApiKey()).c_str()) != ESP_OK) {
    LOG_ERR("KCP", "Failed to set auth header");
    esp_http_client_cleanup(client);
    return nullptr;
  }

  return client;
}

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Helper to build proxy URL
std::string buildProxyUrl(const std::string& path) {
  std::string url = KARAKEEP_STORE.getProxyUrl();
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url + path;
}
}  // namespace

KarakeepClient::Error KarakeepClient::fetchBookmarks(const std::string& tagFilter,
                                                      std::vector<Bookmark>& out, size_t limit) {
  if (!KARAKEEP_STORE.hasCredentials()) {
    LOG_DBG("KCP", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = buildProxyUrl("/bookmarks?limit=" + std::to_string(limit));
  if (!tagFilter.empty()) {
    url += "&tag=" + tagFilter;
  }

  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("KCP", "Fetching bookmarks: %s (heap: %u)", url.c_str(), (unsigned)freeHeap);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("KCP", "Insufficient heap for TLS: %u bytes free (need %u)", freeHeap, MIN_HEAP_FOR_TLS);
    return LOW_MEMORY;
  }

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) return NETWORK_ERROR;

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  LOG_DBG("KCP", "Bookmarks response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode != 200) return SERVER_ERROR;
  if (!buf.data || buf.len == 0) return SERVER_ERROR;

  // Parse JSON with StreamingJsonParser
  out.clear();
  // Conservative reserve: the proxy already filtered server-side
  out.reserve(limit > 50 ? 50 : limit);

  struct ParseCtx {
    std::vector<Bookmark>* bookmarks;
    Bookmark current;
    enum { NONE, ID, TITLE, IS_READ } expect = NONE;
    bool inBookmarksArray = false;
  };

  ParseCtx ctx{};
  ctx.bookmarks = &out;

  JsonCallbacks cb{};
  cb.ctx = &ctx;
  cb.onKey = [](void* c, const char* key, size_t len) {
    auto* ctx = static_cast<ParseCtx*>(c);
    if (len == 2 && memcmp(key, "id", 2) == 0) {
      ctx->expect = ParseCtx::ID;
    } else if (len == 5 && memcmp(key, "title", 5) == 0) {
      ctx->expect = ParseCtx::TITLE;
    } else if (len == 6 && memcmp(key, "isRead", 6) == 0) {
      ctx->expect = ParseCtx::IS_READ;
    } else {
      ctx->expect = ParseCtx::NONE;
    }
  };
  cb.onString = [](void* c, const char* val, size_t len) {
    auto* ctx = static_cast<ParseCtx*>(c);
    if (ctx->expect == ParseCtx::ID) {
      ctx->current.id.assign(val, len);
    } else if (ctx->expect == ParseCtx::TITLE) {
      ctx->current.title.assign(val, len);
    }
    ctx->expect = ParseCtx::NONE;
  };
  cb.onBool = [](void* c, bool val) {
    auto* ctx = static_cast<ParseCtx*>(c);
    if (ctx->expect == ParseCtx::IS_READ) {
      ctx->current.isRead = val;
    }
    ctx->expect = ParseCtx::NONE;
  };
  cb.onObjectStart = [](void* c) {
    auto* ctx = static_cast<ParseCtx*>(c);
    // Reset current bookmark on each new object inside bookmarks array
    if (ctx->inBookmarksArray) {
      ctx->current = Bookmark{};
    }
  };
  cb.onObjectEnd = [](void* c) {
    auto* ctx = static_cast<ParseCtx*>(c);
    if (ctx->inBookmarksArray && !ctx->current.id.empty()) {
      ctx->bookmarks->push_back(ctx->current);
      ctx->current = Bookmark{};
    }
  };
  cb.onArrayStart = [](void* c) {
    auto* ctx = static_cast<ParseCtx*>(c);
    // We enter the "bookmarks" array; simplistic: assume any array start is bookmarks
    ctx->inBookmarksArray = true;
  };

  StreamingJsonParser parser(cb);
  parser.feed(buf.data, buf.len);

  if (parser.hasError()) {
    LOG_ERR("KCP", "JSON parse error");
    return JSON_ERROR;
  }

  LOG_DBG("KCP", "Parsed %zu bookmarks", out.size());
  return OK;
}

KarakeepClient::Error KarakeepClient::downloadContent(const std::string& bookmarkId,
                                                         const std::string& format,
                                                         const std::string& destPath,
                                                         std::function<void(size_t, size_t)> progress) {
  if (!KARAKEEP_STORE.hasCredentials()) {
    LOG_DBG("KCP", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = buildProxyUrl("/bookmarks/" + bookmarkId + "/content?format=" + format);

  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("KCP", "Downloading: %s -> %s (heap: %u)", url.c_str(), destPath.c_str(), (unsigned)freeHeap);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("KCP", "Insufficient heap for TLS: %u bytes free (need %u)", freeHeap, MIN_HEAP_FOR_TLS);
    return LOW_MEMORY;
  }

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  HalFile file;
  if (!Storage.openFileForWrite("KCP", destPath.c_str(), file)) {
    LOG_ERR("KCP", "Failed to open dest file for writing");
    return NETWORK_ERROR;
  }

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.buffer_size = HTTP_BUF_SIZE;
  config.buffer_size_tx = HTTP_BUF_SIZE;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("KCP", "client init failed");
    return NETWORK_ERROR;
  }

  if (esp_http_client_set_header(client, "Authorization",
                                 ("Bearer " + KARAKEEP_STORE.getApiKey()).c_str()) != ESP_OK) {
    LOG_ERR("KCP", "Failed to set auth header");
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    LOG_ERR("KCP", "open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  int64_t contentLength = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  for (int hop = 0; isRedirect(status) && hop < 5; ++hop) {
    if (esp_http_client_set_redirection(client) != ESP_OK) break;
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) break;
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
  }

  if (status != 200) {
    LOG_ERR("KCP", "unexpected status: %d", status);
    esp_http_client_cleanup(client);
    return SERVER_ERROR;
  }

  size_t total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;
  size_t downloaded = 0;

  auto buf = makeUniqueNoThrow<char[]>(HTTP_BUF_SIZE);
  if (!buf) {
    LOG_ERR("KCP", "OOM: %d byte read buffer", HTTP_BUF_SIZE);
    esp_http_client_cleanup(client);
    return LOW_MEMORY;
  }

  while (true) {
    const int read = esp_http_client_read(client, buf.get(), HTTP_BUF_SIZE);
    if (read < 0) {
      LOG_ERR("KCP", "read error after %zu bytes", downloaded);
      esp_http_client_cleanup(client);
      Storage.remove(destPath.c_str());
      return NETWORK_ERROR;
    }
    if (read == 0) break;
    if (file.write(reinterpret_cast<const uint8_t*>(buf.get()), read) != read) {
      LOG_ERR("KCP", "write error after %zu bytes", downloaded);
      esp_http_client_cleanup(client);
      Storage.remove(destPath.c_str());
      return NETWORK_ERROR;
    }
    downloaded += read;
    if (progress && total > 0) {
      progress(downloaded, total);
    }
  }

  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);

  if (!complete) {
    LOG_ERR("KCP", "incomplete download: %zu of %zu bytes", downloaded, total);
    Storage.remove(destPath.c_str());
    return NETWORK_ERROR;
  }

  LOG_DBG("KCP", "Downloaded %zu bytes to %s", downloaded, destPath.c_str());
  return OK;
}

KarakeepClient::Error KarakeepClient::markRead(const std::string& bookmarkId) {
  if (!KARAKEEP_STORE.hasCredentials()) return NO_CREDENTIALS;

  std::string url = buildProxyUrl("/bookmarks/" + bookmarkId + "/read");

  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("KCP", "Insufficient heap for TLS");
    return LOW_MEMORY;
  }

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_POST);
  if (!client) return NETWORK_ERROR;

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (status != 200) return SERVER_ERROR;
  return OK;
}

KarakeepClient::Error KarakeepClient::markUnread(const std::string& bookmarkId) {
  if (!KARAKEEP_STORE.hasCredentials()) return NO_CREDENTIALS;

  std::string url = buildProxyUrl("/bookmarks/" + bookmarkId + "/unread");

  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("KCP", "Insufficient heap for TLS");
    return LOW_MEMORY;
  }

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_POST);
  if (!client) return NETWORK_ERROR;

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (status != 200) return SERVER_ERROR;
  return OK;
}

KarakeepClient::Error KarakeepClient::updateTags(const std::string& bookmarkId,
                                                    const std::vector<std::string>& addTags,
                                                    const std::vector<std::string>& removeTags) {
  if (!KARAKEEP_STORE.hasCredentials()) return NO_CREDENTIALS;

  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_HEAP_FOR_TLS) return LOW_MEMORY;

  std::string url = buildProxyUrl("/bookmarks/" + bookmarkId + "/tags");

  // Build JSON body
  std::string body = R"({"add":[)";
  for (size_t i = 0; i < addTags.size(); ++i) {
    if (i > 0) body += ",";
    body += "\"" + addTags[i] + "\"";
  }
  body += R"(],"remove":[)";
  for (size_t i = 0; i < removeTags.size(); ++i) {
    if (i > 0) body += ",";
    body += "\"" + removeTags[i] + "\"";
  }
  body += "]}";

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.event_handler = httpEventHandler;
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.buffer_size = HTTP_BUF_SIZE;
  config.buffer_size_tx = HTTP_BUF_SIZE;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  ResponseBuffer buf;
  config.user_data = &buf;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return NETWORK_ERROR;

  if (esp_http_client_set_header(client, "Authorization",
                                  ("Bearer " + KARAKEEP_STORE.getApiKey()).c_str()) != ESP_OK) {
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body.c_str(), body.size());

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (status != 200) return SERVER_ERROR;
  return OK;
}

void KarakeepClient::scanLocalFiles(std::vector<LocalFile>& out) {
  out.clear();
  auto files = Storage.listFiles("/karakeep");
  for (const auto& f : files) {
    std::string name = f.c_str();
    if (name.size() < 5 || name.substr(name.size() - 5) != ".epub") continue;
    LocalFile lf;
    lf.path = "/karakeep/" + name;
    std::string title = name.substr(0, name.size() - 5);  // strip .epub
    // Trim leading/trailing underscores from sanitization
    while (!title.empty() && title.front() == '_') title.erase(0, 1);
    while (!title.empty() && title.back() == '_') title.pop_back();
    if (title.empty()) title = name;
    lf.title = title;
    out.push_back(lf);
  }
}

const char* KarakeepClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return "No credentials configured";
    case NETWORK_ERROR:
      return "Network error";
    case SERVER_ERROR:
      return "Server error";
    case JSON_ERROR:
      return "JSON parse error";
    case LOW_MEMORY:
      return "Not enough memory";
    default:
      return "Unknown error";
  }
}
