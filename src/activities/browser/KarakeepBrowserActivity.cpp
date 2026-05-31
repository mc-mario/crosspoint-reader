#include "KarakeepBrowserActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/StringUtils.h"

namespace {
constexpr int PAGE_ITEMS = 20;
}

void KarakeepBrowserActivity::onEnter() {
  Activity::onEnter();

  state = BrowserState::CHECK_WIFI;
  bookmarks.clear();
  selectorIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  isOffline = true;
  actions.clear();
  actionIndex = 0;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();

  checkAndConnectWifi();
}

void KarakeepBrowserActivity::onExit() {
  Activity::onExit();
  bookmarks.clear();
  actions.clear();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void KarakeepBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::DOWNLOADING) return;

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        state = BrowserState::LOADING;
        statusMessage = tr(STR_LOADING);
        isOffline = false;
        requestUpdate();
        fetchBookmarks();
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) onGoHome();
    return;
  }

  if (state == BrowserState::BROWSING) {
    const bool hasItems = !bookmarks.empty();

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && hasItems) {
      consumeConfirm = true;
      if (isOffline) {
        // Offline mode: build actions for local file
        buildActionsForLocal({bookmarks[selectorIndex].localPath, bookmarks[selectorIndex].title});
      } else {
        buildActionsFor(bookmarks[selectorIndex]);
      }
      state = BrowserState::BOOKMARK_ACTIONS;
      actionIndex = 0;
      requestUpdate();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
      return;
    }

    if (hasItems) {
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, bookmarks.size());
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, bookmarks.size());
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, bookmarks.size(), PAGE_ITEMS);
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, bookmarks.size(), PAGE_ITEMS);
        requestUpdate();
      });
    }
    return;
  }

  if (state == BrowserState::BOOKMARK_ACTIONS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !actions.empty()) {
      consumeConfirm = true;
      executeAction(actions[actionIndex].tag);
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = BrowserState::BROWSING;
      actions.clear();
      requestUpdate();
      return;
    }

    if (!actions.empty()) {
      buttonNavigator.onNextRelease([this] {
        actionIndex = ButtonNavigator::nextIndex(actionIndex, actions.size());
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        actionIndex = ButtonNavigator::previousIndex(actionIndex, actions.size());
        requestUpdate();
      });
    }
    return;
  }
}

void KarakeepBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_12_FONT_ID, 15, isOffline ? tr(STR_KARAKEEP_OFFLINE_TITLE) : tr(STR_KARAKEEP_BROWSER), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_KARAKEEP_BROWSER), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_KARAKEEP_BROWSER), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_KARAKEEP_DOWNLOADING));
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  // BROWSING and BOOKMARK_ACTIONS share the same background
  renderer.drawCenteredText(UI_12_FONT_ID, 15, isOffline ? tr(STR_KARAKEEP_OFFLINE_TITLE) : tr(STR_KARAKEEP_BROWSER), true, EpdFontFamily::BOLD);

  if (bookmarks.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_KARAKEEP_NO_BOOKMARKS));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = (selectorIndex / PAGE_ITEMS) * PAGE_ITEMS;

  for (size_t i = static_cast<size_t>(pageStartIndex);
       i < bookmarks.size() && i < static_cast<size_t>(pageStartIndex + PAGE_ITEMS); i++) {
    const auto& bm = bookmarks[i];
    std::string displayText = bm.title;
    if (bm.isRead) displayText += " [R]";
    if (bm.isCached) displayText += " [+]";
    auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), pageWidth - 40);
    renderer.drawText(UI_10_FONT_ID, 20, 50 + (i % PAGE_ITEMS) * 20 - 2, item.c_str(),
                      i != static_cast<size_t>(selectorIndex));
    // Highlight selected
    if (i == static_cast<size_t>(selectorIndex)) {
      renderer.fillRect(0, 50 + (i % PAGE_ITEMS) * 20 - 2, pageWidth - 1, 20);
      renderer.drawText(UI_10_FONT_ID, 20, 50 + (i % PAGE_ITEMS) * 20 - 2, item.c_str(), false);
    }
  }

  if (state == BrowserState::BROWSING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == BrowserState::BOOKMARK_ACTIONS) {
    // Draw action sub-menu below the title
    int y = 50 + (selectorIndex % PAGE_ITEMS) * 20 + 25;
    renderer.drawRect(10, y, pageWidth - 20,
                       static_cast<int>(actions.size()) * 20 + 10);
    for (size_t i = 0; i < actions.size(); i++) {
      int ay = y + 8 + static_cast<int>(i) * 20;
      if (static_cast<int>(i) == actionIndex) {
        renderer.fillRect(15, ay - 1, pageWidth - 30, 20);
        renderer.drawText(UI_10_FONT_ID, 25, ay, actions[i].label.c_str(), false);
      } else {
        renderer.drawText(UI_10_FONT_ID, 25, ay, actions[i].label.c_str(), true);
      }
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

void KarakeepBrowserActivity::buildActionsFor(const KarakeepClient::Bookmark& bm) {
  actions.clear();
  if (bm.isCached) {
    actions.push_back({tr(STR_KARAKEEP_READ_EPUB), 1});
    actions.push_back({tr(STR_KARAKEEP_DOWNLOAD_AGAIN), 2});
  } else {
    actions.push_back({tr(STR_DOWNLOAD), 2});
  }
  if (bm.isRead) {
    actions.push_back({tr(STR_KARAKEEP_MARK_UNREAD), 4});
  } else {
    actions.push_back({tr(STR_KARAKEEP_MARK_READ), 3});
  }
  if (bm.isCached) {
    actions.push_back({tr(STR_KARAKEEP_DELETE), 5});
  }
  actions.push_back({tr(STR_BACK), 0});
}

void KarakeepBrowserActivity::buildActionsForLocal(const KarakeepClient::LocalFile& f) {
  actions.clear();
  actions.push_back({tr(STR_KARAKEEP_READ_EPUB), 1});
  actions.push_back({tr(STR_KARAKEEP_DELETE), 5});
  actions.push_back({tr(STR_BACK), 0});
}

void KarakeepBrowserActivity::executeAction(int tag) {
  if (tag == 0) {
    // Back
    state = BrowserState::BROWSING;
    actions.clear();
    requestUpdate();
    return;
  }

  if (isOffline) {
    const auto& bm = bookmarks[selectorIndex];
    if (tag == 1) {
      // Read
      activityManager.goToReader(bm.localPath);
      return;
    }
    if (tag == 5) {
      // Delete
      deleteLocal(bm.localPath);
      return;
    }
  }

  const auto& bm = bookmarks[selectorIndex];

  if (tag == 1 || tag == 2) {
    // Download (1=read cached, 2=download new)
    if (tag == 1) {
      activityManager.goToReader(bm.localPath);
    } else {
      downloadArticle(bm);
    }
  } else if (tag == 3) {
    markRead(bm);
  } else if (tag == 4) {
    markUnread(bm);
  } else if (tag == 5) {
    deleteLocal(bm.localPath);
  }
}

void KarakeepBrowserActivity::markRead(const KarakeepClient::Bookmark& bm) {
  const auto result = KarakeepClient::markRead(bm.id);
  if (result == KarakeepClient::OK) {
    // Refresh the list
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchBookmarks();
  } else {
    state = BrowserState::ERROR;
    errorMessage = KarakeepClient::errorString(result);
    requestUpdate();
  }
}

void KarakeepBrowserActivity::markUnread(const KarakeepClient::Bookmark& bm) {
  const auto result = KarakeepClient::markUnread(bm.id);
  if (result == KarakeepClient::OK) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchBookmarks();
  } else {
    state = BrowserState::ERROR;
    errorMessage = KarakeepClient::errorString(result);
    requestUpdate();
  }
}

void KarakeepBrowserActivity::deleteLocal(const std::string& path) {
  if (!path.empty() && Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
    LOG_DBG("KBR", "Deleted: %s", path.c_str());
  }
  // Refresh
  state = BrowserState::BROWSING;
  actions.clear();
  if (isOffline) {
    scanAndMergeLocal();
  } else {
    fetchBookmarks();
  }
  requestUpdate();
}

void KarakeepBrowserActivity::downloadArticle(const KarakeepClient::Bookmark& bm) {
  state = BrowserState::DOWNLOADING;
  statusMessage = bm.title;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);

  Storage.mkdir("/karakeep");
  std::string safeTitle = StringUtils::sanitizeFilename(bm.title, 50);
  // Remove leading/trailing underscores
  while (!safeTitle.empty() && safeTitle.front() == '_') safeTitle.erase(0, 1);
  while (!safeTitle.empty() && safeTitle.back() == '_') safeTitle.pop_back();
  std::string destPath = "/karakeep/" + safeTitle + ".epub";

  LOG_DBG("KBR", "Downloading: %s -> %s", bm.id.c_str(), destPath.c_str());

  const auto result = KarakeepClient::downloadContent(
      bm.id, "epub", destPath,
      [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        requestUpdate(true);
      });

  if (result == KarakeepClient::OK) {
    KarakeepClient::markRead(bm.id);
    activityManager.goToReader(destPath);
  } else {
    state = BrowserState::ERROR;
    errorMessage = KarakeepClient::errorString(result);
    requestUpdate();
  }
}

