#pragma once

#include <string>
#include <vector>

#include "KarakeepClient.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class KarakeepBrowserActivity final : public Activity {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, BOOKMARK_ACTIONS, DOWNLOADING, ERROR };

  explicit KarakeepBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("KarakeepBrowser", renderer, mappedInput), buttonNavigator() {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct ActionItem {
    std::string label;
    int tag;
  };

  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::CHECK_WIFI;
  std::vector<KarakeepClient::Bookmark> bookmarks;
  int selectorIndex = 0;
  bool consumeConfirm = false;
  bool consumeBack = false;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  bool isOffline = true;

  // Action sub-menu
  std::vector<ActionItem> actions;
  int actionIndex = 0;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchBookmarks();
  void downloadArticle(const KarakeepClient::Bookmark& bm);
  void markRead(const KarakeepClient::Bookmark& bm);
  void markUnread(const KarakeepClient::Bookmark& bm);
  void deleteLocal(const std::string& path);
  void buildActionsFor(const KarakeepClient::Bookmark& bm);
  void buildActionsForLocal(const KarakeepClient::LocalFile& f);
  void executeAction(int tag);
  void scanAndMergeLocal();
  void skipWifiGoOffline();
  bool preventAutoSleep() override { return true; }
};
