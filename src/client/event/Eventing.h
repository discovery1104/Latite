#pragma once
#include "client/event/Event.h"
#include "client/event/Listener.h"
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <algorithm>
#include <type_traits>

class Eventing final {
public:
	Eventing() = default;
	~Eventing() = default;

    template <typename T>
    bool dispatch(T&& ev) requires std::derived_from<std::remove_reference_t<T>, Event> {
        using EventType = std::remove_reference_t<T>;
        std::vector<EventListener> eventListeners;
        bool foundDeadListener = false;
        {
            std::shared_lock lock{ mutex };
            eventListeners.reserve(listeners.size());

            for (auto const& pair : listeners) {
                if (pair.first == EventType::hash) {
                    eventListeners.push_back(pair.second);
                }
            }
        }

        for (auto const& listener : eventListeners) {
            if (!listener.listener) continue;
            if (!Listener::isAlive(listener.listener, listener.listenerToken)) {
                foundDeadListener = true;
                continue;
            }
            if (listener.listener->shouldListen() || listener.callWhileInactive) {
                auto isCancel = ev.isCancellable();
                (listener.listener->*listener.fptr)(ev);
                if (isCancel) {
                    auto& cEv = reinterpret_cast<Cancellable&>(ev);
                    if (cEv.isCancelled()) {
                        return true;
                    }
                }
            }
        }

        if (foundDeadListener) {
            pruneDeadListeners();
        }
        return false;
    }

    // DO NOT USE, use listen<Event, &Listener::func> instead
    template <typename T>
    void listen(Listener* ptr, EventListenerFunc listener, int priority = 0, bool callWhileInactive = false) requires std::derived_from<T, Event> {
        std::unique_lock lock{ mutex };
        listeners.push_back({ T::hash, EventListener{ listener, ptr, callWhileInactive, priority } });
        std::sort(listeners.begin(), listeners.end(), [](std::pair<uint32_t, EventListener> const& left,
                                                         std::pair<uint32_t, EventListener> const& right) {
            return left.second.priority > right.second.priority;
        });
    }

    template <typename T, auto listener>
    void listen(Listener* ptr, int priority = 0, bool callWhileInactive = false) requires std::derived_from<T, Event> {
        std::unique_lock lock{ mutex };

        struct MFPtr {
            const EventListenerFunc ptr;
            const ptrdiff_t adj;
        };

        if constexpr (sizeof(listener) == sizeof(EventListenerFunc)) {
            listeners.push_back({ T::hash, EventListener{ static_cast<EventListenerFunc>(listener), ptr, callWhileInactive, priority } });
        } else if constexpr (sizeof(listener) == sizeof(MFPtr)) {
            const MFPtr mfp = std::bit_cast<MFPtr>(listener);

            listeners.push_back({ T::hash, EventListener{ mfp.ptr, ptr, callWhileInactive, priority } });
        } else {
            static_assert(false, "Unsupported listener function type");
        }
        std::sort(listeners.begin(), listeners.end(), [](std::pair<uint32_t, EventListener> const& left,
                                                         std::pair<uint32_t, EventListener> const& right) {
            return left.second.priority > right.second.priority;
        });
    }

    void unlisten(Listener* ptr) {
        std::unique_lock lock{ mutex };
        for (auto it = listeners.begin(); it != listeners.end();) {
            if (it->second.listener == ptr) {
                listeners.erase(it);
                continue;
            }
            ++it;
        }
    }

	// Substitute for Omoti::getEventing
	[[nodiscard]] static Eventing& get();
private:
    void pruneDeadListeners() {
        std::unique_lock lock{ mutex };
        listeners.erase(std::remove_if(listeners.begin(), listeners.end(),
            [](std::pair<uint32_t, EventListener> const& item) {
                auto const& listener = item.second;
                return !listener.listener || !Listener::isAlive(listener.listener, listener.listenerToken);
            }),
            listeners.end());
    }

    std::shared_mutex mutex;
    std::vector<std::pair<uint32_t, EventListener>> listeners;
};
