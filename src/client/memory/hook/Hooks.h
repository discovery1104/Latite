#pragma once
#include "Hook.h"
#include "client/manager/StaticManager.h"

#include "hooks/GeneralHooks.h"
#include "hooks/LevelRendererHooks.h"
#include "hooks/OptionHooks.h"
#include "hooks/DXHooks.h"
#include "hooks/MinecraftGameHooks.h"
#include "hooks/RenderControllerHooks.h"
#include "hooks/ScreenViewHooks.h"
#include "hooks/PacketHooks.h"
#include "hooks/PlayerHooks.h"

class OmotiHooks final : public StaticManager<HookGroup,
	GenericHooks,
	DXHooks,
	MinecraftGameHooks> {
public:
	OmotiHooks();
	~OmotiHooks();

	void enable();
	void disable();
private:
};
