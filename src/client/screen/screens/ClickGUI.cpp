#include "pch.h"
#include "ClickGUI.h"
#include "client/event/Eventing.h"
#include "client/event/events/RenderOverlayEvent.h"
#include "client/event/events/RendererCleanupEvent.h"
#include "client/event/events/RendererInitEvent.h"
#include "client/event/events/KeyUpdateEvent.h"
#include "client/event/events/ClickEvent.h"
#include "client/event/events/CharEvent.h"
#include "client/render/Renderer.h"
#include "client/Omoti.h"
#include "client/feature/module/Module.h"
#include "client/feature/module/ModuleManager.h"
#include "util/DrawContext.h"
#include "../../render/asset/Assets.h"
#include "client/resource/InitResources.h"
#include "client/config/ConfigManager.h"
#include "client/misc/MusicHelperClient.h"
#include "util/Logger.h"

#include "../ScreenManager.h"

#include <type_traits>
#include <filesystem>
#include <cwctype>
#include <mmsystem.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl.h>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

#ifdef min
#undef min
#undef max
#endif
#include <client/feature/module/HUDModule.h>

#include <optional>
#include <array>
#include <cstring>
#include <string>
#include <regex>

using FontSelection = Renderer::FontSelection;
using RectF = d2d::Rect;

float calcAnim = 0.f;

namespace {
	static constexpr float setting_height_relative = 0.0168f; // 0.0168
	static constexpr bool kMusicGuiSafeOnly = true;

	struct MusicTrack {
		std::filesystem::path folderPath = {};
		std::filesystem::path path = {};
		std::filesystem::path jacketPath = {};
		std::wstring title = {};
		std::wstring artist = {};
		ComPtr<ID2D1Bitmap> artwork = {};
		bool artworkLoaded = false;
	};

	std::wstring getTrackDisplayTitle(MusicTrack const& track);
	std::wstring getTrackDisplayArtist(MusicTrack const& track);
	MusicTrack makeMusicTrack(std::filesystem::path const& path);
	bool ensureTrackArtwork(MusicTrack& track, ID2D1DeviceContext* ctx);
	std::filesystem::path getSongJsonPath(std::filesystem::path const& folderPath);
	std::filesystem::path findPreferredJacket(std::filesystem::path const& folderPath);
	std::optional<MusicTrack> tryLoadSongFolder(std::filesystem::path const& folderPath);
	std::optional<MusicTrack> migrateLooseTrackIntoFolder(std::filesystem::path const& audioPath);
	void writeSongJson(MusicTrack const& track);

	struct MusicGuiState {
		std::filesystem::path folderPath = {};
		std::vector<MusicTrack> tracks = {};
		int selectedTrack = -1;
		int currentTrack = -1;
		bool paused = false;
		bool repeat = false;
		bool shuffle = false;
		float volume = 1.f;
		ULONGLONG nextControlActionTick = 0;
		ULONGLONG nextSeekActionTick = 0;
		ULONGLONG nextVolumeActionTick = 0;
		bool initialized = false;
		bool needsRefresh = false;
		bool metaLoaded = false;
		enum class FilterMode {
			All,
			Favorites,
			Playlist
		} filterMode = FilterMode::All;
		std::unordered_set<std::wstring> favorites = {};
		std::unordered_map<std::wstring, std::vector<std::wstring>> playlists = {};
		std::wstring activePlaylist = L"Favorites";
		ULONGLONG playStartTick = 0;
		int stoppedPollStreak = 0;
		ULONGLONG uiStabilizeUntilTick = 0;
		ULONGLONG uiLastTick = 0;
		int uiLengthMs = -1;
		int uiPosMs = 0;
		int modePausedStreak = 0;
		int modePlayingStreak = 0;
		float miniHudNormX = -1.f;
		float miniHudNormY = -1.f;
		float libraryScroll = 0.f;
		float libraryScrollMax = 0.f;
		std::optional<RectF> libraryViewport = std::nullopt;
		bool miniHudDragging = false;
		Vec2 miniHudDragOffset = {};
		bool miniHudPrevLmbDown = false;
	};

	struct MusicPlaybackSnapshot {
		ULONGLONG nowTick = 0;
		bool inUiStabilize = false;
		bool modePaused = false;
		bool modePlaying = false;
		bool modeStopped = true;
	};

	MusicGuiState gMusicState{};
	constexpr float kMusicAlphaClear = 0.0f;
	constexpr float kMusicAlphaGlass = 0.24f;
	constexpr float kMusicAlphaGlassHover = 0.32f;
	constexpr float kMusicAlphaGlassActive = 0.40f;
	constexpr float kMusicAlphaMiniHud = 0.48f;
	constexpr float kMusicAlphaStatePill = 0.88f;
	constexpr float kMusicAlphaArtworkFrame = 0.96f;
	constexpr float kMusicAlphaGlyph = 0.94f;
	constexpr float kMusicAlphaFallbackArt = 0.10f;
	constexpr float kMusicAlphaScrollbarTrack = 0.16f;
	void loadMusicMeta(MusicGuiState& state);
	std::wstring getMusicMode();
	bool playAdjacentTrack(MusicGuiState& state, int direction);

	enum class SvgIconKind {
		Play,
		Pause,
		Prev,
		Next,
		Shuffle,
		Repeat,
		Refresh
	};

	struct SvgIconCache {
		ID2D1DeviceContext* owner = nullptr;
		ComPtr<ID2D1DeviceContext5> ctx5;
		ComPtr<ID2D1SvgDocument> play;
		ComPtr<ID2D1SvgDocument> pause;
		ComPtr<ID2D1SvgDocument> prev;
		ComPtr<ID2D1SvgDocument> next;
		ComPtr<ID2D1SvgDocument> shuffle;
		ComPtr<ID2D1SvgDocument> repeat;
		ComPtr<ID2D1SvgDocument> refresh;
	};
	SvgIconCache gSvgIcons{};

	struct GlassBlurScratch {
		ID2D1DeviceContext* owner = nullptr;
		D2D1_SIZE_U size = D2D1::SizeU(0, 0);
		D2D1_BITMAP_OPTIONS options = D2D1_BITMAP_OPTIONS_NONE;
		D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
		ComPtr<ID2D1Bitmap1> bitmap;
	};
	GlassBlurScratch gGlassBlurScratch{};
	GlassBlurScratch gGlassBlurDownsampleScratch{};
	bool gLoggedGlassBlurCopyFailure = false;

	ID2D1Bitmap1* ensureGlassBlurScratch(ID2D1DeviceContext* ctx, D2D1_SIZE_U requiredSize, D2D1_BITMAP_OPTIONS options = D2D1_BITMAP_OPTIONS_NONE, D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_IGNORE, GlassBlurScratch& slot = gGlassBlurScratch) {
		if (!ctx || requiredSize.width == 0 || requiredSize.height == 0) return nullptr;
		bool needsRecreate = slot.owner != ctx || !slot.bitmap
			|| slot.size.width < requiredSize.width
			|| slot.size.height < requiredSize.height
			|| slot.options != options
			|| slot.alphaMode != alphaMode;
		if (!needsRecreate) return slot.bitmap.Get();

		slot.owner = ctx;
		slot.size = requiredSize;
		slot.options = options;
		slot.alphaMode = alphaMode;
		slot.bitmap = nullptr;

		float dpiX = 96.f;
		float dpiY = 96.f;
		ctx->GetDpi(&dpiX, &dpiY);
		auto renderPixelFormat = Omoti::getRenderer().getBitmap()->GetPixelFormat();
		auto blurPixelFormat = D2D1::PixelFormat(renderPixelFormat.format, alphaMode);
		ComPtr<ID2D1Bitmap1> newBitmap;
		if (FAILED(ctx->CreateBitmap(
			requiredSize,
			nullptr,
			0,
			D2D1::BitmapProperties1(options, blurPixelFormat, dpiX, dpiY),
			newBitmap.GetAddressOf()
		))) {
			return nullptr;
		}

		slot.bitmap = std::move(newBitmap);
		return slot.bitmap.Get();
	}

	Resource getSvgResource(SvgIconKind kind) {
		switch (kind) {
		case SvgIconKind::Play:
			return GET_RESOURCE(icons_music_play_svg);
		case SvgIconKind::Pause:
			return GET_RESOURCE(icons_music_pause_svg);
		case SvgIconKind::Prev:
			return GET_RESOURCE(icons_music_prev_svg);
		case SvgIconKind::Next:
			return GET_RESOURCE(icons_music_next_svg);
		case SvgIconKind::Shuffle:
			return GET_RESOURCE(icons_music_shuffle_svg);
		case SvgIconKind::Repeat:
			return GET_RESOURCE(icons_music_repeat_svg);
		case SvgIconKind::Refresh:
			return GET_RESOURCE(icons_music_refresh_svg);
		default:
			return GET_RESOURCE(icons_music_play_svg);
		}
	}

