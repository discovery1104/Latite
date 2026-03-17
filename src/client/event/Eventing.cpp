#include "pch.h"
#include "Eventing.h"
#include "client/Omoti.h"

Eventing& Eventing::get() {
	return Omoti::getEventing();
}
