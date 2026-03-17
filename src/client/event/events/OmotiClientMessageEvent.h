#pragma once
class OmotiClientMessageEvent : public Event {
public:
	static const uint32_t hash = TOHASH(OmotiClientMessageEvent);

	OmotiClientMessageEvent(std::string const& msg) : message(msg) {}

	[[nodiscard]] std::string getMessage() { return message; }
private:
	std::string message;
};