	bool createSvgDoc(ID2D1DeviceContext5* ctx5, Resource res, ComPtr<ID2D1SvgDocument>& outDoc) {
		if (!ctx5 || res.size() == 0) return false;
		std::string svgText(res.data(), res.size());
		auto replaceAll = [](std::string& s, std::string const& from, std::string const& to) {
			size_t pos = 0;
			while ((pos = s.find(from, pos)) != std::string::npos) {
				s.replace(pos, from.size(), to);
				pos += to.size();
			}
		};
		// Force icon color to white-ish for readability regardless of source icon color.
		replaceAll(svgText, "currentColor", "#F5F8FF");
		replaceAll(svgText, "#000000", "#F5F8FF");
		replaceAll(svgText, "#000", "#F5F8FF");
		replaceAll(svgText, "black", "#F5F8FF");
		// Hard-force all color literals to white.
		svgText = std::regex_replace(svgText, std::regex(R"(#([0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8}))"), "#F5F8FF");
		svgText = std::regex_replace(svgText, std::regex(R"(rgb\s*\([^\)]*\))", std::regex_constants::icase), "#F5F8FF");
		svgText = std::regex_replace(svgText, std::regex(R"(rgba\s*\([^\)]*\))", std::regex_constants::icase), "#F5F8FF");
		// Remove style/class driven color paths so the icon cannot fall back to dark class colors.
		svgText = std::regex_replace(svgText, std::regex(R"(<style[^>]*>[\s\S]*?</style>)", std::regex_constants::icase), "");
		svgText = std::regex_replace(svgText, std::regex(R"(\sclass\s*=\s*\"[^\"]*\")", std::regex_constants::icase), "");
		svgText = std::regex_replace(svgText, std::regex(R"(\sclass\s*=\s*'[^']*')", std::regex_constants::icase), "");
		// Keep explicit "none" values untouched and rewrite any remaining fill/stroke value.
		svgText = std::regex_replace(svgText, std::regex(R"(fill\s*:\s*(?!none)[^;}\"]+)", std::regex_constants::icase), "fill:#F5F8FF");
		svgText = std::regex_replace(svgText, std::regex(R"(stroke\s*:\s*(?!none)[^;}\"]+)", std::regex_constants::icase), "stroke:#F5F8FF");
		svgText = std::regex_replace(svgText, std::regex(R"(fill\s*=\s*\"(?!none)[^\"]+\")", std::regex_constants::icase), "fill=\"#F5F8FF\"");
		svgText = std::regex_replace(svgText, std::regex(R"(stroke\s*=\s*\"(?!none)[^\"]+\")", std::regex_constants::icase), "stroke=\"#F5F8FF\"");
		svgText = std::regex_replace(svgText, std::regex(R"(fill\s*=\s*'(?!none)[^']+')", std::regex_constants::icase), "fill='#F5F8FF'");
		svgText = std::regex_replace(svgText, std::regex(R"(stroke\s*=\s*'(?!none)[^']+')", std::regex_constants::icase), "stroke='#F5F8FF'");
		// If a geometry element has no explicit fill/stroke, force a white fill so default black cannot appear.
		svgText = std::regex_replace(svgText, std::regex(R"(<(path|rect|circle|ellipse|polygon|polyline|line)\b(?![^>]*\bfill\s*=)([^>]*)>)", std::regex_constants::icase), "<$1 fill=\"#F5F8FF\"$2>");

		size_t len = svgText.size();
		HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
		if (!hMem) return false;
		void* mem = GlobalLock(hMem);
		if (!mem) {
			GlobalFree(hMem);
			return false;
		}
		std::memcpy(mem, svgText.data(), len);
		GlobalUnlock(hMem);

		ComPtr<IStream> stream;
		if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, stream.GetAddressOf()))) {
			GlobalFree(hMem);
			return false;
		}

		ComPtr<ID2D1SvgDocument> doc;
		if (FAILED(ctx5->CreateSvgDocument(stream.Get(), D2D1::SizeF(24.f, 24.f), doc.GetAddressOf()))) return false;
		outDoc = std::move(doc);
		return true;
	}

	bool ensureSvgIcons(ID2D1DeviceContext* dc) {
		if (!dc) return false;
		if (gSvgIcons.owner != dc) {
			gSvgIcons = {};
			gSvgIcons.owner = dc;
			if (FAILED(dc->QueryInterface(IID_PPV_ARGS(gSvgIcons.ctx5.GetAddressOf())))) {
				return false;
			}
			createSvgDoc(gSvgIcons.ctx5.Get(), getSvgResource(SvgIconKind::Play), gSvgIcons.play);
			createSvgDoc(gSvgIcons.ctx5.Get(), getSvgResource(SvgIconKind::Pause), gSvgIcons.pause);
			createSvgDoc(gSvgIcons.ctx5.Get(), getSvgResource(SvgIconKind::Prev), gSvgIcons.prev);
			createSvgDoc(gSvgIcons.ctx5.Get(), getSvgResource(SvgIconKind::Next), gSvgIcons.next);
			createSvgDoc(gSvgIcons.ctx5.Get(), getSvgResource(SvgIconKind::Shuffle), gSvgIcons.shuffle);
			createSvgDoc(gSvgIcons.ctx5.Get(), getSvgResource(SvgIconKind::Repeat), gSvgIcons.repeat);
			createSvgDoc(gSvgIcons.ctx5.Get(), getSvgResource(SvgIconKind::Refresh), gSvgIcons.refresh);
		}
		return gSvgIcons.ctx5 && gSvgIcons.play && gSvgIcons.pause && gSvgIcons.prev && gSvgIcons.next && gSvgIcons.shuffle && gSvgIcons.repeat && gSvgIcons.refresh;
	}

	ID2D1SvgDocument* getSvgDoc(SvgIconKind kind) {
		switch (kind) {
		case SvgIconKind::Play: return gSvgIcons.play.Get();
		case SvgIconKind::Pause: return gSvgIcons.pause.Get();
		case SvgIconKind::Prev: return gSvgIcons.prev.Get();
		case SvgIconKind::Next: return gSvgIcons.next.Get();
		case SvgIconKind::Shuffle: return gSvgIcons.shuffle.Get();
		case SvgIconKind::Repeat: return gSvgIcons.repeat.Get();
		case SvgIconKind::Refresh: return gSvgIcons.refresh.Get();
		default: return nullptr;
		}
	}

	bool drawSvgIcon(D2DUtil& dc, RectF const& rc, SvgIconKind kind) {
		if (!ensureSvgIcons(dc.ctx)) return false;
		auto* doc = getSvgDoc(kind);
		if (!doc || !gSvgIcons.ctx5) return false;
		auto vp = doc->GetViewportSize();
		if (vp.width <= 0.f || vp.height <= 0.f) return false;

		D2D1_MATRIX_3X2_F oldTransform{};
		dc.ctx->GetTransform(&oldTransform);
		float sx = rc.getWidth() / vp.width;
		float sy = rc.getHeight() / vp.height;
		float s = std::min(sx, sy);
		float drawW = vp.width * s;
		float drawH = vp.height * s;
		float tx = rc.left + (rc.getWidth() - drawW) * 0.5f;
		float ty = rc.top + (rc.getHeight() - drawH) * 0.5f;
		auto xform = D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(tx, ty);
		dc.ctx->SetTransform(xform);
		gSvgIcons.ctx5->DrawSvgDocument(doc);
		dc.ctx->SetTransform(oldTransform);
		return true;
	}

	std::wstring formatMusicTime(int ms) {
		if (ms < 0) return L"0:00";
		int totalSeconds = ms / 1000;
		int minutes = totalSeconds / 60;
		int seconds = totalSeconds % 60;
		wchar_t buf[16]{};
		swprintf_s(buf, L"%d:%02d", minutes, seconds);
		return buf;
	}

	std::wstring getMusicFolderLabel(MusicGuiState const& state) {
		if (!state.folderPath.empty() && !state.folderPath.filename().empty()) {
			return state.folderPath.filename().wstring();
		}
		return L"Music";
	}

	std::wstring getCurrentTrackLabel(MusicGuiState const& state, std::wstring const& fallback = L"Nothing queued") {
		if (state.currentTrack >= 0 && state.currentTrack < static_cast<int>(state.tracks.size())) {
			return getTrackDisplayTitle(state.tracks[state.currentTrack]);
		}
		return fallback;
	}

	std::wstring getPlaybackStateLabel(MusicGuiState const& state) {
		if (state.currentTrack < 0) return L"Stopped";
		return state.paused ? L"Paused" : L"Playing";
	}

	d2d::Color getPlaybackStateFill(MusicGuiState const& state, d2d::Color const& accentColor, float accentAlpha) {
		if (state.currentTrack < 0) return d2d::Color::RGB(0x2B, 0x37, 0x46).asAlpha(kMusicAlphaStatePill);
		if (state.paused) return d2d::Color::RGB(0x5D, 0x49, 0x25).asAlpha(kMusicAlphaStatePill);
		return accentColor.asAlpha(accentAlpha);
	}

	void drawGlassPanel(D2DUtil& dc, RectF const& rc, float radius, float blurIntensity, d2d::Color fill, d2d::Color outline, std::optional<d2d::Color> glow = std::nullopt) {
		(void)blurIntensity;
		dc.fillRoundedRectangle(rc, fill, radius);
		if (outline.a > 0.001f) {
			dc.drawRoundedRectangle(rc, outline, radius, 1.2f, DrawUtil::OutlinePosition::Inside);
		}
		if (glow.has_value()) {
			RectF glowRect = { rc.left + 12.f, rc.top + 10.f, rc.right - 12.f, rc.top + 15.f };
			dc.fillRoundedRectangle(glowRect, glow.value(), glowRect.getHeight() * 0.5f);
		}
	}

	bool drawRoundedBitmap(D2DUtil& dc, RectF const& rc, ID2D1Bitmap* bitmap, float radius, float opacity = 1.f) {
		if (!bitmap) return false;

		ComPtr<ID2D1RoundedRectangleGeometry> mask;
		auto rr = D2D1::RoundedRect(rc.get(), radius, radius);
		if (FAILED(Omoti::getRenderer().getFactory()->CreateRoundedRectangleGeometry(rr, mask.GetAddressOf())) || !mask) {
			return false;
		}

		D2D1_SIZE_F bmpSize = bitmap->GetSize();
		if (bmpSize.width <= 0.f || bmpSize.height <= 0.f) return false;

		float destAspect = rc.getWidth() / std::max(1.f, rc.getHeight());
		float srcAspect = bmpSize.width / std::max(1.f, bmpSize.height);
		RectF srcRect = { 0.f, 0.f, bmpSize.width, bmpSize.height };
		if (srcAspect > destAspect) {
			float cropWidth = bmpSize.height * destAspect;
			float offsetX = (bmpSize.width - cropWidth) * 0.5f;
			srcRect = { offsetX, 0.f, offsetX + cropWidth, bmpSize.height };
		}
		else if (srcAspect < destAspect) {
			float cropHeight = bmpSize.width / destAspect;
			float offsetY = (bmpSize.height - cropHeight) * 0.5f;
			srcRect = { 0.f, offsetY, bmpSize.width, offsetY + cropHeight };
		}

		D2D1_LAYER_PARAMETERS layerParams = D2D1::LayerParameters();
		layerParams.contentBounds = rc.get();
		layerParams.geometricMask = mask.Get();
		layerParams.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
		layerParams.opacity = 1.0f;
		layerParams.layerOptions = D2D1_LAYER_OPTIONS_NONE;
		dc.ctx->PushLayer(layerParams, nullptr);
		dc.ctx->DrawBitmap(bitmap, rc.get(), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, srcRect.get());
		dc.ctx->PopLayer();
		return true;
	}

	void drawArtworkFallback(D2DUtil& dc, RectF const& rc, float radius, float iconPaddingScale = 0.18f, bool darkFill = true) {
		if (darkFill) {
			dc.fillRoundedRectangle(rc, d2d::Color::RGB(0xFF, 0xFF, 0xFF).asAlpha(kMusicAlphaFallbackArt), radius);
		}

		RectF iconRect = rc;
		float padX = rc.getWidth() * iconPaddingScale;
		float padY = rc.getHeight() * iconPaddingScale;
		iconRect.left += padX;
		iconRect.right -= padX;
		iconRect.top += padY;
		iconRect.bottom -= padY;

		if (auto bmp = Omoti::getAssets().logoWhite.getBitmap()) {
			dc.ctx->DrawBitmap(bmp, iconRect.get(), 0.92f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
		}
		else if (auto bmp = Omoti::getAssets().OmotiLogo.getBitmap()) {
			dc.ctx->DrawBitmap(bmp, iconRect.get(), 0.92f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
		}
		else {
			dc.drawText(rc, L"\x266B", d2d::Color(1.f, 1.f, 1.f, 0.92f), FontSelection::PrimarySemilight, rc.getHeight() * 0.48f, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
		}
	}

	void drawPill(D2DUtil& dc, RectF const& rc, std::wstring const& text, d2d::Color fill, d2d::Color textColor, float fontSize) {
		dc.fillRoundedRectangle(rc, fill, rc.getHeight() * 0.5f);
		dc.drawText(rc, text, textColor, FontSelection::PrimaryRegular, fontSize, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
	}

	void drawMarqueeText(D2DUtil& dc, RectF const& rc, std::wstring const& text, d2d::Color const& color, Renderer::FontSelection font, float fontSize, DWRITE_PARAGRAPH_ALIGNMENT verticalAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
		if (text.empty()) return;

		Vec2 textSize = dc.getTextSize(text, font, fontSize, true, false);
		if (textSize.x <= rc.getWidth() - 4.f) {
			dc.drawText(rc, text, color, font, fontSize, DWRITE_TEXT_ALIGNMENT_LEADING, verticalAlign, false);
			return;
		}

		float gap = std::max(26.f, fontSize * 1.8f);
		float cycleWidth = textSize.x + gap;
		float speed = std::max(24.f, fontSize * 2.6f);
		float offset = std::fmod((GetTickCount64() / 1000.f) * speed, cycleWidth);

		dc.ctx->PushAxisAlignedClip(rc.get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		for (int i = 0; i < 3; i++) {
			float drawLeft = rc.left - offset + cycleWidth * static_cast<float>(i);
			RectF drawRect = { drawLeft, rc.top, drawLeft + textSize.x + 8.f, rc.bottom };
			dc.drawText(drawRect, text, color, font, fontSize, DWRITE_TEXT_ALIGNMENT_LEADING, verticalAlign, false);
		}
		dc.ctx->PopAxisAlignedClip();
	}

	int getMusicLengthMs();
	int getMusicPositionMs();

	float syncPlaybackTimeline(MusicGuiState& state, ULONGLONG nowTick, bool modePlaying, bool modePaused, bool inUiStabilize) {
		int queriedLengthMs = getMusicLengthMs();
		int queriedPosMs = getMusicPositionMs();
		if (queriedLengthMs > 0) {
			state.uiLengthMs = queriedLengthMs;
		}
		if (queriedPosMs >= 0) {
			bool backwardsJump = queriedPosMs + 400 < state.uiPosMs;
			if (!(inUiStabilize && backwardsJump)) {
				state.uiPosMs = queriedPosMs;
			}
		}
		if (state.uiLastTick == 0) state.uiLastTick = nowTick;
		if (modePlaying && !modePaused && queriedPosMs < 0) {
			int deltaMs = static_cast<int>(nowTick - state.uiLastTick);
			if (deltaMs > 0 && deltaMs < 500) {
				state.uiPosMs += deltaMs;
			}
		}
		state.uiLastTick = nowTick;
		if (state.uiLengthMs > 0) {
			state.uiPosMs = std::clamp(state.uiPosMs, 0, state.uiLengthMs);
			return std::clamp(static_cast<float>(state.uiPosMs) / static_cast<float>(state.uiLengthMs), 0.f, 1.f);
		}
		return 0.f;
	}

	MusicPlaybackSnapshot updateMusicPlaybackSnapshot(MusicGuiState& state) {
		MusicPlaybackSnapshot snapshot;
		snapshot.nowTick = GetTickCount64();
		snapshot.inUiStabilize = snapshot.nowTick < state.uiStabilizeUntilTick;

		std::wstring modeNow = getMusicMode();
		snapshot.modePaused = modeNow == L"paused";
		snapshot.modePlaying = modeNow == L"playing";
		snapshot.modeStopped = modeNow == L"stopped";

		if (snapshot.inUiStabilize && state.currentTrack >= 0) {
			snapshot.modePaused = state.paused;
			snapshot.modePlaying = !state.paused;
			snapshot.modeStopped = false;
		}

		if (state.currentTrack >= 0) {
			if (snapshot.modePaused) {
				state.modePausedStreak++;
				state.modePlayingStreak = 0;
				if (state.modePausedStreak >= 3) state.paused = true;
			}
			else if (snapshot.modePlaying) {
				state.modePlayingStreak++;
				state.modePausedStreak = 0;
				if (state.modePlayingStreak >= 3) state.paused = false;
			}
			else {
				state.modePausedStreak = 0;
				state.modePlayingStreak = 0;
			}
		}

		if (!snapshot.inUiStabilize && state.currentTrack >= 0 && !state.paused && snapshot.modeStopped) {
			bool inStartupGrace = (state.playStartTick != 0) && ((snapshot.nowTick - state.playStartTick) < 1500);
			if (inStartupGrace) {
				state.stoppedPollStreak = 0;
			}
			else {
				state.stoppedPollStreak++;
				if (state.stoppedPollStreak >= 10) {
					if (playAdjacentTrack(state, 1)) {
						snapshot.modeStopped = false;
						snapshot.modePlaying = true;
						snapshot.modePaused = false;
					}
					else {
						state.currentTrack = -1;
						state.paused = false;
					}
					state.stoppedPollStreak = 0;
				}
			}
		}
		else {
			state.stoppedPollStreak = 0;
		}

		return snapshot;
	}

	bool tryUseMusicActionCooldown(ULONGLONG& nextTick, ULONGLONG cooldownMs) {
		auto now = GetTickCount64();
		if (now < nextTick) return false;
		nextTick = now + cooldownMs;
		return true;
	}

	bool isSupportedMusicFile(std::filesystem::path const& path) {
		if (!path.has_extension()) return false;
		auto ext = path.extension().wstring();
		std::ranges::transform(ext, ext.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
		return ext == L".mp3" || ext == L".wav" || ext == L".ogg" || ext == L".flac" || ext == L".m4a";
	}

	void setMusicBackendVolume(float volume) {
		MusicHelperClient::get().setVolume(std::clamp(volume, 0.f, 1.f));
	}

	void closeMusicAlias() {
		MusicHelperClient::get().stop();
	}

	std::filesystem::path getMusicRootPath();
	std::filesystem::path getMusicFolderPath();

	std::filesystem::path getMusicRootPath() {
		wchar_t localAppData[MAX_PATH]{};
		DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
		if (len > 0 && len < MAX_PATH) {
			auto path = std::filesystem::path(localAppData)
				/ "Packages"
				/ "Microsoft.MinecraftUWP_8wekyb3d8bbwe"
				/ "RoamingState"
				/ "Omoti";
			std::error_code ec;
			std::filesystem::create_directories(path, ec);
			return path;
		}
		auto fallback = util::GetOmotiPath();
		std::error_code ec;
		std::filesystem::create_directories(fallback, ec);
		return fallback;
	}

	std::filesystem::path getMusicFolderPath() {
		auto primary = getMusicRootPath() / "Music";
		auto fallback = util::GetOmotiPath() / "Music";
		std::error_code ec;
		std::filesystem::create_directories(primary, ec);
		std::filesystem::create_directories(fallback, ec);
		if (std::filesystem::exists(primary, ec) && !std::filesystem::is_empty(primary, ec)) return primary;
		if (std::filesystem::exists(fallback, ec) && !std::filesystem::is_empty(fallback, ec)) return fallback;
		return primary;
	}

	std::filesystem::path getMusicMetaPath() {
		auto root = getMusicRootPath();
		auto metaPath = root / "MusicPlaylists.json";

		// One-time migration from legacy Local\Omoti path.
		std::error_code ec;
		auto legacy = util::GetOmotiPath() / "MusicPlaylists.json";
		if (!std::filesystem::exists(metaPath, ec) && std::filesystem::exists(legacy, ec)) {
			std::filesystem::copy_file(legacy, metaPath, std::filesystem::copy_options::overwrite_existing, ec);
		}
		return metaPath;
	}

	void ensureMusicStateInitialized(MusicGuiState& state) {
		if (state.initialized) return;
		state.initialized = true;
		state.folderPath = getMusicFolderPath();
		std::error_code ec;
		std::filesystem::create_directories(state.folderPath, ec);
		loadMusicMeta(state);
		if (!state.playlists.contains(state.activePlaylist)) {
			state.playlists[state.activePlaylist] = {};
		}
	}

	std::wstring getTrackKey(MusicGuiState const& state, std::filesystem::path const& path) {
		std::error_code ec;
		auto rel = std::filesystem::relative(path, state.folderPath, ec);
		if (ec || rel.empty()) return path.filename().wstring();
		return rel.wstring();
	}

	std::wstring getTrackKey(MusicGuiState const& state, MusicTrack const& track) {
		return getTrackKey(state, track.path);
	}

	Setting* findGlobalSetting(std::string_view settingName) {
		Setting* found = nullptr;
		Omoti::getSettings().forEach([&](std::shared_ptr<Setting> const& set) {
			if (!found && set->name() == settingName) {
				found = set.get();
			}
		});
		return found;
	}

	bool applyGlobalKeySetting(std::string_view settingName, int key) {
		auto* setting = findGlobalSetting(settingName);
		if (!setting || !setting->value) return false;
		*setting->value = KeyValue(key);
		setting->resolvedValue = KeyValue(key);
		setting->update();
		setting->userUpdate();
		return true;
	}

	void saveMusicMeta(MusicGuiState const& state) {
		try {
			json j;
			j["favorites"] = json::array();
			for (auto const& fav : state.favorites) {
				j["favorites"].push_back(util::WStrToStr(fav));
			}

			json pls = json::object();
			for (auto const& [name, list] : state.playlists) {
				json arr = json::array();
				for (auto const& item : list) arr.push_back(util::WStrToStr(item));
				pls[util::WStrToStr(name)] = arr;
			}
			j["playlists"] = pls;
			j["activePlaylist"] = util::WStrToStr(state.activePlaylist);
			j["miniHudNormX"] = state.miniHudNormX;
			j["miniHudNormY"] = state.miniHudNormY;

			auto outPath = getMusicMetaPath();
			std::ofstream ofs(outPath, std::ios::trunc);
			if (ofs.is_open()) {
				ofs << j.dump(2);
			}
		}
		catch (...) {
		}
	}

	void loadMusicMeta(MusicGuiState& state) {
		if (state.metaLoaded) return;
		state.metaLoaded = true;

		try {
			auto path = getMusicMetaPath();
			if (!std::filesystem::exists(path)) return;
			std::ifstream ifs(path);
			if (!ifs.is_open()) return;
			json j;
			ifs >> j;

			if (j.contains("favorites") && j["favorites"].is_array()) {
				for (auto const& v : j["favorites"]) {
					if (v.is_string()) state.favorites.insert(util::StrToWStr(v.get<std::string>()));
				}
			}

			if (j.contains("playlists") && j["playlists"].is_object()) {
				for (auto it = j["playlists"].begin(); it != j["playlists"].end(); ++it) {
					std::wstring name = util::StrToWStr(it.key());
					if (!it.value().is_array()) continue;
					auto& arr = state.playlists[name];
					for (auto const& item : it.value()) {
						if (item.is_string()) arr.push_back(util::StrToWStr(item.get<std::string>()));
					}
				}
			}

			if (j.contains("activePlaylist") && j["activePlaylist"].is_string()) {
				state.activePlaylist = util::StrToWStr(j["activePlaylist"].get<std::string>());
			}

			if (j.contains("miniHudNormX") && j["miniHudNormX"].is_number()) {
				state.miniHudNormX = j["miniHudNormX"].get<float>();
			}
			if (j.contains("miniHudNormY") && j["miniHudNormY"].is_number()) {
				state.miniHudNormY = j["miniHudNormY"].get<float>();
			}
		}
		catch (...) {
		}
	}

	void refreshMusicTracks(MusicGuiState& state) {
		try {
			state.tracks.clear();
			state.selectedTrack = -1;
			state.currentTrack = -1;
			state.paused = false;
			state.uiLengthMs = -1;
			state.uiPosMs = 0;
			state.uiLastTick = 0;
			state.uiStabilizeUntilTick = 0;
			state.modePausedStreak = 0;
			state.modePlayingStreak = 0;
			closeMusicAlias();

			std::error_code ec;
			if (state.folderPath.empty()) return;
			if (!std::filesystem::exists(state.folderPath, ec)) return;
			if (!std::filesystem::is_directory(state.folderPath, ec)) return;

			for (auto it = std::filesystem::recursive_directory_iterator(state.folderPath, ec); it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
				if (ec) break;
				auto const& entry = *it;
				if (entry.is_directory(ec)) {
					if (auto track = tryLoadSongFolder(entry.path())) {
						state.tracks.emplace_back(std::move(*track));
						it.disable_recursion_pending();
					}
				}
				else if (it.depth() == 0 && entry.is_regular_file(ec) && isSupportedMusicFile(entry.path())) {
					if (auto track = migrateLooseTrackIntoFolder(entry.path())) {
						state.tracks.emplace_back(std::move(*track));
					}
				}
				if (state.tracks.size() >= 1000) break;
			}

			std::ranges::sort(state.tracks, [](auto const& a, auto const& b) {
				return getTrackDisplayTitle(a) < getTrackDisplayTitle(b);
			});

			std::unordered_set<std::wstring> validTrackKeys;
			for (auto const& t : state.tracks) {
				validTrackKeys.insert(getTrackKey(state, t));
			}

			for (auto it = state.favorites.begin(); it != state.favorites.end();) {
				if (!validTrackKeys.contains(*it)) it = state.favorites.erase(it);
				else ++it;
			}

			for (auto& [_, list] : state.playlists) {
				list.erase(std::remove_if(list.begin(), list.end(), [&](std::wstring const& key) {
					return !validTrackKeys.contains(key);
				}), list.end());
			}
		}
		catch (...) {
			state.tracks.clear();
			state.selectedTrack = -1;
			state.currentTrack = -1;
			state.paused = false;
			state.uiLengthMs = -1;
			state.uiPosMs = 0;
			state.uiLastTick = 0;
			state.uiStabilizeUntilTick = 0;
			state.modePausedStreak = 0;
			state.modePlayingStreak = 0;
			closeMusicAlias();
		}
	}

	bool playMusicTrack(MusicGuiState& state, int index) {
		try {
			if (index < 0 || index >= static_cast<int>(state.tracks.size())) return false;
			closeMusicAlias();

			auto const& filePath = state.tracks[index].path;
			if (!MusicHelperClient::get().play(filePath, state.volume)) {
				auto err = MusicHelperClient::get().getLastError();
				if (!err.empty()) {
					Logger::Warn("Music helper play failed: {}", util::WStrToStr(err));
				}
				else {
					Logger::Warn("Music helper play failed: {}", util::WStrToStr(filePath.wstring()));
				}
				closeMusicAlias();
				return false;
			}
			state.currentTrack = index;
			state.selectedTrack = index;
			state.paused = false;
			state.playStartTick = GetTickCount64();
			state.stoppedPollStreak = 0;
			state.uiStabilizeUntilTick = state.playStartTick + 1200;
			state.uiLastTick = state.playStartTick;
			state.uiPosMs = 0;
			state.uiLengthMs = -1;
			state.modePausedStreak = 0;
			state.modePlayingStreak = 0;
			return true;
		}
		catch (...) {
			Logger::Warn("Music track play exception");
			closeMusicAlias();
			state.currentTrack = -1;
			state.paused = false;
			return false;
		}
	}

	bool isMusicAliasStopped() {
		return MusicHelperClient::get().getStatus().state == L"stopped";
	}

	std::wstring getMusicMode() {
		return MusicHelperClient::get().getStatus().state;
	}

	int getMusicLengthMs() {
		return MusicHelperClient::get().getStatus().durationMs;
	}

	int getMusicPositionMs() {
		return MusicHelperClient::get().getStatus().positionMs;
	}

	bool seekMusicMs(MusicGuiState& state, int targetMs) {
		if (state.currentTrack < 0) return false;
		targetMs = std::max(0, targetMs);
		if (!MusicHelperClient::get().seek(targetMs)) return false;
		state.playStartTick = GetTickCount64();
		state.stoppedPollStreak = 0;
		state.uiStabilizeUntilTick = state.playStartTick + 700;
		state.uiLastTick = state.playStartTick;
		state.uiPosMs = targetMs;
		return true;
	}

	bool pauseMusicPlayback() {
		return MusicHelperClient::get().pause();
	}

	bool resumeMusicPlayback() {
		return MusicHelperClient::get().resume();
	}

	bool playAdjacentTrack(MusicGuiState& state, int direction) {
		if (state.tracks.empty()) return false;
		if (state.currentTrack < 0 || state.currentTrack >= static_cast<int>(state.tracks.size())) {
			return playMusicTrack(state, 0);
		}

		int nextIndex = state.currentTrack;
		if (state.shuffle && state.tracks.size() > 1) {
			nextIndex = std::rand() % static_cast<int>(state.tracks.size());
			if (nextIndex == state.currentTrack) {
				nextIndex = (nextIndex + 1) % static_cast<int>(state.tracks.size());
			}
		}
		else {
			nextIndex += direction;
			if (nextIndex < 0) {
				if (!state.repeat) return false;
				nextIndex = static_cast<int>(state.tracks.size()) - 1;
			}
			if (nextIndex >= static_cast<int>(state.tracks.size())) {
				if (!state.repeat) return false;
				nextIndex = 0;
			}
		}

		return playMusicTrack(state, nextIndex);
	}

	bool isTrackInPlaylist(std::vector<std::wstring> const& list, std::wstring const& key) {
		return std::find(list.begin(), list.end(), key) != list.end();
	}

	std::wstring trimCopy(std::wstring value) {
		value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](wchar_t c) { return !iswspace(c); }));
		value.erase(std::find_if(value.rbegin(), value.rend(), [](wchar_t c) { return !iswspace(c); }).base(), value.end());
		return value;
	}

	std::wstring readPropertyString(IPropertyStore* store, PROPERTYKEY const& key) {
		PROPVARIANT value;
		PropVariantInit(&value);
		std::wstring result;
		if (SUCCEEDED(store->GetValue(key, &value))) {
			PWSTR out = nullptr;
			if (SUCCEEDED(PropVariantToStringAlloc(value, &out)) && out) {
				result = out;
				CoTaskMemFree(out);
			}
		}
		PropVariantClear(&value);
		return trimCopy(result);
	}

	std::filesystem::path getSongJsonPath(std::filesystem::path const& folderPath) {
		return folderPath / "song.json";
	}

	std::filesystem::path findPreferredJacket(std::filesystem::path const& folderPath) {
		static std::array<std::wstring, 8> const candidates = {
			L"jacket.png", L"jacket.jpg", L"jacket.jpeg", L"jacket.webp",
			L"cover.png", L"cover.jpg", L"cover.jpeg", L"cover.webp"
		};
		std::error_code ec;
		for (auto const& name : candidates) {
			auto path = folderPath / name;
			if (std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec)) return path;
		}
		return {};
	}

	MusicTrack makeMusicTrack(std::filesystem::path const& path) {
		MusicTrack track;
		track.folderPath = path.parent_path();
		track.path = path;
		track.title = path.stem().wstring();
		track.artist = L"Unknown artist";

		ComPtr<IPropertyStore> store;
		if (SUCCEEDED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(store.GetAddressOf())))) {
			auto title = readPropertyString(store.Get(), PKEY_Title);
			if (!title.empty()) track.title = title;

			auto artist = readPropertyString(store.Get(), PKEY_Music_Artist);
			if (artist.empty()) artist = readPropertyString(store.Get(), PKEY_Music_AlbumArtist);
			if (!artist.empty()) track.artist = artist;
		}

		return track;
	}

	void writeSongJson(MusicTrack const& track) {
		if (track.folderPath.empty() || track.path.empty()) return;
		try {
			json j;
			j["title"] = util::WStrToStr(track.title.empty() ? track.path.stem().wstring() : track.title);
			j["artist"] = util::WStrToStr(track.artist);
			j["audio"] = util::WStrToStr(track.path.filename().wstring());
			if (!track.jacketPath.empty()) j["jacket"] = util::WStrToStr(track.jacketPath.filename().wstring());
			j["version"] = 1;
			std::ofstream ofs(getSongJsonPath(track.folderPath), std::ios::trunc);
			if (ofs.is_open()) ofs << j.dump(2);
		}
		catch (...) {
		}
	}

	std::optional<MusicTrack> tryLoadSongFolder(std::filesystem::path const& folderPath) {
		try {
			auto songJsonPath = getSongJsonPath(folderPath);
			if (!std::filesystem::exists(songJsonPath)) return std::nullopt;

			std::ifstream ifs(songJsonPath);
			if (!ifs.is_open()) return std::nullopt;
			json j;
			ifs >> j;

			std::filesystem::path audioPath;
			if (j.contains("audio") && j["audio"].is_string()) {
				audioPath = folderPath / util::StrToWStr(j["audio"].get<std::string>());
			}
			if (audioPath.empty() || !std::filesystem::exists(audioPath) || !isSupportedMusicFile(audioPath)) {
				for (auto const& entry : std::filesystem::directory_iterator(folderPath)) {
					if (entry.is_regular_file() && isSupportedMusicFile(entry.path())) {
						audioPath = entry.path();
						break;
					}
				}
			}
			if (audioPath.empty() || !std::filesystem::exists(audioPath)) return std::nullopt;

			auto track = makeMusicTrack(audioPath);
			track.folderPath = folderPath;
			if (j.contains("title") && j["title"].is_string()) track.title = util::StrToWStr(j["title"].get<std::string>());
			if (j.contains("artist") && j["artist"].is_string()) track.artist = util::StrToWStr(j["artist"].get<std::string>());
			if (j.contains("jacket") && j["jacket"].is_string()) {
				auto explicitJacket = folderPath / util::StrToWStr(j["jacket"].get<std::string>());
				if (std::filesystem::exists(explicitJacket)) track.jacketPath = explicitJacket;
			}
			if (track.jacketPath.empty()) track.jacketPath = findPreferredJacket(folderPath);
			return track;
		}
		catch (...) {
			return std::nullopt;
		}
	}

	std::optional<MusicTrack> migrateLooseTrackIntoFolder(std::filesystem::path const& audioPath) {
		try {
			if (!std::filesystem::exists(audioPath) || !isSupportedMusicFile(audioPath)) return std::nullopt;

			auto parent = audioPath.parent_path();
			auto baseName = trimCopy(audioPath.stem().wstring());
			if (baseName.empty()) baseName = L"Track";

			std::filesystem::path folderPath = parent / baseName;
			int suffix = 2;
			std::error_code ec;
			while (std::filesystem::exists(folderPath, ec)) {
				if (!std::filesystem::exists(folderPath / audioPath.filename(), ec) && !std::filesystem::exists(getSongJsonPath(folderPath), ec)) break;
				folderPath = parent / (baseName + L" (" + std::to_wstring(suffix++) + L")");
			}

			std::filesystem::create_directories(folderPath, ec);
			if (ec) return std::nullopt;

			auto movedAudioPath = folderPath / audioPath.filename();
			if (!std::filesystem::equivalent(audioPath, movedAudioPath, ec)) {
				std::filesystem::rename(audioPath, movedAudioPath, ec);
				if (ec) {
					ec.clear();
					std::filesystem::copy_file(audioPath, movedAudioPath, std::filesystem::copy_options::overwrite_existing, ec);
					if (ec) return std::nullopt;
					std::filesystem::remove(audioPath, ec);
				}
			}

			auto track = makeMusicTrack(movedAudioPath);
			track.folderPath = folderPath;
			track.jacketPath = findPreferredJacket(folderPath);
			writeSongJson(track);
			return track;
		}
		catch (...) {
			return std::nullopt;
		}
	}

	std::wstring getTrackDisplayTitle(MusicTrack const& track) {
		return track.title.empty() ? track.path.stem().wstring() : track.title;
	}

	std::wstring getTrackDisplayArtist(MusicTrack const& track) {
		return track.artist.empty() ? L"Unknown artist" : track.artist;
	}

	bool ensureTrackArtwork(MusicTrack& track, ID2D1DeviceContext* ctx) {
		if (track.artworkLoaded) return track.artwork != nullptr;
		track.artworkLoaded = true;
		if (!ctx) return false;

		if (!track.jacketPath.empty() && std::filesystem::exists(track.jacketPath)) {
			auto* imagingFactory = Omoti::getRenderer().getImagingFactory();
			if (imagingFactory) {
				ComPtr<IWICBitmapDecoder> decoder;
				if (SUCCEEDED(imagingFactory->CreateDecoderFromFilename(track.jacketPath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())) && decoder) {
					ComPtr<IWICBitmapFrameDecode> frame;
					if (SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf())) && frame) {
						ComPtr<IWICFormatConverter> conv;
						if (SUCCEEDED(imagingFactory->CreateFormatConverter(conv.GetAddressOf())) &&
							SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
							ComPtr<ID2D1Bitmap> bitmap;
							if (SUCCEEDED(ctx->CreateBitmapFromWicBitmap(conv.Get(), nullptr, bitmap.GetAddressOf()))) {
								track.artwork = std::move(bitmap);
								return true;
							}
						}
					}
				}
			}
		}

		ComPtr<IShellItemImageFactory> imageFactory;
		if (FAILED(SHCreateItemFromParsingName(track.path.c_str(), nullptr, IID_PPV_ARGS(imageFactory.GetAddressOf())))) {
			return false;
		}

		HBITMAP hBitmap = nullptr;
		SIZE thumbSize = { 512, 512 };
		if (FAILED(imageFactory->GetImage(thumbSize, SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY, &hBitmap)) || !hBitmap) {
			return false;
		}

		auto* imagingFactory = Omoti::getRenderer().getImagingFactory();
		if (!imagingFactory) {
			DeleteObject(hBitmap);
			return false;
		}

		ComPtr<IWICBitmap> wicBitmap;
		HRESULT hr = imagingFactory->CreateBitmapFromHBITMAP(hBitmap, nullptr, WICBitmapUseAlpha, wicBitmap.GetAddressOf());
		DeleteObject(hBitmap);
		if (FAILED(hr)) return false;

		ComPtr<ID2D1Bitmap> bitmap;
		if (FAILED(ctx->CreateBitmapFromWicBitmap(wicBitmap.Get(), nullptr, bitmap.GetAddressOf()))) return false;
		track.artwork = std::move(bitmap);
		return track.artwork != nullptr;
	}
}

