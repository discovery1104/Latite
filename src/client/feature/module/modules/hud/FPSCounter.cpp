#include "pch.h"
#include "FPSCounter.h"
#include "client/Omoti.h"

FPSCounter::FPSCounter() : TextModule("FPS", LocalizeString::get("client.textmodule.fpsCounter.name"),
                                      LocalizeString::get("client.textmodule.fpsCounter.desc"), HUD) {
    this->prefix = TextValue(L"FPS: ");
}

std::wstringstream FPSCounter::text(bool isDefault, bool inEditor) {
	std::wstringstream wss;
	wss << Omoti::get().getTimings().getFPS();
	return wss;
}
