#pragma once

#include <functional>
#include <string>
#include <vector>

/**
 * Lightweight HTTP client that speaks to the Karakeep proxy.
 * Uses esp_http_client directly (same pattern as KOReaderSyncClient).
 */
class KarakeepClient {
 public:
  enum Error { OK = 0, NO_CREDENTIALS, NETWORK_ERROR, SERVER_ERROR, JSON_ERROR, LOW_MEMORY };

  struct Bookmark {
    std::string id;
    std::string title;
    bool isRead = false;
    bool isCached = false;   // downloaded locally to /karakeep/
    std::string localPath;   // SD path if cached
  };

  struct LocalFile {
    std::string path;    // /karakeep/filename.epub
    std::string title;   // derived from filename
  };

  /**
   * Fetch bookmarks filtered by tag from the proxy.
   * @param tagFilter Tag name to filter by (e.g. "Sync"). Empty = all.
   * @param out Output vector (caller should .reserve() if desired).
   * @param limit Max bookmarks to return.
   */
  static Error fetchBookmarks(const std::string& tagFilter, std::vector<Bookmark>& out, size_t limit = 20);

  /**
   * Download article content (txt or epub) directly to SD file.
   * @param bookmarkId Karakeep bookmark ID.
   * @param format "txt" or "epub".
   * @param destPath Absolute SD path (e.g. "/karakeep/foo.epub").
   * @param progress Optional callback (downloaded, total).
   */
  static Error downloadContent(const std::string& bookmarkId, const std::string& format,
                                 const std::string& destPath,
                                 std::function<void(size_t, size_t)> progress = nullptr);

  /** Mark bookmark as read via proxy. */
  static Error markRead(const std::string& bookmarkId);

  /** Mark bookmark as unread via proxy. */
  static Error markUnread(const std::string& bookmarkId);

  /** Add or remove tags on a bookmark via proxy. */
  static Error updateTags(const std::string& bookmarkId,
                           const std::vector<std::string>& addTags,
                           const std::vector<std::string>& removeTags);

  /** Scan /karakeep/ for downloaded EPUB files. */
  static void scanLocalFiles(std::vector<LocalFile>& out);

  static const char* errorString(Error error);
};
