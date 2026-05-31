#pragma once

#include <string>
#include <vector>

#include "KarakeepClient.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class KarakeepBrowserActivity final : public Activity {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR };

  explicit KarakeepBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("KarakeepBrowser", renderer, mappedInput), buttonNavigator() {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
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

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchBookmarks();
  void downloadArticle(const KarakeepClient::Bookmark& bm);
  bool preventAutoSleep() override { return true; }
};
