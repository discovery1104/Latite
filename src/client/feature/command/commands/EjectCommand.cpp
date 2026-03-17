#include "pch.h"
#include "EjectCommand.h"
#include "client/Omoti.h"
#include "client/misc/ClientMessageQueue.h"

EjectCommand::EjectCommand() : Command("eject", LocalizeString::get("client.commands.eject.desc"), "{0}") {
}

bool EjectCommand::execute(std::string const label, std::vector<std::string> args) {
	message(LocalizeString::get("client.commands.eject.ejectMsg.name"));
	Omoti::get().queueEject();
	return true;
}