void KarakeepBrowserActivity::fetchBookmarks() {
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate();

  auto result = KarakeepClient::fetchBookmarks("Sync", bookmarks, 50);
  if (result == KarakeepClient::OK) {
    scanAndMergeLocal();
    if (bookmarks.empty()) {
      // Still try offline as fallback
      state = BrowserState::LOADING;
      statusMessage = tr(STR_LOADING);
      requestUpdate();
      skipWifiGoOffline();
    } else {
      selectorIndex = 0;
      state = BrowserState::BROWSING;
    }
  } else {
    state = BrowserState::ERROR;
    errorMessage = KarakeepClient::errorString(result);
  }
  requestUpdate();
}

void KarakeepBrowserActivity::scanAndMergeLocal() {
  std::vector<KarakeepClient::LocalFile> localFiles;
  KarakeepClient::scanLocalFiles(localFiles);

  // Mark online bookmarks as cached
  for (auto& bm : bookmarks) {
    std::string safeTitle = StringUtils::sanitizeFilename(bm.title, 50);
    while (!safeTitle.empty() && safeTitle.front() == '_') safeTitle.erase(0, 1);
    while (!safeTitle.empty() && safeTitle.back() == '_') safeTitle.pop_back();
    for (const auto& lf : localFiles) {
      if (lf.path == "/karakeep/" + safeTitle + ".epub") {
        bm.isCached = true;
        bm.localPath = lf.path;
        break;
      }
    }
  }

  // Add local-only files (downloaded but not in online list) at the end
  for (const auto& lf : localFiles) {
    bool found = false;
    for (const auto& bm : bookmarks) {
      if (bm.localPath == lf.path) {
        found = true;
        break;
      }
    }
    if (!found && !isOffline) {
      // Only add orphaned local files in online mode
    }
  }
}

void KarakeepBrowserActivity::skipWifiGoOffline() {
  isOffline = true;
  bookmarks.clear();
  std::vector<KarakeepClient::LocalFile> localFiles;
  KarakeepClient::scanLocalFiles(localFiles);
  for (const auto& lf : localFiles) {
    KarakeepClient::Bookmark bm;
    bm.title = lf.title;
    bm.localPath = lf.path;
    bm.isCached = true;
    bm.isRead = false;
    bookmarks.push_back(bm);
  }
  if (bookmarks.empty()) {
    state = BrowserState::ERROR;
    errorMessage = "No downloaded articles and no WiFi connection";
  } else {
    selectorIndex = 0;
    state = BrowserState::BROWSING;
  }
}

void KarakeepBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    isOffline = false;
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchBookmarks();
    return;
  }
  launchWifiSelection();
}

void KarakeepBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KarakeepBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    isOffline = false;
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchBookmarks();
  } else {
    // Go offline
    skipWifiGoOffline();
  }
}
