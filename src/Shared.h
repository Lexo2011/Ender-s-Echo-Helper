#pragma once

#include <windows.h>
#include "nexus/Nexus.h"
#include "mumble/Mumble.h"

constexpr const char* ADDON_NAME = "Ender's Echo Helper";
constexpr unsigned    MAP_HALL_OF_CHAINS = 1264;
constexpr unsigned    SPECIES_ENDERS_ECHO = 19628; // from Elite Insights, for the live source

extern AddonAPI_t*      APIDefs;
extern NexusLinkData_t* NexusLink;
extern Mumble::Data*    MumbleLink;
extern Mumble::Identity* MumbleIdentity;
