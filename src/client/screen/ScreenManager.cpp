#include "pch.h"
#include "ScreenManager.h"
#include "screens/ClickGUI.h"
#include "mc/common/client/game/ClientInstance.h"
#include "client/event/events/KeyUpdateEvent.h"
#include "util/Logger.h"

ScreenManager::ScreenManager() {
	Eventing::get().listen<KeyUpdateEvent, &ScreenManager::onKey>(this, 200);
	Eventing::get().listen<FocusLostEvent, &ScreenManager::onFocusLost>(this);
}

void ScreenManager::exitCurrentScreen() {
	if (this->activeScreen) {
		this->activeScreen->get().setActive(false);
		this->activeScreen->get().onDisable();
		this->activeScreen = std::nullopt;
		SDK::ClientInstance::get()->grabCursor();
	}
}

void ScreenManager::onKey(KeyUpdateEvent& ev) {
	const int key = ev.getKey();
	const bool keyTracked = key >= 0 && key < static_cast<int>(toggleKeysHeld.size());

	if (!ev.isDown()) {
		if (keyTracked) toggleKeysHeld[key] = false;
		return;
	}

	const bool wasHeld = keyTracked && toggleKeysHeld[key];
	if (wasHeld) {
		if (key == VK_ESCAPE || getActiveScreen()) {
			ev.setCancelled(true);
		}
		return;
	}

	if (ev.getKey() == VK_ESCAPE && getActiveScreen()) {
		if (keyTracked) toggleKeysHeld[key] = true;
		Logger::Info("ScreenManager: closing active screen with ESC");
		exitCurrentScreen();
		ev.setCancelled(true);
		return;
	}

	std::optional<std::reference_wrapper<Screen>> associatedScreen;
	this->forEach([&](Screen& s) {
		if (s.key == ev.getKey()) associatedScreen = s;
		});

	if (associatedScreen && ev.isDown()
		&& (!ev.inUI() || getActiveScreen())) {
		if (keyTracked) toggleKeysHeld[key] = true;
		Logger::Info("ScreenManager: key {} matched screen toggle", ev.getKey());
		if (getActiveScreen())
			exitCurrentScreen();
		else {
			Logger::Info("ScreenManager: enabling screen");
			this->activeScreen = associatedScreen;
			associatedScreen->get().setActive(true);
			associatedScreen->get().onEnable(false);
			Logger::Info("ScreenManager: screen enabled");
		}
		ev.setCancelled(true);
		return;
	}
}

void ScreenManager::onFocusLost(FocusLostEvent& ev) {
	toggleKeysHeld.fill(false);
	if (getActiveScreen()) {
		ev.setCancelled(true);
	}
}