ClickGUI::ClickGUI() {
	this->key = Omoti::get().getMenuKey();
	Omoti::get().addTextBox(&this->searchTextBox);
	Omoti::get().addTextBox(&this->playlistNameTextBox);
	this->playlistNameTextBox.setText(L"");

	Eventing::get().listen<RenderOverlayEvent>(this, (EventListenerFunc)&ClickGUI::onRender, 1, true);
	Eventing::get().listen<RendererCleanupEvent>(this, (EventListenerFunc)&ClickGUI::onCleanup, 1, true);
	Eventing::get().listen<RendererInitEvent>(this, (EventListenerFunc)&ClickGUI::onInit, 1, true);
	Eventing::get().listen<KeyUpdateEvent>(this, (EventListenerFunc)&ClickGUI::onKey, 150);
	Eventing::get().listen<ClickEvent>(this, (EventListenerFunc)&ClickGUI::onClick, 150);
}

void ClickGUI::onRender(Event&) {
#if defined(_MSC_VER)
	__try {
		onRenderImpl();
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		Logger::Fatal("ClickGUI::onRender crashed with SEH exception code: {:#x}", static_cast<unsigned int>(GetExceptionCode()));
	}
#else
	onRenderImpl();
#endif
}

void ClickGUI::onRenderImpl() {
#if !defined(OMOTI_MUSIC_GUI_SAFE_ONLY)
#define OMOTI_MUSIC_GUI_SAFE_ONLY 1
#endif
#if !OMOTI_MUSIC_GUI_SAFE_ONLY
	static std::vector<ModuleLike> mods = {};
	static size_t lastCount = 0;
	static size_t marketScriptCount = 0;
#endif

#if !OMOTI_MUSIC_GUI_SAFE_ONLY
	// Music-only mode: disable legacy module/market population to avoid unnecessary work/crashes.
	if (shouldRebuildModLikes) {
		shouldRebuildModLikes = false;
	}
	mods.clear();
	lastCount = 0;
	marketScriptCount = 0;
#endif

	{
		auto scn = Omoti::getScreenManager().getActiveScreen();
		bool hasMiniPlayback = gMusicState.currentTrack >= 0 && gMusicState.currentTrack < static_cast<int>(gMusicState.tracks.size());
		if (!isActive() && !hasMiniPlayback && (calcAnim < 0.03f)) {
			calcAnim = 0.f;
			return;
		}
		if (scn) {
			auto scnName = scn->get().getName();
			if (scnName != this->getName() && !hasMiniPlayback) {
				calcAnim = 0.f;
				return;
			}
		}
	}

	if (isActive()) {
		ensureMusicStateInitialized(gMusicState);
		if (gMusicState.needsRefresh) {
			refreshMusicTracks(gMusicState);
			gMusicState.needsRefresh = false;
		}
	}

	bool shouldArrow = true;
	clearLayers();

	if (colorPicker.setting) {
		addLayer(cPickerRect);
	}

	D2DUtil dc;
	if (!isActive()) justClicked = { false, false, false };
	if (isActive()) SDK::ClientInstance::get()->releaseCursor();
	dc.ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

	Vec2& cursorPos = SDK::ClientInstance::get()->cursorPos;
	auto accentColor = d2d::Color(Omoti::get().getAccentColor().getMainColor());

	//auto& ev = reinterpret_cast<RenderOverlayEvent&>(evGeneric);
	auto& rend = Omoti::getRenderer();
	auto ss = rend.getScreenSize();

	adaptedScale = ss.width / 1920.f;

	float guiX = ss.width / 4.f;
	float guiY = ss.height / 4.f;
	{
		float totalWidth = ss.height * (16.f / 9.f);;

		float realGuiX = totalWidth / 2.f;

		guiX = (ss.width / 2.25f) - (realGuiX / 2.f);
		guiY = (ss.height / 5.f);
	}

	rect = { guiX, guiY, ss.width - guiX, ss.height - guiY };
	float guiWidth = rect.getWidth();
	(void)guiWidth;

	constexpr bool kSafeMusicGuiRender = kMusicGuiSafeOnly;
	if (kSafeMusicGuiRender) {
		modClip = std::nullopt;

		auto drawMiniPlaybackHud = [&](bool interactive) -> bool {
			if (gMusicState.currentTrack < 0 || gMusicState.currentTrack >= static_cast<int>(gMusicState.tracks.size())) return false;

			float hudScale = std::clamp(adaptedScale, 0.8f, 1.35f);
			float panelW = 646.f * hudScale;
			float panelH = 156.f * hudScale;
			float margin = 24.f * hudScale;
			float availW = std::max(1.f, ss.width - panelW);
			float availH = std::max(1.f, ss.height - panelH);
			if (gMusicState.miniHudNormX < 0.f || gMusicState.miniHudNormY < 0.f) {
				float defaultX = 380.f * hudScale;
				float defaultY = 92.f * hudScale;
				gMusicState.miniHudNormX = std::clamp(defaultX / availW, 0.f, 1.f);
				gMusicState.miniHudNormY = std::clamp(defaultY / availH, 0.f, 1.f);
			}

			float panelX = std::clamp(gMusicState.miniHudNormX, 0.f, 1.f) * availW;
			float panelY = std::clamp(gMusicState.miniHudNormY, 0.f, 1.f) * availH;
			RectF miniRect = { panelX, panelY, panelX + panelW, panelY + panelH };
			miniRect.round();

			const float miniRound = 20.f * hudScale;
			drawGlassPanel(
				dc,
				miniRect,
				miniRound,
				24.f * hudScale,
				d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaMiniHud),
				d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaClear)
			);

			if (interactive) {
				addLayer(miniRect);
				Vec2& pointerPos = SDK::ClientInstance::get()->cursorPos;
				bool lmbDown = mouseButtons[0];
				if (lmbDown && !gMusicState.miniHudPrevLmbDown && miniRect.contains(pointerPos)) {
					gMusicState.miniHudDragging = true;
					gMusicState.miniHudDragOffset = pointerPos - miniRect.getPos();
				}
				if (!lmbDown) gMusicState.miniHudDragging = false;
				if (gMusicState.miniHudDragging) {
					RectF moved = miniRect;
					moved.setPos(pointerPos - gMusicState.miniHudDragOffset);
					util::KeepInBounds(moved, { 0.f, 0.f, ss.width, ss.height });
					miniRect = moved;
					gMusicState.miniHudNormX = std::clamp(miniRect.left / availW, 0.f, 1.f);
					gMusicState.miniHudNormY = std::clamp(miniRect.top / availH, 0.f, 1.f);
				}
				gMusicState.miniHudPrevLmbDown = lmbDown;
			} else {
				gMusicState.miniHudDragging = false;
				gMusicState.miniHudPrevLmbDown = false;
			}

			auto playbackSnapshot = updateMusicPlaybackSnapshot(gMusicState);
			syncPlaybackTimeline(
				gMusicState,
				playbackSnapshot.nowTick,
				playbackSnapshot.modePlaying,
				playbackSnapshot.modePaused,
				playbackSnapshot.inUiStabilize
			);

			std::wstring stateText = getPlaybackStateLabel(gMusicState);
			std::wstring trackName = getCurrentTrackLabel(gMusicState);
			std::wstring artistName = getTrackDisplayArtist(gMusicState.tracks[gMusicState.currentTrack]);
			std::wstring timeText = formatMusicTime(std::max(0, gMusicState.uiPosMs));

			RectF artworkFrame = { miniRect.left + 18.f * hudScale, miniRect.top + 18.f * hudScale, miniRect.left + 118.f * hudScale, miniRect.top + 118.f * hudScale };
			dc.fillRoundedRectangle(artworkFrame, d2d::Color::RGB(0xFF, 0xFF, 0xFF).asAlpha(kMusicAlphaArtworkFrame), 18.f * hudScale);
			RectF artworkRect = { artworkFrame.left + 2.f * hudScale, artworkFrame.top + 2.f * hudScale, artworkFrame.right - 2.f * hudScale, artworkFrame.bottom - 2.f * hudScale };
			if (!ensureTrackArtwork(gMusicState.tracks[gMusicState.currentTrack], dc.ctx) ||
				!drawRoundedBitmap(dc, artworkRect, gMusicState.tracks[gMusicState.currentTrack].artwork.Get(), 16.f * hudScale)) {
				drawArtworkFallback(dc, artworkRect, 16.f * hudScale, 0.18f, false);
			}

			RectF contentRect = { artworkFrame.right + 20.f * hudScale, miniRect.top + 18.f * hudScale, miniRect.right - 20.f * hudScale, miniRect.bottom - 18.f * hudScale };
			RectF titleRect = { contentRect.left, contentRect.top + 2.f * hudScale, contentRect.right, contentRect.top + 44.f * hudScale };
			drawMarqueeText(dc, titleRect, trackName, d2d::Color(1.f, 1.f, 1.f, 0.98f), FontSelection::PrimaryRegular, 28.f * hudScale, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

			RectF artistRect = { contentRect.left, titleRect.bottom - 1.f * hudScale, contentRect.right, titleRect.bottom + 30.f * hudScale };
			dc.drawText(artistRect, artistName, d2d::Color(1.f, 1.f, 1.f, 0.92f), FontSelection::PrimaryRegular, 17.f * hudScale, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);

			Vec2 pillTextSize = dc.getTextSize(stateText, FontSelection::PrimaryRegular, 15.f * hudScale, true, false);
			float pillWidth = std::max(118.f * hudScale, pillTextSize.x + 28.f * hudScale);
			RectF timeRect = { contentRect.left, miniRect.bottom - 48.f * hudScale, contentRect.left + 120.f * hudScale, miniRect.bottom - 14.f * hudScale };
			dc.drawText(timeRect, timeText, d2d::Color(1.f, 1.f, 1.f, 0.98f), FontSelection::PrimaryRegular, 17.f * hudScale, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);

			RectF statePill = { miniRect.right - 22.f * hudScale - pillWidth, miniRect.bottom - 52.f * hudScale, miniRect.right - 22.f * hudScale, miniRect.bottom - 14.f * hudScale };
			drawPill(
				dc,
				statePill,
				stateText,
				getPlaybackStateFill(gMusicState, accentColor, 0.84f),
				d2d::Color(1.f, 1.f, 1.f, 0.98f),
				15.f * hudScale
			);
			return true;
		};

		if (!isActive()) {
			if (!drawMiniPlaybackHud(false)) return;
			return;
		}
		static bool loggedSafeMode = false;
		if (!loggedSafeMode) {
			Logger::Info("ClickGUI: SAFE render mode enabled");
			loggedSafeMode = true;
		}
		float uiScale = std::clamp(adaptedScale, 0.78f, 1.14f);
		float panelWidth = std::clamp(ss.width * 0.72f, 920.f, ss.width - 44.f * uiScale);
		float panelHeight = std::clamp(ss.height * 0.86f, 560.f, ss.height - 36.f * uiScale);
		rect = {
			(ss.width - panelWidth) * 0.5f,
			(ss.height - panelHeight) * 0.5f,
			(ss.width - panelWidth) * 0.5f + panelWidth,
			(ss.height - panelHeight) * 0.5f + panelHeight
		};
		rect.round();

		const float round = std::clamp(34.f * uiScale, 22.f, 38.f);
		float blurIntensity = std::max(18.f, Omoti::get().getMenuBlur().value_or(10.f) * 2.05f);
		auto panelTint = d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaGlass);
		auto panelOutline = d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaClear);
		auto headerTint = d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaGlass);
		auto panelCardTint = d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaGlass);
		auto popupTint = d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaGlass);
		auto oliveDark = d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaGlass);
		auto oliveCard = d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaGlass);
		auto oliveHover = d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaGlassHover);
		auto oliveActive = d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaGlassActive);
		auto softWhite = d2d::Color(1.f, 1.f, 1.f, 0.96f);
		auto softWhiteDim = d2d::Color(1.f, 1.f, 1.f, 0.80f);
		auto darkGlyph = d2d::Color::RGB(0x12, 0x12, 0x12).asAlpha(kMusicAlphaGlyph);

		drawGlassPanel(dc, rect, round, blurIntensity, panelTint, panelOutline);

		RectF headerRect = { rect.left + 52.f * uiScale, rect.top + 42.f * uiScale, rect.right - 52.f * uiScale, rect.top + 160.f * uiScale };
		RectF titleCardRect = { headerRect.left, headerRect.top, headerRect.left + headerRect.getWidth() * 0.61f, headerRect.top + 90.f * uiScale };
		drawGlassPanel(dc, titleCardRect, 20.f * uiScale, blurIntensity * 0.78f, headerTint, d2d::Color(0.f, 0.f, 0.f, 0.0f));

		RectF brandRect = { titleCardRect.left + 16.f * uiScale, titleCardRect.top + 16.f * uiScale, titleCardRect.left + 84.f * uiScale, titleCardRect.top + 84.f * uiScale };
		drawArtworkFallback(dc, brandRect, 14.f * uiScale, 0.15f, true);

		dc.drawText(
			{ brandRect.right + 22.f * uiScale, titleCardRect.top + 10.f * uiScale, titleCardRect.right - 16.f * uiScale, titleCardRect.top + 50.f * uiScale },
			L"Omoti Music Client",
			softWhite,
			FontSelection::PrimaryLight,
			31.f * uiScale,
			DWRITE_TEXT_ALIGNMENT_LEADING,
			DWRITE_PARAGRAPH_ALIGNMENT_CENTER
		);
		dc.drawText(
			{ brandRect.right + 22.f * uiScale, titleCardRect.top + 46.f * uiScale, titleCardRect.right - 16.f * uiScale, titleCardRect.bottom - 10.f * uiScale },
			L"by OmOti go",
			softWhite,
			FontSelection::PrimaryRegular,
			19.f * uiScale,
			DWRITE_TEXT_ALIGNMENT_LEADING,
			DWRITE_PARAGRAPH_ALIGNMENT_CENTER
		);

		auto drawHeaderActionButton = [&](RectF const& buttonRect, std::wstring const& label, bool active, bool closeButton = false) {
			bool hovered = shouldSelect(buttonRect, cursorPos);
			auto fill = active ? oliveActive : (hovered ? oliveHover : oliveCard);
			dc.fillRoundedRectangle(buttonRect, fill, 16.f * uiScale);
			dc.drawText(
				buttonRect,
				label,
				softWhite,
				closeButton ? FontSelection::PrimarySemilight : FontSelection::PrimaryRegular,
				closeButton ? 34.f * uiScale : 18.f * uiScale,
				DWRITE_TEXT_ALIGNMENT_CENTER,
				DWRITE_PARAGRAPH_ALIGNMENT_CENTER
			);
			return hovered;
		};

		const float actionGap = 16.f * uiScale;
		const float actionTop = titleCardRect.top + 4.f * uiScale;
		const float actionBottom = titleCardRect.bottom - 4.f * uiScale;
		RectF closeRect = { headerRect.right - 74.f * uiScale, actionTop, headerRect.right, actionBottom };
		RectF keysRect = { closeRect.left - actionGap - 126.f * uiScale, actionTop, closeRect.left - actionGap, actionBottom };
		RectF hudRect = { keysRect.left - actionGap - 116.f * uiScale, actionTop, keysRect.left - actionGap, actionBottom };
		bool hudHovered = drawHeaderActionButton(hudRect, L"HUD", quickPanel == QuickPanel::Hud);
		bool keysHovered = drawHeaderActionButton(keysRect, L"Keys", quickPanel == QuickPanel::Keys);
		bool closeHovered = drawHeaderActionButton(closeRect, L"X", false, true);
		if (justClicked[0] && hudHovered) {
			quickPanel = quickPanel == QuickPanel::Hud ? QuickPanel::None : QuickPanel::Hud;
			quickCaptureTarget = QuickCaptureTarget::None;
			playClickSound();
		}
		if (justClicked[0] && keysHovered) {
			quickPanel = quickPanel == QuickPanel::Keys ? QuickPanel::None : QuickPanel::Keys;
			quickCaptureTarget = QuickCaptureTarget::None;
			playClickSound();
		}
		if (justClicked[0] && closeHovered) {
			playClickSound();
			close();
			return;
		}

		RectF dividerRect = { headerRect.left, titleCardRect.bottom + 18.f * uiScale, headerRect.right, titleCardRect.bottom + 29.f * uiScale };
		dc.fillRoundedRectangle(dividerRect, d2d::Color(1.f, 1.f, 1.f, 0.94f), dividerRect.getHeight() * 0.5f);

		if (quickPanel != QuickPanel::None) {
			RectF quickPanelRect = { headerRect.right - 320.f * uiScale, dividerRect.bottom + 14.f * uiScale, headerRect.right, dividerRect.bottom + 194.f * uiScale };
			drawGlassPanel(
				dc,
				quickPanelRect,
				18.f * uiScale,
				blurIntensity * 0.82f,
				popupTint,
				d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaClear)
			);

			RectF titleRect = { quickPanelRect.left + 16.f * uiScale, quickPanelRect.top + 12.f * uiScale, quickPanelRect.right - 16.f * uiScale, quickPanelRect.top + 36.f * uiScale };
			if (quickPanel == QuickPanel::Hud) {
				dc.drawText(titleRect, L"HUD Menu", softWhite, FontSelection::PrimarySemilight, 13.5f * uiScale, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
				dc.drawText(
					{ titleRect.left, titleRect.bottom + 2.f * uiScale, quickPanelRect.right - 16.f * uiScale, titleRect.bottom + 32.f * uiScale },
					L"Move the Now Playing HUD with these buttons.",
					softWhiteDim,
					FontSelection::PrimaryRegular,
					10.f * uiScale,
					DWRITE_TEXT_ALIGNMENT_LEADING,
					DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
					false
				);

				auto nudgeHud = [&](float dx, float dy) {
					float x = gMusicState.miniHudNormX;
					float y = gMusicState.miniHudNormY;
					if (x < 0.f || y < 0.f) {
						x = 1.f;
						y = 0.095f;
					}
					gMusicState.miniHudNormX = std::clamp(x + dx, 0.f, 1.f);
					gMusicState.miniHudNormY = std::clamp(y + dy, 0.f, 1.f);
					saveMusicMeta(gMusicState);
				};
				auto drawHudMoveButton = [&](RectF const& buttonRect, std::wstring const& label) {
					bool hovered = shouldSelect(buttonRect, cursorPos);
					dc.fillRoundedRectangle(buttonRect, hovered ? oliveHover : oliveDark, 10.f * uiScale);
					dc.drawText(buttonRect, label, softWhite, FontSelection::PrimaryRegular, 11.f * uiScale, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
					return hovered;
				};

				const float btnW = 84.f * uiScale;
				const float btnH = 28.f * uiScale;
				const float btnGap = 10.f * uiScale;
				const float centerX = quickPanelRect.left + (quickPanelRect.getWidth() * 0.5f);
				RectF upRect = { centerX - btnW * 0.5f, quickPanelRect.top + 58.f * uiScale, centerX + btnW * 0.5f, quickPanelRect.top + 58.f * uiScale + btnH };
				RectF leftRect = { centerX - btnW - btnGap * 0.5f, upRect.bottom + btnGap, centerX - btnGap * 0.5f, upRect.bottom + btnGap + btnH };
				RectF rightRect = { centerX + btnGap * 0.5f, upRect.bottom + btnGap, centerX + btnGap * 0.5f + btnW, upRect.bottom + btnGap + btnH };
				RectF downRect = { centerX - btnW * 0.5f, leftRect.bottom + btnGap, centerX + btnW * 0.5f, leftRect.bottom + btnGap + btnH };
				bool upHovered = drawHudMoveButton(upRect, L"Up");
				bool leftHovered = drawHudMoveButton(leftRect, L"Left");
				bool rightHovered = drawHudMoveButton(rightRect, L"Right");
				bool downHovered = drawHudMoveButton(downRect, L"Down");
				const float nudge = 0.03f;
				if (justClicked[0] && upHovered) { nudgeHud(0.f, -nudge); playClickSound(); }
				if (justClicked[0] && leftHovered) { nudgeHud(-nudge, 0.f); playClickSound(); }
				if (justClicked[0] && rightHovered) { nudgeHud(nudge, 0.f); playClickSound(); }
				if (justClicked[0] && downHovered) { nudgeHud(0.f, nudge); playClickSound(); }

				RectF resetHudRect = { quickPanelRect.left + 16.f * uiScale, quickPanelRect.bottom - 36.f * uiScale, quickPanelRect.right - 16.f * uiScale, quickPanelRect.bottom - 10.f * uiScale };
				bool resetHudHovered = shouldSelect(resetHudRect, cursorPos);
				dc.fillRoundedRectangle(resetHudRect, resetHudHovered ? oliveHover : oliveDark, 10.f * uiScale);
				dc.drawText(resetHudRect, L"Reset HUD Position", softWhite, FontSelection::PrimaryRegular, 11.f * uiScale, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
				if (justClicked[0] && resetHudHovered) {
					gMusicState.miniHudNormX = -1.f;
					gMusicState.miniHudNormY = -1.f;
					gMusicState.miniHudDragging = false;
					gMusicState.miniHudPrevLmbDown = false;
					saveMusicMeta(gMusicState);
					playClickSound();
				}
			}
			else if (quickPanel == QuickPanel::Keys) {
				auto* menuSetting = findGlobalSetting("menuKey");
				auto* ejectSetting = findGlobalSetting("ejectKey");
				int menuVk = menuSetting && menuSetting->value ? std::get<KeyValue>(*menuSetting->value).value : Omoti::get().getMenuKey().value;
				int ejectVk = ejectSetting && ejectSetting->value ? std::get<KeyValue>(*ejectSetting->value).value : VK_END;

				dc.drawText(titleRect, L"Key Settings", softWhite, FontSelection::PrimarySemilight, 13.5f * uiScale, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
				dc.drawText(
					{ titleRect.left, titleRect.bottom + 2.f * uiScale, quickPanelRect.right - 16.f * uiScale, titleRect.bottom + 24.f * uiScale },
					L"Click a button, then press a key. ESC cancels capture.",
					softWhiteDim,
					FontSelection::PrimaryRegular,
					9.8f * uiScale,
					DWRITE_TEXT_ALIGNMENT_LEADING,
					DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
					false
				);

				auto drawKeyButton = [&](RectF const& buttonRect, std::wstring const& label, std::wstring const& value, bool activeCapture) {
					bool hovered = shouldSelect(buttonRect, cursorPos);
					auto fill = activeCapture ? oliveActive : (hovered ? oliveHover : oliveDark);
					dc.fillRoundedRectangle(buttonRect, fill, 10.f * uiScale);
					dc.drawText({ buttonRect.left + 12.f * uiScale, buttonRect.top, buttonRect.right - 90.f * uiScale, buttonRect.bottom }, label, softWhite, FontSelection::PrimaryRegular, 10.8f * uiScale, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
					dc.drawText({ buttonRect.right - 88.f * uiScale, buttonRect.top, buttonRect.right - 12.f * uiScale, buttonRect.bottom }, value, softWhiteDim, FontSelection::PrimaryRegular, 10.8f * uiScale, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
					return hovered;
				};

				RectF menuKeyRect = { quickPanelRect.left + 16.f * uiScale, quickPanelRect.top + 58.f * uiScale, quickPanelRect.right - 16.f * uiScale, quickPanelRect.top + 88.f * uiScale };
				RectF ejectKeyRect = { quickPanelRect.left + 16.f * uiScale, menuKeyRect.bottom + 10.f * uiScale, quickPanelRect.right - 16.f * uiScale, menuKeyRect.bottom + 40.f * uiScale };
				std::wstring menuValue = quickCaptureTarget == QuickCaptureTarget::Menu ? L"Press key..." : util::StrToWStr(util::KeyToString(menuVk));
				std::wstring ejectValue = quickCaptureTarget == QuickCaptureTarget::Eject ? L"Press key..." : util::StrToWStr(util::KeyToString(ejectVk));
				bool menuKeyHovered = drawKeyButton(menuKeyRect, L"Menu", menuValue, quickCaptureTarget == QuickCaptureTarget::Menu);
				bool ejectKeyHovered = drawKeyButton(ejectKeyRect, L"Eject", ejectValue, quickCaptureTarget == QuickCaptureTarget::Eject);
				if (justClicked[0] && menuKeyHovered) {
					quickCaptureTarget = QuickCaptureTarget::Menu;
					playClickSound();
				}
				if (justClicked[0] && ejectKeyHovered) {
					quickCaptureTarget = QuickCaptureTarget::Eject;
					playClickSound();
				}
			}
		}

		auto playbackSnapshot = updateMusicPlaybackSnapshot(gMusicState);
		ULONGLONG uiNowTick = playbackSnapshot.nowTick;
		bool inUiStabilize = playbackSnapshot.inUiStabilize;
		bool modePaused = playbackSnapshot.modePaused;
		bool modePlaying = playbackSnapshot.modePlaying;

		float contentTop = dividerRect.bottom + 28.f * uiScale;
		float contentBottom = rect.bottom - 46.f * uiScale;
		float gap = 30.f * uiScale;
		float listWidth = rect.getWidth() * 0.53f;
		RectF listRect = { headerRect.left, contentTop, headerRect.left + listWidth, contentBottom };
		RectF sideRect = { listRect.right + gap, contentTop, headerRect.right, rect.bottom - 52.f * uiScale };

		drawGlassPanel(dc, sideRect, 22.f * uiScale, blurIntensity * 0.92f, panelCardTint, d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaClear));

		RectF listItemsRect = { listRect.left, listRect.top, listRect.right - 8.f * uiScale, listRect.bottom };
		gMusicState.libraryViewport = listItemsRect;
		if (gMusicState.tracks.empty()) {
			gMusicState.libraryScroll = 0.f;
			gMusicState.libraryScrollMax = 0.f;
			RectF emptyRow = { listItemsRect.left + 6.f * uiScale, listItemsRect.top + 16.f * uiScale, listItemsRect.right - 12.f * uiScale, listItemsRect.top + 88.f * uiScale };
			dc.fillRoundedRectangle(emptyRow, d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaGlassHover), 18.f * uiScale);
			RectF emptyThumb = { emptyRow.left + 18.f * uiScale, emptyRow.top + 18.f * uiScale, emptyRow.left + 58.f * uiScale, emptyRow.top + 58.f * uiScale };
			drawArtworkFallback(dc, emptyThumb, 8.f * uiScale, 0.18f, true);
			dc.drawText({ emptyThumb.right + 16.f * uiScale, emptyRow.top + 14.f * uiScale, emptyRow.right - 16.f * uiScale, emptyRow.top + 38.f * uiScale }, L"No songs found", softWhite, FontSelection::PrimaryRegular, 15.f * uiScale, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
			dc.drawText({ emptyThumb.right + 16.f * uiScale, emptyRow.top + 38.f * uiScale, emptyRow.right - 16.f * uiScale, emptyRow.bottom - 14.f * uiScale }, L"Put files in RoamingState/Omoti/Music", softWhiteDim, FontSelection::PrimaryRegular, 11.f * uiScale, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
		}
		else {
			float rowH = 72.f * uiScale;
			float rowGap = 12.f * uiScale;
			float rowSpan = rowH + rowGap;
			float totalHeight = static_cast<float>(gMusicState.tracks.size()) * rowSpan;
			gMusicState.libraryScrollMax = std::max(0.f, totalHeight - listItemsRect.getHeight());
			gMusicState.libraryScroll = std::clamp(gMusicState.libraryScroll, 0.f, gMusicState.libraryScrollMax);
			float y = listItemsRect.top - gMusicState.libraryScroll;

			dc.ctx->PushAxisAlignedClip(listItemsRect.get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
			for (int i = 0; i < static_cast<int>(gMusicState.tracks.size()); i++) {
				RectF row = { listItemsRect.left + 2.f * uiScale, y, listItemsRect.right - 12.f * uiScale, y + rowH };
				y += rowSpan;
				if (row.bottom < listItemsRect.top) continue;
				if (row.top > listItemsRect.bottom) break;

				bool hovered = shouldSelect(row, cursorPos);
				bool isCurrent = i == gMusicState.currentTrack;
				bool isSelected = i == gMusicState.selectedTrack;
				auto rowFill =
					isCurrent ? d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaGlassActive) :
					(isSelected ? d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaGlassHover) :
					(hovered ? d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaGlassHover) : oliveDark));
				dc.fillRoundedRectangle(row, rowFill, 18.f * uiScale);

				RectF thumbRect = { row.left + 18.f * uiScale, row.top + 16.f * uiScale, row.left + 58.f * uiScale, row.top + 56.f * uiScale };
				if (!ensureTrackArtwork(gMusicState.tracks[i], dc.ctx) || !drawRoundedBitmap(dc, thumbRect, gMusicState.tracks[i].artwork.Get(), 8.f * uiScale)) {
					drawArtworkFallback(dc, thumbRect, 8.f * uiScale, 0.18f, true);
				}

				RectF titleRect = { thumbRect.right + 16.f * uiScale, row.top + 14.f * uiScale, row.right - 18.f * uiScale, row.top + 38.f * uiScale };
				RectF artistRect = { thumbRect.right + 16.f * uiScale, row.top + 36.f * uiScale, row.right - 18.f * uiScale, row.bottom - 12.f * uiScale };
				drawMarqueeText(dc, titleRect, getTrackDisplayTitle(gMusicState.tracks[i]), softWhite, FontSelection::PrimaryRegular, 14.f * uiScale);
				drawMarqueeText(dc, artistRect, getTrackDisplayArtist(gMusicState.tracks[i]), softWhiteDim, FontSelection::PrimaryRegular, 10.8f * uiScale);

				if (justClicked[0] && hovered) {
					gMusicState.selectedTrack = i;
					if (tryUseMusicActionCooldown(gMusicState.nextControlActionTick, 140)) {
						playMusicTrack(gMusicState, i);
						playClickSound();
					}
				}
			}

			if (gMusicState.libraryScrollMax > 0.f) {
				RectF scrollTrack = { listItemsRect.right - 6.f * uiScale, listItemsRect.top + 8.f * uiScale, listItemsRect.right - 1.f * uiScale, listItemsRect.bottom - 8.f * uiScale };
				dc.fillRoundedRectangle(scrollTrack, d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(kMusicAlphaScrollbarTrack), scrollTrack.getWidth() * 0.5f);
				float thumbHeight = std::max(34.f * uiScale, scrollTrack.getHeight() * (listItemsRect.getHeight() / totalHeight));
				float travel = std::max(1.f, scrollTrack.getHeight() - thumbHeight);
				float scrollRatio = std::clamp(gMusicState.libraryScroll / gMusicState.libraryScrollMax, 0.f, 1.f);
				RectF thumb = { scrollTrack.left, scrollTrack.top + travel * scrollRatio, scrollTrack.right, scrollTrack.top + travel * scrollRatio + thumbHeight };
				dc.fillRoundedRectangle(thumb, d2d::Color(1.f, 1.f, 1.f, 0.32f), thumb.getWidth() * 0.5f);
			}
			dc.ctx->PopAxisAlignedClip();
		}

		std::wstring trackTitle = getCurrentTrackLabel(gMusicState, L"Song name");
		std::wstring trackArtist = L"Artist name";
		if (gMusicState.currentTrack >= 0 && gMusicState.currentTrack < static_cast<int>(gMusicState.tracks.size())) {
			trackArtist = getTrackDisplayArtist(gMusicState.tracks[gMusicState.currentTrack]);
		}
		else if (gMusicState.selectedTrack >= 0 && gMusicState.selectedTrack < static_cast<int>(gMusicState.tracks.size())) {
			trackTitle = getTrackDisplayTitle(gMusicState.tracks[gMusicState.selectedTrack]);
			trackArtist = getTrackDisplayArtist(gMusicState.tracks[gMusicState.selectedTrack]);
		}
		if (trackArtist.empty()) trackArtist = L"Artist name";

		dc.drawText({ sideRect.left + 20.f * uiScale, sideRect.top + 18.f * uiScale, sideRect.right - 20.f * uiScale, sideRect.top + 62.f * uiScale },
			trackTitle, softWhite, FontSelection::PrimaryLight, 25.f * uiScale, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
		dc.drawText({ sideRect.left + 24.f * uiScale, sideRect.top + 56.f * uiScale, sideRect.right - 24.f * uiScale, sideRect.top + 88.f * uiScale },
			trackArtist, softWhiteDim, FontSelection::PrimaryRegular, 16.f * uiScale, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);

		float artSize = std::min(sideRect.getWidth() * 0.52f, sideRect.getHeight() * 0.42f);
		RectF artRect = { sideRect.center().x - artSize * 0.5f, sideRect.top + 110.f * uiScale, sideRect.center().x + artSize * 0.5f, sideRect.top + 110.f * uiScale + artSize };
		int artworkTrackIndex = -1;
		if (gMusicState.currentTrack >= 0 && gMusicState.currentTrack < static_cast<int>(gMusicState.tracks.size())) artworkTrackIndex = gMusicState.currentTrack;
		else if (gMusicState.selectedTrack >= 0 && gMusicState.selectedTrack < static_cast<int>(gMusicState.tracks.size())) artworkTrackIndex = gMusicState.selectedTrack;
		if (artworkTrackIndex >= 0 && ensureTrackArtwork(gMusicState.tracks[artworkTrackIndex], dc.ctx) && drawRoundedBitmap(dc, artRect, gMusicState.tracks[artworkTrackIndex].artwork.Get(), 18.f * uiScale)) {
		}
		else {
			drawArtworkFallback(dc, artRect, 18.f * uiScale, 0.18f, true);
		}

		enum class TransportGlyph {
			Play,
			Pause,
			Prev,
			Next
		};
		auto fillTriangle = [&](Vec2 a, Vec2 b, Vec2 c, d2d::Color const& color) {
			ComPtr<ID2D1PathGeometry> geometry;
			if (FAILED(Omoti::getRenderer().getFactory()->CreatePathGeometry(geometry.GetAddressOf())) || !geometry) return;

			ComPtr<ID2D1GeometrySink> sink;
			if (FAILED(geometry->Open(sink.GetAddressOf())) || !sink) return;

			D2D1_POINT_2F points[2] = {
				D2D1::Point2F(b.x, b.y),
				D2D1::Point2F(c.x, c.y)
			};

			sink->BeginFigure(D2D1::Point2F(a.x, a.y), D2D1_FIGURE_BEGIN_FILLED);
			sink->AddLines(points, 2);
			sink->EndFigure(D2D1_FIGURE_END_CLOSED);
			if (FAILED(sink->Close())) return;

			dc.brush->SetColor(color.get());
			dc.ctx->FillGeometry(geometry.Get(), dc.brush);
		};
		auto drawTransportCircleButton = [&](RectF const& rc, TransportGlyph glyph, float alpha = 0.96f, bool emphasized = false) {
			bool hovered = shouldSelect(rc, cursorPos);
			float radius = std::min(rc.getWidth(), rc.getHeight()) * 0.5f;
			float fillAlpha = hovered ? std::min(1.f, alpha + 0.04f) : alpha;
			dc.fillRoundedRectangle(rc, d2d::Color(1.f, 1.f, 1.f, fillAlpha), radius);

			auto iconColor = darkGlyph;
			Vec2 center = rc.center();
			float iconScale = emphasized ? 1.0f : 0.92f;

			switch (glyph) {
			case TransportGlyph::Pause: {
				float barHeight = rc.getHeight() * 0.34f * iconScale;
				float barWidth = rc.getWidth() * 0.088f * iconScale;
				float gapWidth = rc.getWidth() * 0.072f * iconScale;
				RectF leftBar = {
					center.x - gapWidth * 0.5f - barWidth,
					center.y - barHeight * 0.5f,
					center.x - gapWidth * 0.5f,
					center.y + barHeight * 0.5f
				};
				RectF rightBar = {
					center.x + gapWidth * 0.5f,
					center.y - barHeight * 0.5f,
					center.x + gapWidth * 0.5f + barWidth,
					center.y + barHeight * 0.5f
				};
				dc.fillRoundedRectangle(leftBar, iconColor, barWidth * 0.7f);
				dc.fillRoundedRectangle(rightBar, iconColor, barWidth * 0.7f);
				break;
			}
			case TransportGlyph::Play: {
				float triWidth = rc.getWidth() * 0.22f * iconScale;
				float triHeight = rc.getHeight() * 0.28f * iconScale;
				float left = center.x - triWidth * 0.44f;
				fillTriangle(
					{ left, center.y - triHeight * 0.5f },
					{ left, center.y + triHeight * 0.5f },
					{ left + triWidth, center.y },
					iconColor
				);
				break;
			}
			case TransportGlyph::Prev: {
				float barWidth = rc.getWidth() * 0.072f * iconScale;
				float barHeight = rc.getHeight() * 0.26f * iconScale;
				RectF bar = {
					center.x - rc.getWidth() * 0.18f,
					center.y - barHeight * 0.5f,
					center.x - rc.getWidth() * 0.18f + barWidth,
					center.y + barHeight * 0.5f
				};
				dc.fillRoundedRectangle(bar, iconColor, barWidth * 0.65f);
				float triWidth = rc.getWidth() * 0.18f * iconScale;
				float triHeight = rc.getHeight() * 0.24f * iconScale;
				float right = center.x + rc.getWidth() * 0.13f;
				fillTriangle(
					{ right, center.y - triHeight * 0.5f },
					{ right, center.y + triHeight * 0.5f },
					{ right - triWidth, center.y },
					iconColor
				);
				break;
			}
			case TransportGlyph::Next: {
				float barWidth = rc.getWidth() * 0.072f * iconScale;
				float barHeight = rc.getHeight() * 0.26f * iconScale;
				RectF bar = {
					center.x + rc.getWidth() * 0.11f,
					center.y - barHeight * 0.5f,
					center.x + rc.getWidth() * 0.11f + barWidth,
					center.y + barHeight * 0.5f
				};
				dc.fillRoundedRectangle(bar, iconColor, barWidth * 0.65f);
				float triWidth = rc.getWidth() * 0.18f * iconScale;
				float triHeight = rc.getHeight() * 0.24f * iconScale;
				float left = center.x - rc.getWidth() * 0.13f;
				fillTriangle(
					{ left, center.y - triHeight * 0.5f },
					{ left, center.y + triHeight * 0.5f },
					{ left + triWidth, center.y },
					iconColor
				);
				break;
			}
			}

			return hovered;
		};
		auto drawSideIcon = [&](RectF const& rc, SvgIconKind icon, bool active) {
			bool hovered = shouldSelect(rc, cursorPos);
			if (active || hovered) {
				dc.fillRoundedRectangle(rc, d2d::Color(1.f, 1.f, 1.f, active ? 0.14f : 0.08f), std::min(rc.getWidth(), rc.getHeight()) * 0.5f);
			}
			drawSvgIcon(dc, rc, icon);
			return hovered;
		};

		float ratio = syncPlaybackTimeline(gMusicState, uiNowTick, modePlaying, modePaused, inUiStabilize);
		int lengthMs = std::max(0, gMusicState.uiLengthMs);
		int posMs = std::max(0, gMusicState.uiPosMs);

		float timelineTop = artRect.bottom + 28.f * uiScale;
		RectF leftTimeRect = { sideRect.left + 54.f * uiScale, timelineTop - 10.f * uiScale, sideRect.left + 110.f * uiScale, timelineTop + 12.f * uiScale };
		RectF rightTimeRect = { sideRect.right - 110.f * uiScale, timelineTop - 10.f * uiScale, sideRect.right - 54.f * uiScale, timelineTop + 12.f * uiScale };
		dc.drawText(leftTimeRect, formatMusicTime(posMs), softWhiteDim, FontSelection::PrimaryRegular, 11.f * uiScale, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		dc.drawText(rightTimeRect, L"-" + formatMusicTime(lengthMs > 0 ? std::max(0, lengthMs - posMs) : 0), softWhiteDim, FontSelection::PrimaryRegular, 11.f * uiScale, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

		RectF seekRect = { sideRect.left + 54.f * uiScale, timelineTop + 16.f * uiScale, sideRect.right - 54.f * uiScale, timelineTop + 28.f * uiScale };
		dc.fillRoundedRectangle(seekRect, d2d::Color(1.f, 1.f, 1.f, 0.40f), seekRect.getHeight() * 0.5f);
		RectF seekFill = seekRect;
		seekFill.right = seekRect.left + seekRect.getWidth() * ratio;
		dc.fillRoundedRectangle(seekFill, d2d::Color(1.f, 1.f, 1.f, 0.96f), seekFill.getHeight() * 0.5f);
		float knobX = seekRect.left + seekRect.getWidth() * ratio;
		dc.brush->SetColor(softWhite.get());
		dc.ctx->FillEllipse(D2D1::Ellipse({ knobX, seekRect.centerY() }, 9.f * uiScale, 9.f * uiScale), dc.brush);

		float playSize = 70.f * uiScale;
		float prevNextSize = 42.f * uiScale;
		float sideSmallSize = 28.f * uiScale;
		float controlY = seekRect.bottom + 24.f * uiScale;
		float controlGap = 16.f * uiScale;
		float clusterW = sideSmallSize + controlGap + prevNextSize + controlGap + playSize + controlGap + prevNextSize + controlGap + sideSmallSize;
		float x = sideRect.center().x - clusterW * 0.5f;
		RectF shuffleRect = { x, controlY + (playSize - sideSmallSize) * 0.5f, x + sideSmallSize, controlY + (playSize - sideSmallSize) * 0.5f + sideSmallSize };
		x = shuffleRect.right + controlGap;
		RectF prevRect = { x, controlY + (playSize - prevNextSize) * 0.5f, x + prevNextSize, controlY + (playSize - prevNextSize) * 0.5f + prevNextSize };
		x = prevRect.right + controlGap;
		RectF playRect = { x, controlY, x + playSize, controlY + playSize };
		x = playRect.right + controlGap;
		RectF nextRect = { x, controlY + (playSize - prevNextSize) * 0.5f, x + prevNextSize, controlY + (playSize - prevNextSize) * 0.5f + prevNextSize };
		x = nextRect.right + controlGap;
		RectF repeatRect = { x, controlY + (playSize - sideSmallSize) * 0.5f, x + sideSmallSize, controlY + (playSize - sideSmallSize) * 0.5f + sideSmallSize };

		bool shuffleHovered = drawSideIcon(shuffleRect, SvgIconKind::Shuffle, gMusicState.shuffle);
		bool prevHovered = drawTransportCircleButton(prevRect, TransportGlyph::Prev, 0.94f);
		bool playHovered = drawTransportCircleButton(playRect, (gMusicState.paused || gMusicState.currentTrack < 0) ? TransportGlyph::Play : TransportGlyph::Pause, 0.98f, true);
		bool nextHovered = drawTransportCircleButton(nextRect, TransportGlyph::Next, 0.94f);
		bool repeatHovered = drawSideIcon(repeatRect, SvgIconKind::Repeat, gMusicState.repeat);

		if (justClicked[0] && prevHovered && tryUseMusicActionCooldown(gMusicState.nextControlActionTick, 140)) { playAdjacentTrack(gMusicState, -1); playClickSound(); }
		if (justClicked[0] && nextHovered && tryUseMusicActionCooldown(gMusicState.nextControlActionTick, 140)) { playAdjacentTrack(gMusicState, 1); playClickSound(); }
		if (justClicked[0] && repeatHovered) { gMusicState.repeat = !gMusicState.repeat; playClickSound(); }
		if (justClicked[0] && shuffleHovered) { gMusicState.shuffle = !gMusicState.shuffle; playClickSound(); }
		if (justClicked[0] && playHovered && tryUseMusicActionCooldown(gMusicState.nextControlActionTick, 140)) {
			if (gMusicState.currentTrack < 0 && gMusicState.selectedTrack >= 0) {
				playMusicTrack(gMusicState, gMusicState.selectedTrack);
			}
			else if (gMusicState.currentTrack >= 0) {
				if (gMusicState.paused) {
					int resumeTarget = std::max(0, gMusicState.uiPosMs);
					if (gMusicState.uiLengthMs > 0) {
						resumeTarget = std::clamp(resumeTarget, 0, std::max(0, gMusicState.uiLengthMs - 250));
					}
					bool resumed = seekMusicMs(gMusicState, resumeTarget) && resumeMusicPlayback();
					if (resumed) {
						gMusicState.paused = false;
						gMusicState.playStartTick = GetTickCount64();
						gMusicState.stoppedPollStreak = 0;
						gMusicState.uiStabilizeUntilTick = gMusicState.playStartTick + 1400;
						gMusicState.uiPosMs = resumeTarget;
						gMusicState.uiLastTick = gMusicState.playStartTick;
						gMusicState.modePausedStreak = 0;
						gMusicState.modePlayingStreak = 0;
					}
				}
				else {
					if (pauseMusicPlayback()) {
						gMusicState.paused = true;
						gMusicState.stoppedPollStreak = 0;
						gMusicState.uiStabilizeUntilTick = GetTickCount64() + 300;
						gMusicState.modePausedStreak = 0;
						gMusicState.modePlayingStreak = 0;
					}
				}
			}
			playClickSound();
		}

		static bool draggingSeek = false;
		if (justClicked[0] && shouldSelect(seekRect, cursorPos)) draggingSeek = true;
		if (!mouseButtons[0]) draggingSeek = false;
		if (draggingSeek && lengthMs > 0) {
			float t = std::clamp((cursorPos.x - seekRect.left) / seekRect.getWidth(), 0.f, 1.f);
			int targetMs = static_cast<int>(t * lengthMs);
			gMusicState.uiPosMs = targetMs;
			if (tryUseMusicActionCooldown(gMusicState.nextSeekActionTick, 60)) {
				seekMusicMs(gMusicState, targetMs);
			}
		}

		RectF volumeRect = { sideRect.left + 54.f * uiScale, controlY + playSize + 26.f * uiScale, sideRect.right - 54.f * uiScale, controlY + playSize + 38.f * uiScale };
		dc.fillRoundedRectangle(volumeRect, d2d::Color(1.f, 1.f, 1.f, 0.34f), volumeRect.getHeight() * 0.5f);
		float volRatio = std::clamp(gMusicState.volume, 0.f, 1.f);
		RectF volFill = volumeRect;
		volFill.right = volumeRect.left + (volumeRect.getWidth() * volRatio);
		dc.fillRoundedRectangle(volFill, d2d::Color(1.f, 1.f, 1.f, 0.94f), volFill.getHeight() * 0.5f);

		static bool draggingVol = false;
		if (justClicked[0] && shouldSelect(volumeRect, cursorPos)) draggingVol = true;
		if (!mouseButtons[0]) draggingVol = false;
		if (draggingVol) {
			float t = std::clamp((cursorPos.x - volumeRect.left) / volumeRect.getWidth(), 0.f, 1.f);
			gMusicState.volume = t;
			if (gMusicState.currentTrack >= 0 && tryUseMusicActionCooldown(gMusicState.nextVolumeActionTick, 60)) {
				setMusicBackendVolume(gMusicState.volume);
			}
		}

		if (shouldArrow) cursor = Cursor::Arrow;
		return;
	}

#if !OMOTI_MUSIC_GUI_SAFE_ONLY
	if (Omoti::get().getMenuBlur()) dc.drawGaussianBlur(Omoti::get().getMenuBlur().value() * (isActive() ? 1.f : calcAnim));

	// Animation
	D2D1::Matrix3x2F oTransform;
	D2D1::Matrix3x2F currentMatr;
	if (isActive()) {
		dc.ctx->GetTransform(&oTransform);

		dc.ctx->SetTransform(D2D1::Matrix3x2F::Scale({ calcAnim, calcAnim }, D2D1_POINT_2F(rect.center().x, rect.center().y)));
		dc.ctx->GetTransform(&currentMatr);
	}
	calcAnim = std::lerp(calcAnim, isActive() ? 1.f : 0.f, Omoti::getRenderer().getDeltaTime() * 0.2f);

	d2d::Color outline = accentColor.asAlpha(0.42f);
	d2d::Color rcColor = d2d::Color::RGB(0x10, 0x14, 0x1A).asAlpha(0.90f);
	rect.round();

	if (!isActive()) return;
	// Shadow effect stuff
	auto shadowEffect = Omoti::getRenderer().getShadowEffect();
	if (!shadowEffect || !compositeEffect) {
		Logger::Warn("ClickGUI: renderer effects are not initialized yet, skipping this frame");
		return;
	}
	shadowEffect->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0.f, 0.f, 0.f, 0.1f));
	auto affineTransformEffect = Omoti::getRenderer().getAffineTransformEffect();
	if (!affineTransformEffect) {
		Logger::Warn("ClickGUI: affine transform effect is null, skipping this frame");
		return;
	}

	D2D1::Matrix3x2F mat = D2D1::Matrix3x2F::Translation(10.f * adaptedScale, 5.f * adaptedScale);
	affineTransformEffect->SetInputEffect(0, shadowEffect);
	affineTransformEffect->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, mat);
	// Shadow effect bitmap
	auto myBitmap = rend.getBitmap();
	if (!myBitmap) {
		Logger::Warn("ClickGUI: renderer bitmap is null, skipping this frame");
		return;
	}
	//

	// Menu Rect
	dc.fillRoundedRectangle(rect, rcColor, 19.f * adaptedScale);
	dc.drawRoundedRectangle(rect, outline, 19.f * adaptedScale, 2.4f * adaptedScale, DrawUtil::OutlinePosition::Outside);
	d2d::Rect topBand = { rect.left + 8.f * adaptedScale, rect.top + 8.f * adaptedScale, rect.right - 8.f * adaptedScale, rect.top + 46.f * adaptedScale };
	dc.fillRoundedRectangle(topBand, d2d::Color::RGB(0x1D, 0x25, 0x2E).asAlpha(0.70f), 12.f * adaptedScale);
	d2d::Rect accentStrip = { rect.left + 20.f * adaptedScale, rect.top + 14.f * adaptedScale, rect.right - 20.f * adaptedScale, rect.top + 18.f * adaptedScale };
	dc.fillRoundedRectangle(accentStrip, accentColor.asAlpha(0.95f), 3.f * adaptedScale);

	float offX = 0.01689f * rect.getWidth();
	float offY = 0.03191f * rect.getHeight();
	float imgSize = 0.05338f * rect.getWidth();

	D2D1_RECT_F logoRect = { rect.left + offX, rect.top + offY, rect.left + offX + imgSize, rect.top + offY + imgSize };

	// Omoti Logo + text
	{
		{
			auto bmp = Omoti::getAssets().OmotiLogo.getBitmap();

			D2D1::Matrix3x2F oMat;
			auto sz = Omoti::getRenderer().getScreenSize();

			D2D1::Matrix3x2F m;

			//dc.ctx->GetTransform(&m);
			//dc.ctx->SetTransform(D2D1::Matrix3x2F::Scale(41.f / sz.width, 41.f / sz.height) * D2D1::Matrix3x2F::Translation(logoRect.left, logoRect.top) * m);
			dc.ctx->DrawBitmap(bmp, logoRect, 1.f);
			//dc.ctx->DrawImage(compositeEffect.Get(), { 0.f, 0.f });
			//dc.ctx->SetTransform(m);
		}


		// FIXME: this is scuffed
		// Omoti Text
		//dc.drawRectangle({ logoRect.right + 9.f * adaptedScale, logoRect.top, logoRect.right + 500.f, logoRect.bottom }, D2D1::ColorF::Red);
		float realLogoHeight = rect.getHeight() * 0.077921f;
		dc.drawText({ logoRect.right + 9.f * adaptedScale, logoRect.top, logoRect.right + 500.f, logoRect.top + realLogoHeight }, L"Music GUI", d2d::Color(1.f, 1.f, 1.f, 1.f), FontSelection::PrimaryLight, 24.f * adaptedScale, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	}

	// X button / other menus
	{
		// x btn calc
		float xOffs = rect.getWidth() * 0.02217f;
		float yOffs = rect.getHeight() * 0.04432f;

		float xWidth = rect.getWidth() * 0.02323f;
		float xHeight = xWidth;//rect.getHeight() * 0.04078f;

		RectF xRect = { rect.right - xOffs - xWidth, rect.top + yOffs, rect.right - xOffs, rect.top + yOffs + xHeight };

		auto bmp = Omoti::getAssets().xIcon.getBitmap();
		dc.ctx->DrawBitmap(bmp, xRect, 1.f);

		if (shouldSelect(xRect, cursorPos)) {
			if (justClicked[0]) {
				playClickSound();
				close();
			}
		}

		float betw = rect.getWidth() * 0.01795f;
		if (tab == SETTINGS) {
			float right = xRect.left - betw;
			RectF backArrowRect = { right - xWidth, xRect.top, right, xRect.bottom };
			{
				dc.ctx->DrawBitmap(Omoti::getAssets().arrowBackIcon.getBitmap(), backArrowRect);
			}
			if (shouldSelect(backArrowRect, cursorPos)) {
				if (justClicked[0]) {
					playClickSound();
					this->tab = MODULES;
				}
			}
		}
		else if (tab == MODULES) {
			// Music-only window mode: no extra top-right module buttons.
		}
	}

	// Search Bar + tabs (disabled for music-only window mode)
	RectF searchRect{ rect.left, logoRect.bottom, rect.left, logoRect.bottom };
	if (false) {
		float gaps = guiWidth * 0.02217f;
		float gapY = rect.getHeight() * 0.0175f;

		// prototype height = 564

		float searchWidth = guiWidth * 0.25f;
		float searchHeight = 0.0425f * rect.getHeight();
		float searchRound = searchHeight * 0.416f;

		searchRect = { logoRect.left, logoRect.bottom + gapY, logoRect.left + searchWidth, logoRect.bottom + gapY + searchHeight };
		auto searchCol = d2d::Color::RGB(0x70, 0x70, 0x70).asAlpha(0.28f);

		if (shouldSelect(searchRect, cursorPos)) {
			cursor = Cursor::IBeam;
			shouldArrow = false;

		}

		if (justClicked[0]) {
			if (shouldSelect(searchRect, cursorPos))
				searchTextBox.setSelected(true);
			else searchTextBox.setSelected(false);
		}

		{

			dc.ctx->SetTarget(shadowBitmap.Get());
			dc.ctx->Clear();

			dc.ctx->SetTransform(currentMatr);

			D2D1_ROUNDED_RECT rr;
			rr.radiusX = searchRound;
			rr.radiusY = searchRound;
			rr.rect = searchRect.get();
			rend.getSolidBrush()->SetColor(searchCol.get());
			dc.ctx->FillRoundedRectangle(rr, rend.getSolidBrush());

			// Shadow

			D2D1::Matrix3x2F matr = D2D1::Matrix3x2F::Translation(5 * adaptedScale, 5 * adaptedScale);
			affineTransformEffect->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, matr);
			shadowEffect->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0.f, 0.f, 0.f, 0.4f));
			shadowEffect->SetValue(D2D1_SHADOW_PROP_OPTIMIZATION, D2D1_SHADOW_OPTIMIZATION_SPEED);

			shadowEffect->SetInput(0, shadowBitmap.Get());
			compositeEffect->SetInputEffect(0, affineTransformEffect);
			compositeEffect->SetInput(1, shadowBitmap.Get());
			{
				std::wstring searchStr = L"";
				if (searchTextBox.getText().empty() && !searchTextBox.isSelected()) {
					if (this->tab == SETTINGS) {
						searchStr += L"Search Music Settings";
					}
					else if (this->tab == MODULES) {
						searchStr += L"Search Tracks";
					}
				}
				else {
					searchStr = searchTextBox.getText();
				}
				Vec2 ts = dc.getTextSize(searchTextBox.getText().substr(0, searchTextBox.getCaretLocation()), Renderer::FontSelection::PrimaryRegular, searchRect.getHeight() / 2.f);
				RectF textSearchRect = { searchRect.left + 5.f + searchRect.getHeight(), searchRect.top, searchRect.right - 5.f + searchRect.getHeight(), searchRect.bottom };
				d2d::Rect blinkerRect = { textSearchRect.left + ts.x, searchRect.top + 3.f, textSearchRect.left + ts.x + 2.f, searchRect.bottom - 3.f };
				if (searchTextBox.isSelected() && searchTextBox.shouldBlink()) dc.fillRectangle(blinkerRect, d2d::Color::RGB(0xB9, 0xB9, 0xB9));
				dc.drawText(textSearchRect, searchStr, d2d::Color::RGB(0xB9, 0xB9, 0xB9), FontSelection::PrimaryRegular, searchRect.getHeight() / 2.f, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
				dc.ctx->DrawBitmap(Omoti::getAssets().searchIcon.getBitmap(), { searchRect.left + 10.f, searchRect.top + 6.f, searchRect.left - 3.f + searchRect.getHeight(), searchRect.top + searchRect.getHeight() - 6.f });
			}

			dc.ctx->SetTarget(myBitmap);
		}

		if (tab == SETTINGS) {
			// actual settings
			auto& settings = Omoti::getSettings();

			float settingWidth = rect.getWidth() / 3.f;
			float padToSettings = 0.04787f * rect.getHeight();
			// float settings
			Vec2 setPos = { searchRect.left, searchRect.bottom + padToSettings };
			{
				// go through all float settings
				settings.forEach([&](std::shared_ptr<Setting> set) {
					if (setPos.y <= rect.bottom) {
						if (set->visible && set->shouldRender(settings) && set->value->index() == (size_t)Setting::Type::Float /* || set->value->index() == Setting::Type::Int*/) {
							setPos.y = drawSetting(set.get(), &settings, setPos, dc, settingWidth, 0.35f) + (setting_height_relative * rect.getHeight());
						}
					}
					});
			}

			// key/enum settings
			setPos.y += padToSettings;
			{
				// go through all enum settings
				settings.forEach([&](std::shared_ptr<Setting> set) {
					if (setPos.y <= rect.bottom) {
						if (set->visible && set->shouldRender(settings) && (set->value->index() == (size_t)Setting::Type::Key || set->value->index() == (size_t)Setting::Type::Enum || set->value->index() == (size_t)Setting::Type::Color || set->value->index() == (size_t)Setting::Type::Text)) {
							setPos.y = drawSetting(set.get(), &settings, setPos, dc, settingWidth) + (setting_height_relative * rect.getHeight());
						}
					}
					});
			}

			// bool settings
			setPos = { rect.left + rect.getWidth() * (1.3f / 3.f), searchRect.bottom + padToSettings };
			{
				// go through all bool settings
				settings.forEach([&](std::shared_ptr<Setting> set) {
					if (setPos.y <= rect.bottom) {
						if (set->visible && set->shouldRender(settings) && set->value->index() == (size_t)Setting::Type::Bool /* || set->value->index() == Setting::Type::Enum*/) {
							setPos.y = drawSetting(set.get(), &settings, setPos, dc, settingWidth) + (setting_height_relative * rect.getHeight());
						}
					}

					});
			}
		}
		else if (tab == MODULES) {

			// all, game, hud, etc buttons
			static std::vector<std::tuple<std::wstring, ClickGUI::ModTab, d2d::Color, float>> modTabs = { {L"All", ALL, searchCol, 0.f }, {L"Tracks", GAME, searchCol, 0.f }, {L"Playlists", HUD, searchCol, 0.f }, { L"Artists", SCRIPT, searchCol, 0.f } };

			float nodeWidth = guiWidth * 0.083f;

			RectF nodeRect = { searchRect.right + gaps, searchRect.top, searchRect.right + gaps + nodeWidth, searchRect.bottom };
			float pressDownHeight = searchRect.getHeight() / 10.f;

			for (auto& pair : modTabs) {
				RectF renderTabRect = nodeRect;

				float pressDownTranslate = pressDownHeight * std::get<3>(pair);
				renderTabRect = renderTabRect.translate(0.f, pressDownTranslate);

				bool contains = shouldSelect(renderTabRect, cursorPos);
				std::get<2>(pair) = util::LerpColorState(std::get<2>(pair), searchCol + 0.2f, searchCol, contains);

				if (justClicked[0] && contains) {
					this->modTab = std::get<1>(pair);
					playClickSound();
					scroll = 0.f;
				}

				std::get<3>(pair) = std::lerp(std::get<3>(pair), ((contains && mouseButtons[0]) || modTab == std::get<1>(pair)) ? 1.f : 0.f, Omoti::getRenderer().getDeltaTime() / 5.f);


				contains = shouldSelect(renderTabRect, cursorPos);

				if (pressDownTranslate < 0.01f) dc.ctx->SetTarget(shadowBitmap.Get());
				D2D1_ROUNDED_RECT rr{};
				rr.radiusX = searchRound;
				rr.radiusY = searchRound;
				rr.rect = renderTabRect.get();
				auto solidBrush = rend.getSolidBrush();
				if (this->modTab == std::get<1>(pair)) {
					solidBrush->SetColor((std::get<2>(pair) - 0.1f).get());
				}
				else {
					solidBrush->SetColor(std::get<2>(pair).get());
				}
				dc.ctx->FillRoundedRectangle(rr, rend.getSolidBrush());

				float baseColor = 1.f - (0.1f * std::get<3>(pair));
				dc.drawText(renderTabRect, std::get<0>(pair), { baseColor, baseColor, baseColor, 0.8f }, FontSelection::PrimaryRegular, nodeRect.getHeight() / 2.f, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
				dc.ctx->SetTarget(myBitmap);

				auto oWidth = nodeRect.getWidth() + gaps;
				nodeRect.left += oWidth;
				nodeRect.right += oWidth;

			}
		}
	}

	if (this->tab == MODULES) {
		const float panelPadX = guiWidth * 0.03f;
		const float panelPadY = rect.getHeight() * 0.03f;
		const float panelTop = logoRect.bottom + panelPadY;
		RectF panelRect = { rect.left + panelPadX, panelTop, rect.right - panelPadX, rect.bottom - panelPadY };
		dc.fillRoundedRectangle(panelRect, d2d::Color::RGB(0x1A, 0x1F, 0x27).asAlpha(0.84f), 12.f * adaptedScale);
		dc.drawRoundedRectangle(panelRect, d2d::Color::RGB(0x3C, 0x4A, 0x59).asAlpha(0.65f), 12.f * adaptedScale, 1.2f * adaptedScale, DrawUtil::OutlinePosition::Inside);

		const float contentPad = 10.f * adaptedScale;
		RectF titleRect = { panelRect.left + contentPad, panelRect.top + contentPad, panelRect.right - contentPad, panelRect.top + 28.f * adaptedScale };
		dc.drawText(titleRect, L"Music Folder Player", d2d::Color(1.f, 1.f, 1.f, 0.95f), FontSelection::PrimarySemilight, 16.f * adaptedScale, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

		RectF folderBoxRect = { panelRect.left + contentPad, titleRect.bottom + 6.f * adaptedScale, panelRect.right - contentPad, titleRect.bottom + 34.f * adaptedScale };
		dc.fillRoundedRectangle(folderBoxRect, d2d::Color::RGB(0x2C, 0x35, 0x40).asAlpha(0.95f), 6.f * adaptedScale);
		dc.drawText({ folderBoxRect.left + 8.f * adaptedScale, folderBoxRect.top, folderBoxRect.right - 8.f * adaptedScale, folderBoxRect.bottom },
			gMusicState.folderPath.wstring(), d2d::Color(1.f, 1.f, 1.f, 0.86f), FontSelection::PrimaryRegular, 12.f * adaptedScale, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);

		auto drawMusicButton = [&](RectF const& btn, std::wstring const& label) {
			auto hovered = shouldSelect(btn, cursorPos);
			dc.fillRoundedRectangle(btn, hovered ? d2d::Color::RGB(0x3B, 0x4B, 0x5D).asAlpha(0.95f) : d2d::Color::RGB(0x2A, 0x33, 0x3F).asAlpha(0.95f), 6.f * adaptedScale);
			dc.drawText(btn, label, d2d::Color(1.f, 1.f, 1.f, 0.9f), FontSelection::PrimaryRegular, 12.f * adaptedScale, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
			return hovered;
		};

		const float buttonTop = folderBoxRect.bottom + 8.f * adaptedScale;
		const float buttonGap = 6.f * adaptedScale;
		const float buttonWidth = std::min(120.f * adaptedScale, (folderBoxRect.getWidth() - buttonGap * 2.f) / 3.f);
		RectF loadRect = { folderBoxRect.left, buttonTop, folderBoxRect.left + buttonWidth, buttonTop + 24.f * adaptedScale };
		RectF pauseRect = loadRect.translate(buttonWidth + buttonGap, 0.f);
		RectF stopRect = pauseRect.translate(buttonWidth + buttonGap, 0.f);

		bool loadHovered = drawMusicButton(loadRect, L"Refresh");
		bool pauseHovered = drawMusicButton(pauseRect, gMusicState.paused ? L"Resume" : L"Pause");
		bool stopHovered = drawMusicButton(stopRect, L"Stop");

		if (justClicked[0] && loadHovered) {
			gMusicState.needsRefresh = true;
			playClickSound();
		}
		if (justClicked[0] && pauseHovered && gMusicState.currentTrack >= 0 && tryUseMusicActionCooldown(gMusicState.nextControlActionTick, 140)) {
			bool handled = gMusicState.paused ? resumeMusicPlayback() : pauseMusicPlayback();
			if (handled) {
				gMusicState.paused = !gMusicState.paused;
				playClickSound();
			}
		}
		if (justClicked[0] && stopHovered && tryUseMusicActionCooldown(gMusicState.nextControlActionTick, 140)) {
			if (MusicHelperClient::get().stop()) {
				gMusicState.currentTrack = -1;
				gMusicState.paused = false;
				playClickSound();
			}
		}

		RectF nowPlayingRect = { stopRect.right + 10.f * adaptedScale, buttonTop, panelRect.right - contentPad, stopRect.bottom };
		std::wstring nowPlaying = L"Stopped";
		if (gMusicState.currentTrack >= 0 && gMusicState.currentTrack < static_cast<int>(gMusicState.tracks.size())) {
			nowPlaying = (gMusicState.paused ? L"Paused: " : L"Playing: ") + getTrackDisplayTitle(gMusicState.tracks[gMusicState.currentTrack]);
		}
		drawMarqueeText(dc, nowPlayingRect, nowPlaying, d2d::Color(1.f, 1.f, 1.f, 0.82f), FontSelection::PrimaryRegular, 12.f * adaptedScale);

		const float filterTop = loadRect.bottom + 8.f * adaptedScale;
		RectF allRect = { panelRect.left + contentPad, filterTop, panelRect.left + contentPad + 82.f * adaptedScale, filterTop + 22.f * adaptedScale };
		RectF favRect = allRect.translate(allRect.getWidth() + 6.f * adaptedScale, 0.f);
		RectF playlistModeRect = favRect.translate(favRect.getWidth() + 6.f * adaptedScale, 0.f);

		auto drawFilterButton = [&](RectF const& btn, std::wstring const& label, bool activeFlag) {
			auto hovered = shouldSelect(btn, cursorPos);
			auto fill = activeFlag ? d2d::Color::RGB(0x3F, 0x5A, 0x7E).asAlpha(0.96f) : (hovered ? d2d::Color::RGB(0x2F, 0x3B, 0x4A).asAlpha(0.95f) : d2d::Color::RGB(0x24, 0x2D, 0x38).asAlpha(0.92f));
			dc.fillRoundedRectangle(btn, fill, 5.f * adaptedScale);
			dc.drawText(btn, label, d2d::Color(1.f, 1.f, 1.f, 0.93f), FontSelection::PrimaryRegular, 11.f * adaptedScale, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
			return hovered;
		};

		auto allHovered = drawFilterButton(allRect, L"All", gMusicState.filterMode == MusicGuiState::FilterMode::All);
		auto favHovered = drawFilterButton(favRect, L"Favorites", gMusicState.filterMode == MusicGuiState::FilterMode::Favorites);
		auto playlistModeHovered = drawFilterButton(playlistModeRect, L"Playlist", gMusicState.filterMode == MusicGuiState::FilterMode::Playlist);

		if (justClicked[0] && allHovered) gMusicState.filterMode = MusicGuiState::FilterMode::All;
		if (justClicked[0] && favHovered) gMusicState.filterMode = MusicGuiState::FilterMode::Favorites;
		if (justClicked[0] && playlistModeHovered) gMusicState.filterMode = MusicGuiState::FilterMode::Playlist;

		std::vector<std::wstring> playlistNames;
		playlistNames.reserve(gMusicState.playlists.size());
		for (auto const& [name, _] : gMusicState.playlists) playlistNames.push_back(name);
		std::ranges::sort(playlistNames);
		if (playlistNames.empty()) {
			gMusicState.playlists[L"Favorites"] = {};
			gMusicState.activePlaylist = L"Favorites";
			playlistNames.push_back(L"Favorites");
		}
		if (!gMusicState.playlists.contains(gMusicState.activePlaylist)) {
			gMusicState.activePlaylist = playlistNames.front();
		}

		RectF playlistCycleRect = { playlistModeRect.right + 10.f * adaptedScale, filterTop, playlistModeRect.right + 240.f * adaptedScale, filterTop + 22.f * adaptedScale };
		dc.fillRoundedRectangle(playlistCycleRect, d2d::Color::RGB(0x24, 0x31, 0x42).asAlpha(0.93f), 5.f * adaptedScale);
		dc.drawText(playlistCycleRect, L"Playlist: " + gMusicState.activePlaylist, d2d::Color(1.f, 1.f, 1.f, 0.9f), FontSelection::PrimaryRegular, 11.f * adaptedScale, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
		if (justClicked[0] && shouldSelect(playlistCycleRect, cursorPos) && playlistNames.size() > 1) {
			auto it = std::find(playlistNames.begin(), playlistNames.end(), gMusicState.activePlaylist);
			if (it == playlistNames.end() || ++it == playlistNames.end()) gMusicState.activePlaylist = playlistNames.front();
			else gMusicState.activePlaylist = *it;
			saveMusicMeta(gMusicState);
		}

		RectF createNameRect = { panelRect.left + contentPad, playlistCycleRect.bottom + 8.f * adaptedScale, panelRect.left + 320.f * adaptedScale, playlistCycleRect.bottom + 32.f * adaptedScale };
		playlistNameTextBox.setRect(createNameRect);
		playlistNameTextBox.render(dc, 5.f * adaptedScale, d2d::Color::RGB(0x24, 0x30, 0x3D).asAlpha(0.95f), d2d::Color(1.f, 1.f, 1.f, 1.f));
		if (justClicked[0]) playlistNameTextBox.setSelected(shouldSelect(createNameRect, cursorPos));

		RectF createBtnRect = { createNameRect.right + 8.f * adaptedScale, createNameRect.top, createNameRect.right + 96.f * adaptedScale, createNameRect.bottom };
		RectF addBtnRect = { createBtnRect.right + 8.f * adaptedScale, createBtnRect.top, createBtnRect.right + 120.f * adaptedScale, createBtnRect.bottom };
		RectF favBtnRect = { addBtnRect.right + 8.f * adaptedScale, addBtnRect.top, addBtnRect.right + 140.f * adaptedScale, addBtnRect.bottom };
		bool createHovered = drawMusicButton(createBtnRect, L"Create");
		bool addHovered = drawMusicButton(addBtnRect, L"Add Selected");
		bool favSelHovered = drawMusicButton(favBtnRect, L"Fav Selected");

		if (justClicked[0] && createHovered) {
			std::wstring name = playlistNameTextBox.getText();
			name.erase(name.begin(), std::find_if(name.begin(), name.end(), [](wchar_t c) { return !iswspace(c); }));
			name.erase(std::find_if(name.rbegin(), name.rend(), [](wchar_t c) { return !iswspace(c); }).base(), name.end());
			if (!name.empty()) {
				if (!gMusicState.playlists.contains(name)) gMusicState.playlists[name] = {};
				gMusicState.activePlaylist = name;
				playlistNameTextBox.setText(L"");
				saveMusicMeta(gMusicState);
				playClickSound();
			}
		}
		if (justClicked[0] && addHovered && gMusicState.selectedTrack >= 0 && gMusicState.selectedTrack < static_cast<int>(gMusicState.tracks.size())) {
			auto key = getTrackKey(gMusicState, gMusicState.tracks[gMusicState.selectedTrack]);
			auto& list = gMusicState.playlists[gMusicState.activePlaylist];
			if (!isTrackInPlaylist(list, key)) list.push_back(key);
			saveMusicMeta(gMusicState);
			playClickSound();
		}
		if (justClicked[0] && favSelHovered && gMusicState.selectedTrack >= 0 && gMusicState.selectedTrack < static_cast<int>(gMusicState.tracks.size())) {
			auto key = getTrackKey(gMusicState, gMusicState.tracks[gMusicState.selectedTrack]);
			if (gMusicState.favorites.contains(key)) gMusicState.favorites.erase(key);
			else gMusicState.favorites.insert(key);
			saveMusicMeta(gMusicState);
			playClickSound();
		}

		RectF listRect = { panelRect.left + contentPad, createNameRect.bottom + 10.f * adaptedScale, panelRect.right - contentPad, panelRect.bottom - contentPad };
		dc.fillRoundedRectangle(listRect, d2d::Color::RGB(0x13, 0x18, 0x20).asAlpha(0.92f), 8.f * adaptedScale);

		if (gMusicState.tracks.empty()) {
			dc.drawText(listRect, L"No supported music files found", d2d::Color(1.f, 1.f, 1.f, 0.6f), FontSelection::PrimaryRegular, 12.f * adaptedScale, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		}
		else {
			std::vector<int> displayIndices;
			displayIndices.reserve(gMusicState.tracks.size());
			auto activePlaylistIt = gMusicState.playlists.find(gMusicState.activePlaylist);
			for (int i = 0; i < static_cast<int>(gMusicState.tracks.size()); i++) {
				auto key = getTrackKey(gMusicState, gMusicState.tracks[i]);
				bool include = true;
				if (gMusicState.filterMode == MusicGuiState::FilterMode::Favorites) {
					include = gMusicState.favorites.contains(key);
				}
				else if (gMusicState.filterMode == MusicGuiState::FilterMode::Playlist) {
					include = activePlaylistIt != gMusicState.playlists.end() && isTrackInPlaylist(activePlaylistIt->second, key);
				}
				if (include) displayIndices.push_back(i);
			}

			if (displayIndices.empty()) {
				dc.drawText(listRect, L"No tracks in this filter", d2d::Color(1.f, 1.f, 1.f, 0.55f), FontSelection::PrimaryRegular, 12.f * adaptedScale, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
				goto music_list_done;
			}

			float rowHeight = 22.f * adaptedScale;
			float yRow = listRect.top + 4.f * adaptedScale;
			for (size_t k = 0; k < displayIndices.size(); k++) {
				int i = displayIndices[k];
				RectF row = { listRect.left + 4.f * adaptedScale, yRow, listRect.right - 4.f * adaptedScale, yRow + rowHeight };
				if (row.bottom > listRect.bottom) break;

				bool isCurrent = static_cast<int>(i) == gMusicState.currentTrack;
				bool hovered = shouldSelect(row, cursorPos);
				dc.fillRoundedRectangle(row, (isCurrent ? d2d::Color::RGB(0x2D, 0x5A, 0x3E).asAlpha(0.92f) : hovered ? d2d::Color::RGB(0x2E, 0x39, 0x47).asAlpha(0.92f) : d2d::Color::RGB(0x1A, 0x22, 0x2D).asAlpha(0.80f)), 5.f * adaptedScale);

				std::wstring title = getTrackDisplayTitle(gMusicState.tracks[i]);
				if (isCurrent) title = (gMusicState.paused ? L"[Paused] " : L"[Playing] ") + title;
				auto key = getTrackKey(gMusicState, gMusicState.tracks[i]);
				bool isFavorite = gMusicState.favorites.contains(key);
				RectF starRect = { row.right - 24.f * adaptedScale, row.top + 2.f * adaptedScale, row.right - 6.f * adaptedScale, row.bottom - 2.f * adaptedScale };
				dc.drawText(starRect, isFavorite ? L"*" : L"+", isFavorite ? d2d::Color::RGB(0xFF, 0xD2, 0x4A).asAlpha(0.95f) : d2d::Color(1.f, 1.f, 1.f, 0.5f), FontSelection::PrimarySemilight, 12.f * adaptedScale, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
				drawMarqueeText(dc, { row.left + 6.f * adaptedScale, row.top, starRect.left - 6.f * adaptedScale, row.bottom }, title, d2d::Color(1.f, 1.f, 1.f, 0.92f), FontSelection::PrimaryRegular, 11.f * adaptedScale);

				if (justClicked[0] && hovered) {
					if (shouldSelect(starRect, cursorPos)) {
						if (isFavorite) gMusicState.favorites.erase(key);
						else gMusicState.favorites.insert(key);
						saveMusicMeta(gMusicState);
					}
					else if (tryUseMusicActionCooldown(gMusicState.nextControlActionTick, 140)) {
						gMusicState.selectedTrack = i;
						playMusicTrack(gMusicState, static_cast<int>(i));
						playClickSound();
					}
				}

				yRow += rowHeight + 4.f * adaptedScale;
			}
		}
music_list_done:
		(void)0;
	}

	// Panels (legacy module UI disabled in music-only mode)
	if (false && this->tab == MODULES) {
		auto modulePad = guiWidth * 0.0317f;
		int numMods = 3;
		float modBetwPad = modulePad / 2.f;
		float totalPad = (modBetwPad * 2.f) + modulePad * 2.f;
		float modWidth = (guiWidth - totalPad) / numMods;
		float modHeight = 0.08F * rect.getHeight();
		float padFromSearchBar = 0.034F * rect.getHeight();

		float xStart = rect.left + modulePad;
		float x = xStart;
		float y = searchRect.bottom + padFromSearchBar;
		float modStartTop = y;

		dc.ctx->PushAxisAlignedClip({ rect.left, y, rect.right, rect.bottom }, D2D1_ANTIALIAS_MODE_ALIASED);
		modClip = { rect.left, y, rect.right, rect.bottom };

		y -= this->lerpScroll;

		this->scroll = std::clamp(scroll, 0.f, scrollMax);

		lerpScroll = std::lerp(lerpScroll, scroll, Omoti::getRenderer().getDeltaTime() / 5.f);

		std::vector<std::reference_wrapper<ModuleLike>> displayedModLikes;

		// filter what mods get actually displayed (search box / selected category tab), put them in displayedModLikes

		for (auto& mod : mods) {
			if (searchTextBox.getText().empty()) {
				if (modTab == ALL) {
					if (!mod.mod)
						continue;
				} else if (modTab == GAME) {
					if (!mod.mod || mod.mod->getCategory() == Module::HUD)
						continue;
				} else if (modTab == HUD) {
					if (!mod.mod || !mod.mod->isHud())
						continue;
				} else if (modTab == SCRIPT) {
					if (!mod.isMarketScript)
						continue;
				}
			} else {
				std::wstring lower = mod.name;
				std::ranges::transform(lower, lower.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
				std::wstring lowerSearch = searchTextBox.getText();
				std::ranges::transform(lowerSearch, lowerSearch.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });

				if (lower.rfind(lowerSearch) == std::string::npos)
					continue;
			}

			displayedModLikes.emplace_back(mod);
		}

		std::ranges::sort(displayedModLikes, ModuleLike::isLess);

		for (auto& modLikeRef : displayedModLikes) {
			auto& mod = modLikeRef.get();

			if (this->searchTextBox.getText().size() > 0) {
				mod.shouldRender = false;
				std::wstring lower = mod.name;
				std::ranges::transform(lower, lower.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
				std::wstring lowerSearch = searchTextBox.getText();
				std::ranges::transform(lowerSearch, lowerSearch.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });

				if (lower.rfind(lowerSearch) != std::string::npos)
					mod.shouldRender = true;
			}
		}

		int i = 0;
		int row = 1;
		int column = 1;

		std::array<float, 3> columnOffs = { 0.f, 0.f, 0.f };


		// modules
		scrollMax = 0.f;

		for (auto& modLikeRef : displayedModLikes) {
			auto& mod = modLikeRef.get();

			if (!mod.shouldRender) continue;
			Vec2 pos = { x, y + columnOffs[i] };
			RectF modRect = { pos.x, pos.y, pos.x + modWidth, pos.y + modHeight };

			if (jumpModule.has_value() && mod.mod && mod.mod->name() == *jumpModule) {
				scroll = pos.y - modStartTop;
				mod.isExtended = true;
			}

			float maxHoverOffset = modRect.getHeight() / 10.f;
			modRect = modRect.translate(0.f, -(maxHoverOffset * mod.lerpHover));
			RectF modRectActual = modRect;

			if (mod.modRect.has_value()) {
				if (mod.modRect->bottom < rect.top || mod.modRect->top > rect.bottom) {
					mod.modRect->setPos({ 0.f, pos.y });
					goto end;
				}
			}

			{
				float textHeight = 0.4f * modHeight;
				float rlBounds = modWidth * 0.04561f;

				// toggle width/height

				float togglePad = modHeight * 0.249f;
				float toggleWidth = modWidth * 0.143f;

				RectF toggleRect = { modRect.right - togglePad - toggleWidth, modRect.top + togglePad,
				modRect.right - togglePad, modRect.bottom - togglePad };

				// module settings calculations
				dc.ctx->SetTarget(auxiliaryBitmap.Get());
				bool renderExtended = mod.mod && mod.lerpArrowRot < 0.995f;
				if (renderExtended) {

					// clipped section
					{
						dc.ctx->Clear();

						float textSizeDesc = textHeight * 0.72f;
						float descTextPad = textSizeDesc / 3.f;
						RectF descTextRect = { modRect.left + rlBounds, modRect.bottom, toggleRect.left, modRect.bottom + textSizeDesc + descTextPad };
						descTextRect.bottom = descTextRect.top + dc.getTextSize(mod.description, Renderer::FontSelection::PrimaryRegular, textSizeDesc, true, true, Vec2{ descTextRect.getWidth(), descTextRect.getHeight() }).y + descTextPad;
						modRectActual.bottom = descTextRect.bottom;

						dc.drawText(descTextRect, mod.description, d2d::Color(1.f, 1.f, 1.f, 0.57f), FontSelection::PrimaryRegular, textSizeDesc, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

						{
							// Reset Button
							RectF resetRect = { toggleRect.left, descTextRect.top, toggleRect.right, descTextRect.top + toggleRect.getHeight() };

							dc.drawRoundedRectangle(resetRect, d2d::Color::RGB(0xFB, 0x36, 0x36), resetRect.getHeight() * (0.223f), 0.5f, DrawUtil::OutlinePosition::Inside);

							dc.drawText(resetRect, L"Reset", d2d::Color::RGB(0xFB, 0x36, 0x36), Renderer::FontSelection::PrimaryRegular, resetRect.getHeight() * 0.6f,
								DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);


							if (resetRect.contains(cursorPos) && justClicked[0]) {
								mod.mod->settings->forEach([&](std::shared_ptr<Setting> set) {
									*set->value = set->defaultValue;
									//std::visit([set](auto& obj) {
									//	static_assert(false, "");
									//	obj = std::get<std::remove_reference_t<decltype(obj)>>(set->defaultValue);
									//	}, *set->value);
									//});
									});
							}
						}

						float padToSetting = 0.014184f * rect.getHeight();

						modRectActual.bottom += padToSetting;
						mod.mod->settings->forEach([&](std::shared_ptr<Setting> set) {
							if (set->visible) {
								if (modRectActual.bottom <= rect.bottom) {
									if (set->shouldRender(*mod.mod->settings.get())) {
										float newY = drawSetting(set.get(), mod.mod->settings.get(), { descTextRect.left, modRectActual.bottom }, dc, descTextRect.getWidth(), 0.25f);
										modRectActual.bottom = (newY - modRectActual.bottom) > 2.f ? (newY + setting_height_relative * rect.getHeight() * 1.6f) : modRectActual.bottom;
									}
								}
							}
							});

						if (mod.mod->isHud() && static_cast<HUDModule*>(mod.mod.get())->isShowPreview()) {
							auto rMod = static_cast<HUDModule*>(mod.mod.get());

							RectF box = { modRectActual.left, modRectActual.bottom,
							modRectActual.right, modRectActual.bottom + mod.previewSize.y };

							Vec2 drawPos = box.center(mod.previewSize);
							D2D1::Matrix3x2F oTrans;

							dc.ctx->GetTransform(&oTrans);
							dc.ctx->SetTransform(D2D1::Matrix3x2F::Scale(1.f, 1.f) * D2D1::Matrix3x2F::Translation(drawPos.x, drawPos.y));
							rMod->render(dc, true, false);
							mod.previewSize = rMod->getRectNonScaled().getSize();
							dc.ctx->SetTransform(oTrans);
							modRectActual.bottom += box.getHeight() * 1.25f;
						}
					}
				}
				dc.ctx->SetTarget(myBitmap);

				if (renderExtended) {
					modRectActual.bottom = (modRect.bottom + (modRectActual.getHeight() - modRect.getHeight()) * (1.f - mod.lerpArrowRot));
					RectF clipRect = modRectActual;
					clipRect.left -= 10.f;
					clipRect.right += 10.f;
					dc.ctx->PushAxisAlignedClip(clipRect.get(), D2D1_ANTIALIAS_MODE_ALIASED);
				}


				dc.fillRoundedRectangle(modRectActual, d2d::Color::RGB(0x44, 0x44, 0x44).asAlpha(0.22f), .22f * modHeight);
				dc.drawRoundedRectangle(modRectActual, accentColor.asAlpha(1.f * mod.lerpToggle), .22f * modHeight, 1.f, DrawUtil::OutlinePosition::Inside);;
				if (renderExtended) {

					dc.ctx->DrawBitmap(auxiliaryBitmap.Get());
					dc.ctx->PopAxisAlignedClip();
				}

				// text
				auto textRect = modRect;
				textRect.left += modRect.getWidth() / 6.f;

				if (mod.isMarketScript) {
					float authorTextSize = modRect.getHeight() * 0.45f;
					textRect.bottom -= authorTextSize;

					auto authorRect = textRect;
					authorRect.top = textRect.bottom;
					authorRect.bottom = modRect.bottom;

					dc.drawText(authorRect, L"by " + mod.pluginAuthor, d2d::Color(1.f, 1.f, 1.f, 0.57f), FontSelection::PrimarySemilight, authorRect.getHeight() * 0.7f,
						DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
				}


				// Make the text end before the toggle rectangle
				textRect.right = toggleRect.left;
				dc.drawText(textRect, mod.name, { 1.f, 1.f, 1.f, 1.f }, FontSelection::PrimaryLight, textHeight, DWRITE_TEXT_ALIGNMENT_LEADING, mod.isMarketScript ? DWRITE_PARAGRAPH_ALIGNMENT_FAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

				// toggle

				if (mod.mod) {
					if (mod.mod->shouldHoldToToggle()) {
						d2d::Color color = d2d::Color::RGB(0xD9, 0xD9, 0xD9, 30);

						dc.fillRoundedRectangle(toggleRect, color, toggleRect.getHeight() / 4.f);
						dc.drawText(toggleRect, util::StrToWStr(util::KeyToString(mod.mod->getKeybind())), { 1.f, 1.f, 1.f, 1.f }, Renderer::FontSelection::PrimaryRegular,
							toggleRect.getHeight() / 2.f, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

						if (this->shouldSelect(toggleRect, cursorPos)) {
							setTooltip(L"Enable this module using the keybind.");
						}

					}
					else {
						bool selecToggle = this->shouldSelect(toggleRect, cursorPos);
						if (selecToggle) {
							if (justClicked[0]) {
								mod.mod->setEnabled(!mod.mod->isEnabled());
								playClickSound();
							}
						}
						static auto offCol = d2d::Color(mod.toggleColorOff);

						mod.toggleColorOn = util::LerpColorState(mod.toggleColorOn, accentColor + 0.2f, accentColor, selecToggle);
						mod.toggleColorOff = util::LerpColorState(mod.toggleColorOff, offCol + 0.2f, offCol, selecToggle);

						//float aTogglePadY = toggleRect.getHeight() * 0.15f;
						float radius = toggleRect.getHeight() * 0.35f;
						float circleOffs = toggleWidth * 0.27f;

						dc.fillRoundedRectangle(toggleRect, mod.mod->isEnabled() ? mod.toggleColorOn : mod.toggleColorOff, toggleRect.getHeight() / 2.f);
						Vec2 center{ toggleRect.left + circleOffs, toggleRect.centerY() };
						Vec2 center2 = center;
						center2.x = toggleRect.right - circleOffs;
						float onDist = center2.x - center.x;

						mod.lerpToggle = std::lerp(mod.lerpToggle, mod.mod->isEnabled() ? 1.f : 0.f, Omoti::getRenderer().getDeltaTime() * 0.3f);

						center.x += onDist * mod.lerpToggle;

						dc.brush->SetColor(d2d::Color(0xB9, 0xB9, 0xB9).get());
						dc.ctx->FillEllipse(D2D1::Ellipse({ center.x, center.y }, radius, radius), dc.brush);
					}
				}

				RectF arrowRc = { modRect.left + (modRect.getHeight() * 0.4f),
						modRect.top + (modRect.getHeight() * 0.4f), modRect.left + modRect.getHeight() * 0.70f, modRect.bottom - modRect.getHeight() * 0.4f };
				// arrow
				if (mod.mod) {


					if (this->shouldSelect(modRect, cursorPos) && !shouldSelect(toggleRect, cursorPos)) {
						if (justClicked[0]) {
							mod.isExtended = !mod.isExtended;
							//playClickSound();
						}
					}


					D2D1::Matrix3x2F oMatr;
					dc.ctx->GetTransform(&oMatr);
					float toLerp = mod.isExtended ? 0.f : 1.f;
					dc.ctx->SetTransform(D2D1::Matrix3x2F::Rotation((1.f - mod.lerpArrowRot) * 180.f, { arrowRc.centerX(), arrowRc.centerY() }) * oMatr);
					mod.lerpArrowRot = std::lerp(mod.lerpArrowRot, toLerp, Omoti::getRenderer().getDeltaTime() * 0.3f);
					// icon
					dc.ctx->DrawBitmap(Omoti::getAssets().arrowIcon.getBitmap(), arrowRc.get());
					dc.ctx->SetTransform(oMatr);
				}
				else if (mod.isMarketScript) {
					if (shouldSelect(modRect, cursorPos)) {
						setTooltip(mod.description);
					}

					auto installUpdateRect = toggleRect;
					// make it twice as wide as the toggle rect
					installUpdateRect.left = installUpdateRect.right - installUpdateRect.getWidth() * 1.5f;

					auto documentIconBitmap = Omoti::getAssets().document.getBitmap();
					auto bitmapSize = documentIconBitmap->GetPixelSize();

					// we can't directly use the arrow rect because the height to width ratio of the document icon is different
					// (need it to not look stretched)
					// height/width * width = height
					auto documentRect = arrowRc;
					float newWidth = arrowRc.getWidth() * 1.5f;
					documentRect.left = documentRect.centerX(newWidth);
					documentRect.right = documentRect.centerX(newWidth) + newWidth;
					auto heightByWidth = static_cast<float>(bitmapSize.height) / static_cast<float>(bitmapSize.width);
					auto height = heightByWidth * arrowRc.getWidth();

					float heightCenter = documentRect.centerY();
					documentRect.top = heightCenter - height / 2.f;
					documentRect.bottom = heightCenter + height / 2.f;

					// draw the icon
					dc.ctx->DrawBitmap(documentIconBitmap, documentRect.get());

					auto selecting = shouldSelect(installUpdateRect, cursorPos);
					if (selecting && justClicked[0] && !mod.pluginInstalled) {
						auto result = PluginManager::installScript(mod.pluginId);
						if (!result.has_value()) {
							auto& error = result.error();
							Omoti::getNotifications().push(util::StrToWStr(error));
						} else {
							shouldRebuildModLikes = true;
						}
					}

					mod.toggleColorOn = util::LerpColorState(mod.toggleColorOn, accentColor + 0.2f, accentColor, selecting);
					// draw install/update box
					dc.fillRoundedRectangle(installUpdateRect, mod.pluginInstalled ? mod.toggleColorOff : mod.toggleColorOn, installUpdateRect.getHeight() / 4.f);
					dc.drawText(installUpdateRect, mod.pluginInstalled ? L"Installed" : L"Install", d2d::Colors::WHITE, FontSelection::PrimaryLight, installUpdateRect.getHeight() / 2.f,
						DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
				}

			}
		end:
			columnOffs[i] += modRectActual.getHeight() - modRect.getHeight();
			// set mod rect
			mod.modRect = modRectActual;

			mod.lerpHover = std::lerp(mod.lerpHover, shouldSelect(modRectActual, cursorPos) ? 1.f : 0.f, Omoti::getRenderer().getDeltaTime() / 5.f);

			// scrolling max
			float scrollYNew = std::max(0.f, (modRectActual.bottom + padFromSearchBar) - rect.bottom) + lerpScroll;
			if (scrollYNew > scrollMax) scrollMax = scrollYNew;
			if (i >= (numMods - 1)) {
				i = 0;
				row++;
				column = 0;
				y += modRect.getHeight() + padFromSearchBar;
				x = xStart;
				continue;
			}
			else {
				x += modBetwPad + modWidth;
				column++;
			}
			i++;
		}

		dc.ctx->PopAxisAlignedClip();
	}

	dc.ctx->SetTransform(oTransform);
	dc.ctx->DrawImage(compositeEffect.Get());
	dc.ctx->SetTransform(currentMatr);

	modClip = std::nullopt;
	jumpModule = std::nullopt;

	if (colorPicker.setting) {
		drawColorPicker();
		if (colorPicker.queueClose) {
			auto& colVal = std::get<ColorValue>(*colorPicker.setting->value);
			colVal.isRGB = std::get<BoolValue>(colorPicker.rgbSelector);
			auto d2dCol = d2d::Color(util::HSVToColor(colorPicker.pickerColor)).asAlpha(colorPicker.opacityMod);
			*colorPicker.selectedColor = { d2dCol.r, d2dCol.g, d2dCol.b, d2dCol.a };
			colorPicker.setting->update();
			colorPicker.setting->userUpdate();
			colorPicker = ColorPicker();
		}
	}
	this->clearLayers();

	dc.ctx->SetTransform(oTransform);

	dc.ctx->SetTarget(Omoti::getRenderer().getBitmap());
	//dc.ctx->DrawImage(myBitmap);

	if (shouldArrow) cursor = Cursor::Arrow;
#endif
}

void ClickGUI::onInit(Event&) {
	if (kMusicGuiSafeOnly) return;
	auto myBitmap = Omoti::getRenderer().getBitmap();
	D2D1_SIZE_U bitmapSize = myBitmap->GetPixelSize();
	D2D1_PIXEL_FORMAT pixelFormat = myBitmap->GetPixelFormat();

	auto dc = Omoti::getRenderer().getDeviceContext();

	dc->CreateBitmap(bitmapSize, nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pixelFormat), shadowBitmap.GetAddressOf());
	dc->CreateBitmap(bitmapSize, nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pixelFormat), auxiliaryBitmap.GetAddressOf());
	dc->CreateEffect(CLSID_D2D1Composite, compositeEffect.GetAddressOf());

}

void ClickGUI::onCleanup(Event&) {
	gMusicState.libraryViewport = std::nullopt;
	if (kMusicGuiSafeOnly) return;
	compositeEffect = nullptr;
	shadowBitmap = nullptr;
	auxiliaryBitmap = nullptr;
}


void ClickGUI::onKey(Event& evGeneric) {
	auto& ev = reinterpret_cast<KeyUpdateEvent&>(evGeneric);
	if (ev.getKey() == VK_F11) return;

	if (quickCaptureTarget != QuickCaptureTarget::None && ev.isDown()) {
		if (ev.getKey() == VK_ESCAPE) {
			quickCaptureTarget = QuickCaptureTarget::None;
			ev.setCancelled(true);
			return;
		}

		std::string_view settingName = quickCaptureTarget == QuickCaptureTarget::Menu ? "menuKey" : "ejectKey";
		if (applyGlobalKeySetting(settingName, ev.getKey())) {
			Omoti::getConfigManager().saveCurrentConfig();
			playClickSound();
		}
		quickCaptureTarget = QuickCaptureTarget::None;
		ev.setCancelled(true);
		return;
	}

	if (searchTextBox.isSelected() || playlistNameTextBox.isSelected()) {
		ev.setCancelled(true);
		return;
	}
	if (this->activeSetting) {
		if (ev.isDown()) {
			if (ev.getKey() == VK_ESCAPE) {
				activeSetting = nullptr;
				ev.setCancelled(true);
				return;
			}
			else {
				this->capturedKey = ev.getKey();
			}
		}
	}

	if (ev.isDown() && ev.getKey() == VK_ESCAPE) {
		if (colorPicker.setting) {
			colorPicker.queueClose = true;
		}
		else {
			this->close();
		}
	}

	ev.setCancelled(true);
}

void ClickGUI::onClick(Event& evGeneric) {
	auto& ev = reinterpret_cast<ClickEvent&>(evGeneric);
	if (ev.getMouseButton() > 0) {
		if (isActive() && ev.getMouseButton() < 4) {
			size_t index = static_cast<size_t>(ev.getMouseButton() - 1);
			mouseButtons[index] = ev.isDown();
			if (ev.isDown()) {
				activeMouseButtons[index] = true;
				justClicked[index] = true;
			}
		}
		ev.setCancelled(true);
	}

	if (ev.getMouseButton() == 4) {
		Vec2 cursorPos = SDK::ClientInstance::get()->cursorPos;
		if (kMusicGuiSafeOnly && gMusicState.libraryViewport.has_value() && gMusicState.libraryViewport->contains(cursorPos)) {
			float delta = static_cast<float>(ev.getWheelDelta()) * 0.35f;
			gMusicState.libraryScroll = std::clamp(gMusicState.libraryScroll - delta, 0.f, gMusicState.libraryScrollMax);
		}
		else {
			this->scroll = std::max(std::min(scroll - static_cast<float>(ev.getWheelDelta()) / 3.f, scrollMax), 0.f);
		}
		ev.setCancelled(true);
	}
}


namespace {
	void drawAlphaBar(D2DUtil& dc, d2d::Rect rect, float nodeSize, int rows) {
		float endY = rect.top;
		endY += rect.getHeight() / rows;
		float beginY = rect.top;
		// gray part
		float bs = nodeSize;

		for (int i = 0; i < rows; i++) {
			if (i % 2 == 0) {
				for (float beginX = rect.left; beginX < rect.right; beginX += bs * 2.f) {
					float endX = std::min(rect.right, beginX + bs);
					dc.fillRectangle({ beginX, beginY, endX, endY }, { 1.f, 1.f, 1.f, 0.5f });

				}
			}
			else {
				for (float beginX = rect.left + bs; beginX < rect.right; beginX += bs * 2.f) {
					float endX = std::min(rect.right, beginX + bs);
					dc.fillRectangle({ beginX, beginY, endX, endY }, { 1.f, 1.f, 1.f, 0.5f });
				}
			}
			beginY = endY;
			endY += rect.getHeight() / rows;
		}
	}
}

float ClickGUI::drawSetting(Setting* set, SettingGroup*, Vec2 const& pos, D2DUtil& dc, float size, float fTextWidth, bool bypassClickThrough) {
	if (!set || !set->value) return pos.y;

	const float checkboxSize = rect.getWidth() * setting_height_relative;
	const float textSize = checkboxSize * 0.8f;
	const auto cursorPos = SDK::ClientInstance::get()->cursorPos;
	const float round = 0.1875f * checkboxSize;

	auto accentColor = d2d::Color(Omoti::get().getAccentColor().getMainColor());

	switch (static_cast<Setting::Type>(set->value->index())) {
	case Setting::Type::Text:
	{
		RectF rc = { pos.x, pos.y, (pos.x + size) - (fTextWidth * size), pos.y + checkboxSize };
		RectF txtRc = rc;
		RectF rightRc = rc;

		txtRc.left += (rc.getWidth() * (fTextWidth * 1.5f));
		rightRc.right = txtRc.left;

		d2d::Color col = d2d::Color::RGB(0x8D, 0x8D, 0x8D).asAlpha(0.11f);
		std::shared_ptr<TextBox> tb;
		for (auto& items : settingBoxes) {
			if (items.first == set) {
				tb = items.second;
			}
		}

		auto& textVal = std::get<TextValue>(*set->value);
		if (!tb) {
			tb = std::make_shared<TextBox>(txtRc);
			tb->setText(textVal.str);
			tb->setCaretLocation(static_cast<int>(textVal.str.size()));
			settingBoxes[set] = tb;
			Omoti::get().addTextBox(settingBoxes[set].get());

		}
		tb->setRect(txtRc);
		tb->render(dc, round, col, D2D1::ColorF::White);
		if (tb->isSelected()) {
			dc.drawRoundedRectangle(txtRc, D2D1::ColorF::White, round, 1.f);
		}


		if (justClicked[0]) {
			if (shouldSelect(tb->getRect(), cursorPos))
				tb->setSelected(true);
			else tb->setSelected(false);
		}

		textVal.str = tb->getText();
		dc.drawText(rightRc, set->getDisplayName(), { 1.f, 1.f, 1.f, 1.f }, Renderer::FontSelection::PrimarySemilight, textSize, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
		return rightRc.bottom;
	}
	break;
	case Setting::Type::Bool:
	{
		RectF checkboxRect = { pos.x, pos.y, pos.x + checkboxSize, pos.y + checkboxSize };
		bool contains = bypassClickThrough ? checkboxRect.contains(cursorPos) : this->shouldSelect(checkboxRect, cursorPos);

		auto colOff = d2d::Color::RGB(0xD9, 0xD9, 0xD9).asAlpha(0.11f);
		if (!set->rendererInfo.init) {
			set->rendererInfo.init = true;
			set->rendererInfo.col[0] = colOff.r;
			set->rendererInfo.col[1] = colOff.g;
			set->rendererInfo.col[2] = colOff.b;
			set->rendererInfo.col[3] = colOff.a;
		}
		auto lerpedColor = util::LerpColorState(set->rendererInfo.col, colOff + 0.1f, colOff, contains);
		set->rendererInfo.col[0] = lerpedColor.r;
		set->rendererInfo.col[1] = lerpedColor.g;
		set->rendererInfo.col[2] = lerpedColor.b;
		set->rendererInfo.col[3] = lerpedColor.a;

		if (contains && justClicked[0]) {
			std::get<BoolValue>(*set->value) = !std::get<BoolValue>(*set->value);
			set->update();
			set->userUpdate();
			playClickSound();
		}


		dc.fillRoundedRectangle(checkboxRect, Color(set->rendererInfo.col), round);
		if (std::get<BoolValue>(*set->value)) {
			float checkWidth = 0.6f * checkboxSize;
			float checkHeight = 0.375f * checkboxSize;
			RectF markRect = { checkboxRect.left + checkWidth / 4.f, checkboxRect.top + checkHeight / 2.f,
			checkboxRect.right - checkWidth / 4.f, checkboxRect.bottom - checkHeight / 2.f };

			dc.ctx->DrawBitmap(Omoti::getAssets().checkmarkIcon.getBitmap(), markRect);
		}
		float offs = checkboxSize * 0.66f;

		float newX = checkboxRect.right + offs;
		float rem = newX - pos.x;
		RectF textRect = { newX, checkboxRect.top, newX + (size - rem), checkboxRect.bottom };
		auto disp = set->getDisplayName();

		dc.drawText(textRect, disp, { 1.f, 1.f, 1.f, 1.f }, FontSelection::PrimarySemilight, textSize, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		auto desc = set->desc();
		if (!desc.empty())
			if (contains || shouldSelect(textRect, cursorPos)) setTooltip(desc);
		return checkboxRect.bottom;
	}
	break;
	case Setting::Type::Key:
	{
		RectF keyRect = { pos.x, pos.y, pos.x + checkboxSize * 2.f, pos.y + checkboxSize };
		bool contains = this->shouldSelect(keyRect, cursorPos);

		auto colOff = d2d::Color::RGB(0xD9, 0xD9, 0xD9).asAlpha(0.11f);
		if (!set->rendererInfo.init) {
			set->rendererInfo.init = true;
			set->rendererInfo.col[0] = colOff.r;
			set->rendererInfo.col[1] = colOff.g;
			set->rendererInfo.col[2] = colOff.b;
			set->rendererInfo.col[3] = colOff.a;
		}
		auto lerpedColor = util::LerpColorState(set->rendererInfo.col, colOff + 0.1f, colOff, contains);
		set->rendererInfo.col[0] = lerpedColor.r;
		set->rendererInfo.col[1] = lerpedColor.g;
		set->rendererInfo.col[2] = lerpedColor.b;
		set->rendererInfo.col[3] = lerpedColor.a;

		std::wstring text = util::StrToWStr(util::KeyToString(std::get<KeyValue>(*set->value)));

		if (set == activeSetting) {
			if (justClicked[0] && !contains) {
				activeSetting = nullptr;
			}
		}

		// white outline
		if (set == activeSetting) {
			text = L"...";
		}

		auto ts = dc.getTextSize(text, FontSelection::PrimaryRegular, textSize * 0.9f) + Vec2(8.f, 0.f);
		if (ts.x > keyRect.getWidth()) keyRect.right = keyRect.left + (ts.x);


		dc.fillRoundedRectangle(keyRect, Color(set->rendererInfo.col), round);

		dc.drawText(keyRect, text, d2d::Color(1.f, 1.f, 1.f, 1.f), FontSelection::PrimaryRegular, textSize * 0.9f,
			DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

		if (activeSetting == set) {
			dc.drawRoundedRectangle(keyRect, d2d::Color(1.f, 1.f, 1.f, 1.f), round);
		}
		if (activeSetting == set && this->capturedKey > 0) {
			std::get<KeyValue>(*set->value) = capturedKey;
			set->update();
			set->userUpdate();
			activeSetting = 0;
			capturedKey = 0;
		}


		float padToName = 0.006335f * rect.getWidth();
		float newX = keyRect.right + padToName;
		float rem = newX - pos.x;


		RectF textRect = { keyRect.right + padToName, keyRect.top, newX + (size - rem), keyRect.bottom };

		auto disp = set->getDisplayName();
		dc.drawText(textRect, disp, { 1.f, 1.f, 1.f, 1.f }, FontSelection::PrimarySemilight, textSize, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		if (!set->desc().empty())
			if (shouldSelect(textRect, cursorPos)) setTooltip(set->desc());
		if (shouldSelect(keyRect, cursorPos)) {
			setTooltip(L"Right click to reset");
			if (justClicked[0]) {
				if (!this->activeSetting) activeSetting = set;
				playClickSound();
			}
			if (justClicked[1]) {
				activeSetting = nullptr;
				std::get<KeyValue>(*set->value) = 0;
				set->update();
				set->userUpdate();
				playClickSound();
			}
		}
		return keyRect.bottom;
	}
	case Setting::Type::Enum:
	{
		RectF enumRect = { pos.x, pos.y, pos.x + checkboxSize * 2.f, pos.y + checkboxSize };
		bool contains = this->shouldSelect(enumRect, cursorPos);

		EnumValue& val = std::get<EnumValue>(*set->value);

		auto colOff = d2d::Color::RGB(0xD9, 0xD9, 0xD9).asAlpha(0.11f);
		if (!set->rendererInfo.init) {
			set->rendererInfo.init = true;
			set->rendererInfo.col[0] = colOff.r;
			set->rendererInfo.col[1] = colOff.g;
			set->rendererInfo.col[2] = colOff.b;
			set->rendererInfo.col[3] = colOff.a;
		}
		auto lerpedColor = util::LerpColorState(set->rendererInfo.col, colOff + 0.1f, colOff, contains);
		set->rendererInfo.col[0] = lerpedColor.r;
		set->rendererInfo.col[1] = lerpedColor.g;
		set->rendererInfo.col[2] = lerpedColor.b;
		set->rendererInfo.col[3] = lerpedColor.a;

		auto text = set->enumData->getSelectedName();

		auto ts = dc.getTextSize(text, FontSelection::PrimarySemilight, textSize * 0.9f) + Vec2(8.f, 0.f);
		if (ts.x > enumRect.getWidth()) enumRect.right = enumRect.left + (ts.x);

		dc.fillRoundedRectangle(enumRect, Color(set->rendererInfo.col), round);

		dc.drawText(enumRect, text, d2d::Color(1.f, 1.f, 1.f, 1.f), FontSelection::PrimaryRegular, textSize * 0.9f,
			DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

		if (activeSetting == set) {
			dc.drawRoundedRectangle(enumRect, d2d::Color(1.f, 1.f, 1.f, 1.f), round);
		}


		float padToName = 0.006335f * rect.getWidth();
		float newX = enumRect.right + padToName;
		float rem = newX - pos.x;


		RectF textRect = { enumRect.right + padToName, enumRect.top, newX + (size - rem), enumRect.bottom };

		dc.drawText(textRect, set->getDisplayName(), { 1.f, 1.f, 1.f, 1.f }, FontSelection::PrimaryRegular, textSize, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		if (!set->desc().empty())
			if (shouldSelect(textRect, cursorPos)) setTooltip(set->desc());

		if (shouldSelect(enumRect, cursorPos)) {
			if (set->enumData->getSelectedDesc().size() > 0) {
				setTooltip(set->enumData->getSelectedDesc());
			}
			else setTooltip(set->enumData->getSelectedName());
		}

		if (shouldSelect(enumRect, cursorPos)) {
			if (justClicked[0]) {
				// cycle
				set->enumData->next();
				set->update();
				set->userUpdate();
				playClickSound();
			}
		}
		return enumRect.bottom;
	}
	case Setting::Type::Color:
	{
		float padToName = 0.006335f * rect.getWidth();

		RectF colRect = { pos.x, pos.y, pos.x + checkboxSize * 2.f, pos.y + checkboxSize };
		bool contains = this->shouldSelect(colRect, cursorPos);
		std::wstring name = set->getDisplayName();

		auto& colVal = std::get<ColorValue>(*set->value);

		RectF textRect = { colRect.right + padToName, colRect.top, pos.x + size, colRect.bottom };
		dc.drawText(textRect, name, { 1.f, 1.f, 1.f, 1.f }, FontSelection::PrimarySemilight, textSize, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

		ComPtr<ID2D1LinearGradientBrush> gradientBrush;
		ComPtr<ID2D1GradientStopCollection> gradientStopCollection;

		D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES prop{};
		auto ss = Omoti::getRenderer().getScreenSize();
		prop.startPoint = { 0.f, ss.height / 2.f };
		prop.endPoint = { ss.width, ss.height / 2.f };

		d2d::Color col = { colVal.getMainColor().r, colVal.getMainColor().g, colVal.getMainColor().b, colVal.getMainColor().a };

		const D2D1_GRADIENT_STOP stops[] = {
			0.f, col.asAlpha(1.f).get(),
			1.f, col.get()
		};

		dc.ctx->CreateGradientStopCollection(stops, _countof(stops), gradientStopCollection.GetAddressOf());
		dc.ctx->CreateLinearGradientBrush(prop, gradientStopCollection.Get(), gradientBrush.GetAddressOf());

		gradientBrush->SetStartPoint({ colRect.left, textRect.centerY() });
		gradientBrush->SetEndPoint({ colRect.right, textRect.centerY() });
		ComPtr<ID2D1GradientStopCollection> stopCol;

		dc.fillRoundedRectangle(colRect, { 1.f, 1.f, 1.f, 0.4f }, round);
		// alpha bar

		float apad = 1.f;
		dc.ctx->PushAxisAlignedClip({ colRect.left + apad, colRect.top + apad, colRect.right - apad, colRect.bottom - apad }, D2D1_ANTIALIAS_MODE_ALIASED);
		drawAlphaBar(dc, colRect, colRect.getWidth() / 8.f, 6);
		dc.ctx->PopAxisAlignedClip();

		dc.fillRoundedRectangle(colRect, gradientBrush.Get(), round);
		dc.drawRoundedRectangle(colRect, gradientBrush.Get(), round, 1.f, DrawUtil::OutlinePosition::Inside);

		if (shouldSelect(colRect, cursorPos)) {
			if (justClicked[0]) {
				playClickSound();
				colorPicker.setting = set;
				std::get<BoolValue>(colorPicker.rgbSelector) = std::get<ColorValue>(*set->value).isRGB;
				colorPicker.dragging = false;
				cPickerRect = { colRect.left, colRect.bottom + 20.f, 0.f, 0.f };
				auto& colVal = std::get<ColorValue>(*set->value);
				colorPicker.selectedColor = &colVal.color1;
				auto sCol = *colorPicker.selectedColor;
				colorPicker.pickerColor = util::ColorToHSV({ sCol.r, sCol.g, sCol.b, sCol.a });
				colorPicker.hueMod = colorPicker.pickerColor.h / 360.f;
				colorPicker.svModX = colorPicker.pickerColor.s;
				colorPicker.svModY = 1.f - colorPicker.pickerColor.v;
				colorPicker.opacityMod = sCol.a;
			}
		}
		return colRect.bottom;
	}
	break;
	case Setting::Type::Float:
	{
		float textWidth = fTextWidth * size;
		float sliderHeight = (rect.getHeight() * 0.017730f);

		float textSz = textSize;//sliderHeight * 1.5f;

		RectF textRect = { pos.x, pos.y, pos.x + textWidth, pos.y + sliderHeight };
		RectF rtTextRect = textRect.translate(0.f, -(textRect.getHeight() / 2.f));
		std::wstringstream namew;
		namew << set->getDisplayName();
		dc.drawText(rtTextRect, namew.str(), d2d::Color(1.f, 1.f, 1.f, 1.f), FontSelection::PrimarySemilight, textSz, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
		float padToSlider = rect.getHeight() * 0.01063f;

		float sliderTop = textRect.top + sliderHeight * 0.16f;
		RectF sliderRect = { textRect.right, sliderTop, textRect.left + size - (0.1947f * size), sliderTop + sliderHeight };

		float innerPad = 0.2f * sliderRect.getHeight();
		RectF innerSliderRect = { sliderRect.left + innerPad, sliderRect.top + innerPad, sliderRect.right - innerPad, sliderTop + (rect.getHeight() * 0.017730f) - innerPad };

		std::wstringstream valuew;
		valuew << std::get<FloatValue>(*set->value);

		RectF rightRect = { sliderRect.right, sliderRect.top, pos.x + size, sliderRect.bottom };
		RectF rtRect = rightRect.translate(0.f, -(sliderRect.getHeight() / 2.f));
		dc.drawText(rtRect, valuew.str(), d2d::Color(1.f, 1.f, 1.f, 1.f), Renderer::FontSelection::PrimarySemilight, sliderHeight * 1.4f, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);

		float min = std::get<FloatValue>(set->min);
		float max = std::get<FloatValue>(set->max);
		float interval = std::get<FloatValue>(set->interval);

		if (!set->desc().empty() && (shouldSelect(textRect, cursorPos) || shouldSelect(sliderRect, cursorPos))) {
			setTooltip(set->desc());
		}

		if (!this->activeSetting) {
			if (justClicked[0] && shouldSelect(sliderRect, cursorPos)) {
				activeSetting = set;
				playClickSound();
			}
		}
		else {
			if (activeSetting == set) {
				if (!mouseButtons[0]) activeSetting = nullptr;

				float find = (cursorPos.x - sliderRect.left) / sliderRect.getWidth();

				float percent = (find);

				float newVal = percent * (std::get<FloatValue>(set->max) - min);
				newVal += min;

				newVal = std::clamp(newVal, min, max);

				// Find a good value to set to ("latch to nearest")
				newVal /= interval;
				newVal = std::round(newVal);
				newVal *= interval;

				std::get<FloatValue>(*set->value) = newVal;
				set->update();
				set->userUpdate();
			}
		}

		float percent = std::get<FloatValue>(*set->value) / max;
		float oRight = innerSliderRect.right;
		float oLeft = innerSliderRect.left;
		float newRight = 0.f;

		if (activeSetting == set) {
			newRight = cursorPos.x;
		}
		else {
			newRight = sliderRect.left + (sliderRect.getWidth() * percent);
		}
		innerSliderRect.right = std::clamp(newRight, oLeft, oRight);

		dc.fillRoundedRectangle(sliderRect, d2d::Color::RGB(0x8D, 0x8D, 0x8D).asAlpha(0.11f), sliderRect.getHeight() / 2.f);
		dc.fillRoundedRectangle(innerSliderRect, accentColor, innerSliderRect.getHeight() / 2.f);

		dc.brush->SetColor(d2d::Color(0xB9, 0xB9, 0xB9).get());
		dc.ctx->FillEllipse(D2D1::Ellipse({ innerSliderRect.right, sliderRect.centerY() }, sliderRect.getHeight() * 0.6f, sliderRect.getHeight() * 0.6f), dc.brush);
		return rtTextRect.top + dc.getTextSize(namew.str(), Renderer::FontSelection::PrimarySemilight, textSz, false, true, Vec2{ rtTextRect.getWidth(), rtTextRect.getHeight() }).y;
	}
	break;
	default:
		return pos.y;
	}
}

bool ClickGUI::shouldSelect(d2d::Rect rc, Vec2 const& pt) {
	if (modClip) {
		if (!modClip.value().contains(pt) || !Screen::shouldSelect(rc, pt)) {
			return false;
		}
	}
	return Screen::shouldSelect(rc, pt);
}

void ClickGUI::drawColorPicker() {
	auto& cursorPos = SDK::ClientInstance::get()->cursorPos;
	D2DUtil dc;
	dc.ctx->SetTarget(auxiliaryBitmap.Get());
	dc.ctx->Clear();

	float rectWidth = 0.2419f * rect.getWidth();
	cPickerRect.right = cPickerRect.left + rectWidth;

	float boxWidth = 0.79f * rectWidth;
	float remPad = (rectWidth - boxWidth) / 2.f;

	// Color PIcker Text
	float textSize = 0.09f * rectWidth;
	RectF titleRect = { cPickerRect.left + remPad, cPickerRect.top + remPad, cPickerRect.right - remPad, cPickerRect.top + remPad + textSize };

	{
		dc.drawText(titleRect, L"Color Picker", { 1.f, 1.f, 1.f, 1.f }, Renderer::FontSelection::PrimaryLight, textSize, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	}

	float boxTop = titleRect.bottom + remPad;

	RectF boxRect = { titleRect.left, boxTop, titleRect.right, boxTop + boxWidth };

	ComPtr<ID2D1LinearGradientBrush> mainColorBrush;
	ComPtr<ID2D1LinearGradientBrush> valueBrush;
	ComPtr<ID2D1LinearGradientBrush> hueBrush;
	ComPtr<ID2D1LinearGradientBrush> alphaBrush;

	// TODO: support chroma, multiple colors
	auto& colVal = std::get<ColorValue>(*colorPicker.setting->value);
	d2d::Color col = util::HSVToColor(colorPicker.pickerColor);
	d2d::Color sCol = { colorPicker.selectedColor->r, colorPicker.selectedColor->g, colorPicker.selectedColor->b, colorPicker.selectedColor->a };
	d2d::Color nsCol = util::HSVToColor({ util::ColorToHSV(sCol).h, 1.f, 1.f });
	d2d::Color baseCol = util::HSVToColor({ colorPicker.pickerColor.h, 1.f, 1.f });

	// main brush
	{
		ComPtr<ID2D1GradientStopCollection> gradientStopCollection;

		D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES prop{};
		auto ss = Omoti::getRenderer().getScreenSize();
		prop.startPoint = { boxRect.left, boxRect.top };
		prop.endPoint = { boxRect.right, boxRect.top };

		const D2D1_GRADIENT_STOP stops[] = {
			0.f, { 1.f, 1.f, 1.f, 1.f },
			1.f, baseCol.get()
		};

		dc.ctx->CreateGradientStopCollection(stops, _countof(stops), gradientStopCollection.GetAddressOf());
		dc.ctx->CreateLinearGradientBrush(prop, gradientStopCollection.Get(), mainColorBrush.GetAddressOf());
	}

	// Value brush
	{
		ComPtr<ID2D1GradientStopCollection> gradientStopCollection;

		D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES prop{};
		auto ss = Omoti::getRenderer().getScreenSize();
		prop.startPoint = { boxRect.left, boxRect.bottom };
		prop.endPoint = { boxRect.left, boxRect.top };

		const D2D1_GRADIENT_STOP stops[] = {
			0.f, { 0.f, 0.f, 0.f, 1.f},
			1.f, { 0.f, 0.f, 0.f, 0.f }
		};

		dc.ctx->CreateGradientStopCollection(stops, _countof(stops), gradientStopCollection.GetAddressOf());
		dc.ctx->CreateLinearGradientBrush(prop, gradientStopCollection.Get(), valueBrush.GetAddressOf());
	}
	// Draw inner part of colorpicker

	dc.fillRectangle(boxRect, mainColorBrush.Get());
	dc.fillRectangle(boxRect, valueBrush.Get());
	dc.drawRectangle(boxRect, d2d::Color::RGB(0x50, 0x50, 0x50).asAlpha(0.28f), 2.f);


	float hueBarHeight = boxRect.getHeight() * 0.0506329f;

	float padToHueBar = remPad * 0.6f;

	RectF hueBar = { boxRect.left, boxRect.bottom + padToHueBar, boxRect.right, boxRect.bottom + hueBarHeight + padToHueBar };

	// Hue brush
	{
		ComPtr<ID2D1GradientStopCollection> gradientStopCollection;

		D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES prop{};
		auto ss = Omoti::getRenderer().getScreenSize();
		prop.startPoint = { hueBar.left, hueBar.top };
		prop.endPoint = { hueBar.right, hueBar.top };

		float hueMod = 0.f;

		const D2D1_GRADIENT_STOP stops[] = {
			{0.f, d2d::Color(util::HSVToColor({ 0.f, 1.f, 1.f })).get()},
			{1.f / 7.f, d2d::Color(util::HSVToColor({ (1.f / 7.f) * 360.f, 1.f, 1.f })).get()},
			{2.f / 7.f, d2d::Color(util::HSVToColor({ (2.f / 7.f) * 360.f, 1.f, 1.f })).get()},
			{3.f / 7.f, d2d::Color(util::HSVToColor({ (3.f / 7.f) * 360.f, 1.f, 1.f })).get()},
			{4.f / 7.f, d2d::Color(util::HSVToColor({ (4.f / 7.f) * 360.f, 1.f, 1.f })).get()},
			{5.f / 7.f, d2d::Color(util::HSVToColor({ (5.f / 7.f) * 360.f, 1.f, 1.f })).get()},
			{6.f / 7.f, d2d::Color(util::HSVToColor({ (6.f / 7.f) * 360.f, 1.f, 1.f })).get()},
			{1.f, d2d::Color(util::HSVToColor({ 0.f, 1.f, 1.f })).get()},
		};

		dc.ctx->CreateGradientStopCollection(stops, 8, gradientStopCollection.GetAddressOf());
		dc.ctx->CreateLinearGradientBrush(prop, gradientStopCollection.Get(), hueBrush.GetAddressOf());
	}

	dc.fillRoundedRectangle(hueBar, hueBrush.Get(), hueBar.getHeight() / 2.f);
	dc.drawRoundedRectangle(hueBar, d2d::Color::RGB(0x50, 0x50, 0x50).asAlpha(0.28f), hueBar.getHeight() / 2.f, hueBar.getHeight() / 4.f, DrawUtil::OutlinePosition::Outside);

	RectF alphaBar = { hueBar.left, hueBar.bottom + padToHueBar, hueBar.right, hueBar.bottom + padToHueBar + hueBarHeight };

	// Alpha brush
	{
		ComPtr<ID2D1GradientStopCollection> gradientStopCollection;

		D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES prop{};
		auto ss = Omoti::getRenderer().getScreenSize();
		prop.startPoint = { alphaBar.left, alphaBar.top };
		prop.endPoint = { alphaBar.right, alphaBar.top };

		float hueMod = 0.f;

		const D2D1_GRADIENT_STOP stops[] = {
			{0.f, col.asAlpha(0.f).get()},
			{1.f, col.asAlpha(1.f).get()}
		};

		dc.ctx->CreateGradientStopCollection(stops, _countof(stops), gradientStopCollection.GetAddressOf());
		dc.ctx->CreateLinearGradientBrush(prop, gradientStopCollection.Get(), alphaBrush.GetAddressOf());
	}

	dc.fillRoundedRectangle(alphaBar, { 1.f, 1.f, 1.f, 0.5f }, alphaBar.getHeight() / 2.f);

	drawAlphaBar(dc, alphaBar, alphaBar.getHeight() / 2.f, 2);
	//dc.fillRoundedRectangle(hueBar, hueBrush.Get(), hueBar.getHeight() / 2.f);
	dc.fillRoundedRectangle(alphaBar, alphaBrush.Get(), alphaBar.getHeight() / 2.f);
	dc.drawRoundedRectangle(alphaBar, d2d::Color::RGB(0x37, 0x37, 0x37).asAlpha(0.88f), alphaBar.getHeight() / 2.f, alphaBar.getHeight() / 3.f, DrawUtil::OutlinePosition::Outside);

	// color hex edits/displays

	std::array<std::optional<StoredColor>, 3> cols = { colVal.getMainColor(), std::nullopt, std::nullopt };

	if (colVal.isChroma) {
		cols[1] = colVal.color2;
		cols[2] = colVal.color3;
	}

	RectF lastrc = alphaBar;
	for (size_t i = 0; i < cols.size(); ++i) {
		auto& c = cols[i];
		if (c.has_value()) {
			float colorModeWidth = alphaBar.getWidth() / 4.f;
			float hexBoxWidth = alphaBar.getWidth() * 0.617f;
			float boxHeight = alphaBar.getHeight() * 2.f;
			float colorDisplayWidth = boxHeight;

			float pad = (alphaBar.getWidth() - colorModeWidth - hexBoxWidth - colorDisplayWidth) / 3.f;

			RectF totalDisplayRect = lastrc.translate(0.f, padToHueBar);
			totalDisplayRect.bottom = totalDisplayRect.top + boxHeight;
			lastrc = totalDisplayRect;
			RectF colorModeRect = { totalDisplayRect.left, totalDisplayRect.top, totalDisplayRect.left + colorModeWidth, totalDisplayRect.bottom };
			RectF hexBox = { colorModeRect.right + pad, totalDisplayRect.top, colorModeRect.right + pad + hexBoxWidth, totalDisplayRect.bottom };
			RectF colorDisplayRect = { totalDisplayRect.right - pad - colorDisplayWidth, totalDisplayRect.top, totalDisplayRect.right - pad, totalDisplayRect.bottom };

			if (pickerTextBoxes.size() <= i) {
				pickerTextBoxes.insert(pickerTextBoxes.begin() + i, TextBox(hexBox, 7));
				Omoti::get().addTextBox(&pickerTextBoxes[i]);
			}
			auto& tb = pickerTextBoxes[i];

			auto bgCol = d2d::Color::RGB(0x50, 0x50, 0x50).asAlpha(0.28f);

			auto round = 0.1875f * colorModeRect.getHeight();

			dc.fillRoundedRectangle(colorModeRect, bgCol, round);
			//dc.fillRoundedRectangle(hexBox, bgCol, round);
			dc.fillRoundedRectangle(colorDisplayRect, col.asAlpha(colorPicker.opacityMod), round);

			std::wstring alphaTxt = util::StrToWStr(std::format("{:.2f}", colorPicker.opacityMod));

			dc.drawText(colorDisplayRect, alphaTxt, (colorPicker.opacityMod < 0.5f || colorPicker.pickerColor.v < 0.5f) ? D2D1::ColorF::White : D2D1::ColorF::Black, Renderer::FontSelection::PrimaryRegular, colorDisplayRect.getHeight() * 0.5f, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
			tb.setRect(hexBox);

			if (!tb.isSelected()) {
				tb.setText(util::StrToWStr("#" + col.getHex()));
			}

			tb.render(dc, round, bgCol, D2D1::ColorF::White);
			if (tb.isSelected()) {
				d2d::Color newCol = col;
				std::string txt = util::WStrToStr(tb.getText());
				if (txt[0] == '#') {
					txt = txt.substr(1);
				}
				if (txt.size() == 6)
					try {
					newCol = d2d::Color::Hex(txt);
				}
				catch (...) {
				}

				auto newHSV = util::ColorToHSV(newCol);
				colorPicker.svModX = newHSV.s;
				colorPicker.svModY = 1.f - newHSV.v;
				colorPicker.hueMod = newHSV.h / 360.f;
			}
			else {
				tb.setCaretLocation(static_cast<int>(tb.getText().size()));
			}

			if (justClicked[0]) {
				tb.setSelected(hexBox.contains(cursorPos));
			}


			// rgb setting
			colorPicker.rgbSetting.value = &colorPicker.rgbSelector;
			drawSetting(&colorPicker.rgbSetting, nullptr, { alphaBar.left, alphaBar.bottom + hexBox.getHeight() * 1.5f}, dc, 150.f, 0.21f, true);
		}
	}

	float ellipseRadius = 0.75f * alphaBar.getHeight();


	// sv
	if (colorPicker.isEditingSV || (justClicked[0] && boxRect.contains(cursorPos))) {
		colorPicker.svModX = std::max(std::min(cursorPos.x - boxRect.left, boxRect.getWidth()) / boxRect.getWidth(), 0.f);
		colorPicker.svModY = std::max(std::min(cursorPos.y - boxRect.top, boxRect.getHeight()) / boxRect.getHeight(), 0.f);

		colorPicker.isEditingSV = true;
	}

	// hue
	if (colorPicker.isEditingHue || (justClicked[0] && hueBar.contains(cursorPos))) {
		colorPicker.hueMod = std::max(std::min(cursorPos.x - hueBar.left, hueBar.getWidth()) / hueBar.getWidth(), 0.f);
		colorPicker.isEditingHue = true;
	}

	// alpha
	if (colorPicker.isEditingOpacity || (justClicked[0] && alphaBar.contains(cursorPos))) {
		colorPicker.opacityMod = std::max(std::min(cursorPos.x - alphaBar.left, alphaBar.getWidth()) / alphaBar.getWidth(), 0.f);

		float val = colorPicker.opacityMod;

		float interval = 0.05f;

		// Find a good value to set to ("latch to nearest")
		val /= interval;
		val = std::round(val);
		val *= interval;

		colorPicker.opacityMod = val;
		colorPicker.isEditingOpacity = true;
	}

	if (!mouseButtons[0]) {
		colorPicker.isEditingSV = false;
		colorPicker.isEditingHue = false;
		colorPicker.isEditingOpacity = false;
	}

	{
		colorPicker.pickerColor.h = (colorPicker.hueMod * 360.f);
		colorPicker.pickerColor.s = colorPicker.svModX;
		colorPicker.pickerColor.v = 1.f - colorPicker.svModY;
	}


	// SV
	{
		auto ellipse = D2D1::Ellipse({ boxRect.left + (hueBar.getWidth() * colorPicker.svModX), boxRect.top + (boxRect.getHeight() * colorPicker.svModY) }, ellipseRadius, ellipseRadius);
		dc.brush->SetColor(col.asAlpha(1.f).get());
		dc.ctx->FillEllipse(ellipse, dc.brush);
		dc.brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
		dc.ctx->DrawEllipse(ellipse, dc.brush, ellipseRadius / 2.f);
	}

	// hue
	{
		auto ellipse = D2D1::Ellipse({ hueBar.left + (hueBar.getWidth() * colorPicker.hueMod), hueBar.centerY() }, ellipseRadius, ellipseRadius);
		auto huedCol = util::HSVToColor({ colorPicker.hueMod * 360.f, 1.f, 1.f });
		dc.brush->SetColor(d2d::Color(huedCol).get());
		dc.ctx->FillEllipse(ellipse, dc.brush);
		dc.brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
		dc.ctx->DrawEllipse(ellipse, dc.brush, ellipseRadius / 2.f);
	}

	// alpha
	{
		auto ellipse = D2D1::Ellipse({ alphaBar.left + (alphaBar.getWidth() * colorPicker.opacityMod), alphaBar.centerY() }, ellipseRadius, ellipseRadius);
		dc.brush->SetColor(col.asAlpha(colorPicker.opacityMod).get());
		dc.ctx->FillEllipse(ellipse, dc.brush);
		dc.brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
		dc.ctx->DrawEllipse(ellipse, dc.brush, ellipseRadius / 2.f);
	}


	cPickerRect.bottom = alphaBar.bottom + remPad * 2.f + 50.f;

	dc.ctx->SetTarget(Omoti::getRenderer().getBitmap());

	// draw menu

	dc.fillRoundedRectangle(cPickerRect, d2d::Color::RGB(0x7, 0x7, 0x7).asAlpha(0.8f), 19.f * adaptedScale);
	dc.drawRoundedRectangle(cPickerRect, d2d::Color::RGB(0, 0, 0).asAlpha(0.28f), 19.f * adaptedScale, 4.f * adaptedScale, DrawUtil::OutlinePosition::Outside);

	// x button
	float xWidth = 0.06f * rectWidth;
	RectF xRect = { cPickerRect.right - xWidth * 2.f, cPickerRect.top + xWidth, cPickerRect.right - xWidth, cPickerRect.top + xWidth * 2.f };
	dc.ctx->DrawBitmap(Omoti::getAssets().xIcon.getBitmap(), xRect);

	if (justClicked[0] && xRect.contains(cursorPos)) {
		colorPicker.queueClose = true;
		playClickSound();
	}

	// inner contents
	dc.ctx->DrawBitmap(auxiliaryBitmap.Get());

	RectF pickerTopBar = { cPickerRect.left, cPickerRect.top, cPickerRect.right, boxRect.top };

	if (!colorPicker.dragging && justClicked[0] && pickerTopBar.contains(cursorPos)) {
		colorPicker.dragging = true;
		colorPicker.dragOffs = cursorPos - cPickerRect.getPos();
	}

	if (!mouseButtons[0]) colorPicker.dragging = false;

	if (colorPicker.dragging) {
		cPickerRect.setPos(cursorPos - colorPicker.dragOffs);
	}

	auto ss = Omoti::getRenderer().getScreenSize();
	util::KeepInBounds(cPickerRect, { 0.f, 0.f, ss.width, ss.height });
}

void ClickGUI::onEnable(bool ignoreAnims) {
	calcAnim = 0.f;
	if (ignoreAnims) calcAnim = 1.f;
	scroll = 0.f;
	lerpScroll = 0.f;
	mouseButtons = {};
	justClicked = {};
	this->tab = MODULES;
	this->quickPanel = QuickPanel::None;
	this->quickCaptureTarget = QuickCaptureTarget::None;
	ensureMusicStateInitialized(gMusicState);
	this->playlistNameTextBox.setSelected(false);
	// Avoid resetting current playback when reopening the GUI.
	// Auto-refresh only on first open (or when no tracks are loaded yet).
	gMusicState.needsRefresh = gMusicState.tracks.empty();
}

void ClickGUI::onDisable() {
	capturedKey = 0;
	activeSetting = nullptr;
	quickPanel = QuickPanel::None;
	quickCaptureTarget = QuickCaptureTarget::None;
	gMusicState.libraryViewport = std::nullopt;
	searchTextBox.reset();
	searchTextBox.setSelected(false);
	playlistNameTextBox.setSelected(false);

	for (auto& tb : this->settingBoxes) {
		tb.second->setSelected(false);
	}

	for (auto& tb : this->pickerTextBoxes) {
		tb.setSelected(false);
	}

	saveMusicMeta(gMusicState);
	Omoti::getConfigManager().saveCurrentConfig();
}
