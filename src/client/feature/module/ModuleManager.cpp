#include "pch.h"
#include "ModuleManager.h"
#include "modules/misc/TestModule.h"
#include "modules/misc/DebugInfo.h"
#include "modules/misc/Nickname.h"
#include "modules/misc/ItemTweaks.h"
#include "modules/misc/DebugInfo.h"
#include "modules/misc/CommandShortcuts.h"
#include "modules/misc/BlockGame.h"

#include "modules/game/Zoom.h"
#include "modules/game/CinematicCamera.h"
#include "modules/game/ToggleSprintSneak.h"
#include "modules/game/BehindYou.h"
#include "modules/game/ThirdPersonNametag.h"
#include "modules/game/EnvironmentChanger.h"
#include "modules/game/TextHotkey.h"
#include "modules/game/Freelook.h"
#include "modules/game/AutoGG.h"

#include "modules/visual/Fullbright.h"
#include "modules/visual/MotionBlur.h"
#include "modules/visual/HurtColor.h"
#include "modules/visual/Hitboxes.h"
#include "modules/visual/ChunkBorders.h"
#include "modules/visual/Hitboxes.h"
#include "modules/visual/BlockOutline.h"

#include "modules/hud/FPSCounter.h"
#include "modules/hud/CPSCounter.h"
#include "modules/hud/ServerDisplay.h"
#include "modules/hud/PingDisplay.h"
#include "modules/hud/SpeedDisplay.h"
#include "modules/hud/Clock.h"
#include "modules/hud/BowIndicator.h"
#include "modules/hud/GuiscaleChanger.h"
#include "modules/hud/TabList.h"
#include "modules/hud/Keystrokes.h"
#include "modules/hud/BreakIndicator.h"
#include "modules/hud/HealthWarning.h"
#include "modules/hud/ArmorHUD.h"
#include "modules/hud/MovablePaperdoll.h"
#include "modules/hud/MovableScoreboard.h"
#include "modules/hud/ReachDisplay.h"
#include "modules/hud/MovableBossbar.h"
#include "modules/hud/ItemCounter.h"
#include "modules/hud/Chat.h"
#include "modules/hud/ComboCounter.h"
#include "modules/hud/CustomCoordinates.h"
#include "modules/hud/MovableCoordinates.h"
#include "modules/hud/FrameTimeDisplay.h"

#include "client/event/events/KeyUpdateEvent.h"

ModuleManager::ModuleManager() {
	// Intentionally keep the default module list empty.
	// Music GUI is handled directly by ClickGUI.

	for (auto& mod : items) {
		mod->onInit();
	}
	Eventing::get().listen<KeyUpdateEvent>(this, (EventListenerFunc) & ModuleManager::onKey);
}

ModuleManager::~ModuleManager() {
	for (auto& mod : items) {
		if (mod->isEnabled()) mod->setEnabled(false);
	}
}

void ModuleManager::onKey(Event& evGeneric) {
	auto& ev = reinterpret_cast<KeyUpdateEvent&>(evGeneric);
	for (auto& mod : items) {
		if (ev.inUI()) return;
		if (mod->getKeybind() == ev.getKey()) {
			if (mod->shouldHoldToToggle()) {
				if (!mod->isEnabled() && ev.isDown()) {
					mod->setEnabled(true);
				}
				else if (mod->isEnabled() && !ev.isDown()) {
					mod->setEnabled(false);
				}
				continue;
			}
			else if (ev.isDown()) {
				mod->setEnabled(!mod->isEnabled());
			}
		}
	}
}
