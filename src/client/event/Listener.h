#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

// Abstract class
class Listener {
public:
	Listener() : lifetimeToken(nextLifetimeToken()) {
		registerLiveListener(this, lifetimeToken);
	}

	Listener(Listener const&) : lifetimeToken(nextLifetimeToken()) {
		registerLiveListener(this, lifetimeToken);
	}

	Listener(Listener&&) noexcept : lifetimeToken(nextLifetimeToken()) {
		registerLiveListener(this, lifetimeToken);
	}

	Listener& operator=(Listener const&) = default;
	Listener& operator=(Listener&&) noexcept = default;

	virtual ~Listener() {
		unregisterLiveListener(this, lifetimeToken);
	}

	[[nodiscard]] uint64_t getLifetimeToken() const noexcept {
		return lifetimeToken;
	}

	static bool isAlive(Listener const* ptr, uint64_t token) {
		std::shared_lock lock{ liveListenersMutex };
		auto const it = liveListeners.find(ptr);
		return it != liveListeners.end() && it->second == token;
	}

	virtual bool shouldListen() { return true; }

private:
	static uint64_t nextLifetimeToken() {
		return nextToken.fetch_add(1, std::memory_order_relaxed);
	}

	static void registerLiveListener(Listener const* ptr, uint64_t token) {
		std::unique_lock lock{ liveListenersMutex };
		liveListeners[ptr] = token;
	}

	static void unregisterLiveListener(Listener const* ptr, uint64_t token) {
		std::unique_lock lock{ liveListenersMutex };
		auto const it = liveListeners.find(ptr);
		if (it != liveListeners.end() && it->second == token) {
			liveListeners.erase(it);
		}
	}

	uint64_t lifetimeToken = 0;

	inline static std::shared_mutex liveListenersMutex = {};
	inline static std::unordered_map<Listener const*, uint64_t> liveListeners = {};
	inline static std::atomic_uint64_t nextToken = 1;
};
