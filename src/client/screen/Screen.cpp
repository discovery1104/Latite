#include "pch.h"
#include "Screen.h"
#include "client/Omoti.h"
#include "client/event/Eventing.h"
#include "ScreenManager.h"
#include "client/event/events/ClickEvent.h"
#include "client/event/events/RenderOverlayEvent.h"
#include "client/event/events/UpdateEvent.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/client/game/GameCore.h"

Screen::Screen() {
	/*
	arrow = LoadCursorW(Omoti::get().dllInst, IDC_ARROW);
	hand = LoadCursorW(Omoti::get().dllInst, IDC_HAND);
	ibeam = LoadCursorW(Omoti::get().dllInst, IDC_IBEAM);
	*/
	// ^ this doesnt work with resources...

	Eventing::get().listen<UpdateEvent>(this, (EventListenerFunc)&Screen::onUpdate, 0);
	Eventing::get().listen<RenderOverlayEvent>(this, (EventListenerFunc)&Screen::onRenderOverlay, 0, true);
	Eventing::get().listen<ClickEvent>(this, (EventListenerFunc)&Screen::onClick, 100, true);
}

void Screen::onUpdate(Event& ev) {
	/*
	switch (cursor) {
	case Cursor::Arrow:
		SetCursor(arrow);
		break;
	case Cursor::Hand:
		SetCursor(hand);
		break;
	case Cursor::IBeam:
		SetCursor(ibeam);
		break;
	}
	*/
	if (!isActive()) return;

	SDK::ClientInstance::get()->releaseCursor();

	SDK::GameCore* gameCore = SDK::GameCore::get();
	if (!gameCore || !gameCore->hwnd) return;
	HWND fg = GetForegroundWindow();
	if (fg && fg != gameCore->hwnd && !IsChild(gameCore->hwnd, fg) && !IsChild(fg, gameCore->hwnd)) {
		polledMouseButtons = { false, false, false };
		mouseButtons = { false, false, false };
		activeMouseButtons = { false, false, false };
		return;
	}

	if (auto clientInstance = SDK::ClientInstance::get()) {
		POINT point{};
		if (GetCursorPos(&point) && ScreenToClient(gameCore->hwnd, &point)) {
			clientInstance->cursorPos = Vec2(static_cast<float>(point.x), static_cast<float>(point.y));
		}
	}

	constexpr std::array<int, 3> kMouseVks = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
	for (size_t i = 0; i < kMouseVks.size(); i++) {
		const bool isDown = (GetAsyncKeyState(kMouseVks[i]) & 0x8000) != 0;
		if (polledMouseButtons[i] == isDown) continue;

		polledMouseButtons[i] = isDown;
		mouseButtons[i] = isDown;
		if (isDown) {
			activeMouseButtons[i] = true;
		}
	}

	MSG msg{};
	while (PeekMessageW(&msg, gameCore->hwnd, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_REMOVE)) {
		const auto delta = std::clamp(static_cast<int>(GET_WHEEL_DELTA_WPARAM(msg.wParam)), -127, 127);
		ClickEvent wheelEvent{ 4, static_cast<char>(delta) };
		Eventing::get().dispatch(wheelEvent);
	}
}

void Screen::close() {
	this->activeMouseButtons = { false, false, false };
	this->mouseButtons = { false, false, false };
	this->polledMouseButtons = { false, false, false };
	Omoti::getScreenManager().exitCurrentScreen();
}

void Screen::playClickSound() {
	util::PlaySoundUI("random.click");
}

void Screen::setTooltip(std::optional<std::wstring> newTooltip) {
	this->tooltip = newTooltip;
}

void Screen::onClick(Event& evGeneric) {
	auto& ev = reinterpret_cast<ClickEvent&>(evGeneric);
	if (ev.getMouseButton() > 0) {
		if (ev.getMouseButton() < 4) {
			if (isActive()) {
				if (ev.isDown())
					this->activeMouseButtons[ev.getMouseButton() - 1] = ev.isDown();
				this->mouseButtons[ev.getMouseButton() - 1] = ev.isDown();
			}
		}
		if (isActive()) ev.setCancelled(true);
	}
}

void Screen::onRenderOverlay(Event& ev) {
	if (this->isActive()) {
		for (size_t i = 0; i < justClicked.size(); i++) {
			justClicked[i] = this->activeMouseButtons[i];
			this->activeMouseButtons[i] = false;
		}

		if (this->tooltip != oldTooltip) {
			this->lastTooltipChange = std::chrono::system_clock::now();
			oldTooltip = tooltip;
		}
	}


	if (isActive() && this->tooltip.has_value()) {
		

		auto now = std::chrono::system_clock::now();
		if (now - lastTooltipChange >= 500ms) {
			D2DUtil dc;
			Vec2& mousePos = SDK::ClientInstance::get()->cursorPos;
			d2d::Rect textRect = dc.getTextRect(this->tooltip.value(), Renderer::FontSelection::PrimaryRegular, 15.f, 8.f);
			textRect.setPos(mousePos);
			auto height = textRect.getHeight() * 0.9f;
			textRect.top -= height;
			textRect.bottom -= height;
			textRect.left += 5.f;
			textRect.right += 5.f;

			float rad = height * 0.25f;
			dc.fillRoundedRectangle(textRect, d2d::Color(0.f, 0.f, 0.f, 0.6f), rad);
			dc.drawRoundedRectangle(textRect, d2d::Color(0.9f, 0.9f, 0.9f, 1.f), rad, 1.f);
			dc.drawText(textRect, this->tooltip.value(), d2d::Color(1.f, 1.f, 1.f, 0.8f), Renderer::FontSelection::PrimaryRegular, 15.f, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		}
	}
	this->tooltip = std::nullopt;
}
