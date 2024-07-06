#include "../Interfaces.h"
#include "../Memory.h"

#include "EnginePrediction.h"

#include "../SDK/ClientState.h"
#include "../SDK/Engine.h"
#include "../SDK/Entity.h"
#include "../SDK/EntityList.h"
#include "../SDK/FrameStage.h"
#include "../SDK/GameMovement.h"
#include "../SDK/GlobalVars.h"
#include "../SDK/MoveHelper.h"
#include "../SDK/Prediction.h"
#include "../SDK/PredictionCopy.h"


#include "../GameData.h"

EnginePrediction::NetvarData netvars{ };

static int localPlayerFlags;
static Vector localPlayerVelocity;
static bool inPrediction{ false };
static std::array<EnginePrediction::NetvarData, 150> netvarData;

void EnginePrediction::reset() noexcept
{
    localPlayerFlags = {};
    localPlayerVelocity = Vector{};
    netvarData = {};
    inPrediction = false;
}

void EnginePrediction::update() noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return;

    const auto deltaTick = memory->clientState->deltaTick;
    const auto start = memory->clientState->lastCommandAck;
    const auto stop = memory->clientState->lastOutgoingCommand + memory->clientState->chokedCommands;

    if (netvars.velocityModifier < 1.f)
        interfaces->prediction->inPrediction = true; // m_bFirstTimePrediction

    // correct prediction when framerate is lower than tickrate.
    // https://github.com/VSES/SourceEngine2007/blob/master/se2007/engine/cl_pred.cpp#L41
    if (deltaTick > 0)
    {
        // call CPrediction::Update.
        interfaces->prediction->update(deltaTick, deltaTick > 0, start, stop);
    }
}

void EnginePrediction::run(UserCmd* cmd) noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return;

    inPrediction = true;
    float sv_footsteps_backup = 0.0f;
    float sv_min_jump_landing_sound_backup = 0.0f;

    ConVar* sv_footsteps = nullptr;
    ConVar* sv_min_jump_landing_sound = nullptr;
    localPlayerFlags = localPlayer->flags();
    localPlayerVelocity = localPlayer->velocity();

    *memory->predictionRandomSeed = 0;
    *memory->predictionPlayer = reinterpret_cast<int>(localPlayer.get());

    const auto oldCurrenttime = memory->globalVars->currenttime;
    const auto oldFrametime = memory->globalVars->frametime;
    const auto oldIsFirstTimePredicted = interfaces->prediction->isFirstTimePredicted;
    const auto oldInPrediction = interfaces->prediction->inPrediction;

    memory->globalVars->currenttime = memory->globalVars->serverTime();
    memory->globalVars->frametime = interfaces->prediction->enginePaused ? 0 : memory->globalVars->intervalPerTick;
    interfaces->prediction->isFirstTimePredicted = false;
    interfaces->prediction->inPrediction = true;

    // backup footsteps.
    float backup_footsteps;
    {
        backup_footsteps = sv_footsteps_backup;
        float_t value = 0.0f;
        if (sv_footsteps)
            *(uint32_t*)(uintptr_t(sv_footsteps) + 0x2C) = (uint32_t)sv_footsteps ^ uint32_t(value);

        if (sv_min_jump_landing_sound)
            *(uint32_t*)(uintptr_t(sv_min_jump_landing_sound) + 0x2C) = (uint32_t)sv_min_jump_landing_sound ^ 0x7F7FFFFF;

        if (!sv_footsteps)
            sv_footsteps = interfaces->cvar->findVar("sv_footsteps");

        if (!sv_min_jump_landing_sound)
            sv_min_jump_landing_sound = interfaces->cvar->findVar("sv_min_jump_landing_sound");

        sv_footsteps_backup = *(float*)(uintptr_t(sv_footsteps) + 0x2C);
        sv_min_jump_landing_sound_backup = *(float*)(uintptr_t(sv_min_jump_landing_sound) + 0x2C);
    }

    if (cmd->impulse)
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(localPlayer.get()) + 0x320C) = cmd->impulse;

    cmd->buttons |= *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(localPlayer.get()) + 0x3344);
    cmd->buttons &= ~(*reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(localPlayer.get()) + 0x3340));
    localPlayer->updateButtonState(cmd->buttons);

    interfaces->gameMovement->startTrackPredictionErrors(localPlayer.get());
    interfaces->prediction->checkMovingGround(localPlayer.get(), memory->globalVars->frametime);

    localPlayer->runPreThink();
    localPlayer->runThink();

    memory->moveHelper->setHost(localPlayer.get());
    interfaces->prediction->setupMove(localPlayer.get(), cmd, memory->moveHelper, memory->moveData);
    interfaces->gameMovement->processMovement(localPlayer.get(), memory->moveData);
    interfaces->prediction->finishMove(localPlayer.get(), cmd, memory->moveData);
    memory->moveHelper->processImpacts();

    localPlayer->runPostThink();

    if (sv_footsteps)
        *(float*)(uintptr_t(sv_footsteps) + 0x2C) = backup_footsteps;

    interfaces->gameMovement->finishTrackPredictionErrors(localPlayer.get());
    memory->moveHelper->setHost(nullptr);
    interfaces->gameMovement->reset();

    *memory->predictionRandomSeed = -1;
    *memory->predictionPlayer = 0;

    memory->globalVars->currenttime = oldCurrenttime;
    memory->globalVars->frametime = oldFrametime;

    interfaces->prediction->isFirstTimePredicted = oldIsFirstTimePredicted;
    interfaces->prediction->inPrediction = oldInPrediction;

    const auto activeWeapon = localPlayer->getActiveWeapon();
    if (!activeWeapon || activeWeapon->isGrenade() || activeWeapon->isKnife())
        return;

    activeWeapon->updateAccuracyPenalty();



    inPrediction = false;
}

void EnginePrediction::store() noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return;

    const int tickbase = localPlayer->tickBase();
    netvars.tickbase = tickbase;

    netvars.aimPunchAngle = localPlayer->aimPunchAngle();
    netvars.aimPunchAngleVelocity = localPlayer->aimPunchAngleVelocity();
    netvars.baseVelocity = localPlayer->baseVelocity();
    netvars.duckAmount = localPlayer->duckAmount();
    netvars.duckSpeed = localPlayer->duckSpeed();
    netvars.fallVelocity = localPlayer->fallVelocity();
    netvars.thirdPersonRecoil = localPlayer->thirdPersonRecoil();
    netvars.velocity = localPlayer->velocity();
    netvars.velocityModifier = localPlayer->velocityModifier();
    netvars.viewPunchAngle = localPlayer->viewPunchAngle();
    netvars.viewOffset = localPlayer->viewOffset();

    netvarData.at(tickbase % 150) = netvars;
}

void EnginePrediction::restore() noexcept
{
    if (!localPlayer || !localPlayer->isAlive())
        return;

    const int tickbase = localPlayer->tickBase();
    netvars.tickbase = tickbase;

    localPlayer->aimPunchAngle() = netvars.aimPunchAngle;
    localPlayer->aimPunchAngleVelocity() = netvars.aimPunchAngleVelocity;
    localPlayer->baseVelocity() = netvars.baseVelocity;
    localPlayer->duckAmount() = netvars.duckAmount;
    localPlayer->duckSpeed() = netvars.duckSpeed;
    localPlayer->fallVelocity() = netvars.fallVelocity;
    localPlayer->thirdPersonRecoil() = netvars.thirdPersonRecoil;
    localPlayer->velocity() = netvars.velocity;
    localPlayer->velocityModifier() = netvars.velocityModifier;
    localPlayer->viewPunchAngle() = netvars.viewPunchAngle;
    localPlayer->viewOffset() = netvars.viewOffset;

    netvarData.at(tickbase % 150) = netvars;
}

void EnginePrediction::apply(FrameStage stage) noexcept
{
    if (stage != FrameStage::NET_UPDATE_END)
        return;

    if (!localPlayer || !localPlayer->isAlive())
        return;

    if (netvarData.empty())
        return;

    const int tickbase = localPlayer->tickBase();
    const auto& netvars = netvarData.at(tickbase % 150);

    if (!&netvars)
        return;

    if (netvars.tickbase != tickbase)
        return;

    localPlayer->aimPunchAngle() = NetvarData::checkDifference(localPlayer->aimPunchAngle(), netvars.aimPunchAngle);
    localPlayer->aimPunchAngleVelocity() = NetvarData::checkDifference(localPlayer->aimPunchAngleVelocity(), netvars.aimPunchAngleVelocity);
    localPlayer->baseVelocity() = NetvarData::checkDifference(localPlayer->baseVelocity(), netvars.baseVelocity);
    localPlayer->duckAmount() = std::clamp(NetvarData::checkDifference(localPlayer->duckAmount(), netvars.duckAmount), 0.0f, 1.0f);
    localPlayer->duckSpeed() = NetvarData::checkDifference(localPlayer->duckSpeed(), netvars.duckSpeed);
    localPlayer->fallVelocity() = NetvarData::checkDifference(localPlayer->fallVelocity(), netvars.fallVelocity);
    localPlayer->thirdPersonRecoil() = NetvarData::checkDifference(localPlayer->thirdPersonRecoil(), netvars.thirdPersonRecoil);
    localPlayer->velocity() = NetvarData::checkDifference(localPlayer->velocity(), netvars.velocity);
    localPlayer->velocityModifier() = NetvarData::checkDifference(localPlayer->velocityModifier(), netvars.velocityModifier);
    localPlayer->viewPunchAngle() = NetvarData::checkDifference(localPlayer->viewPunchAngle(), netvars.viewPunchAngle);
    localPlayer->viewOffset() = NetvarData::checkDifference(localPlayer->viewOffset(), netvars.viewOffset);
    localPlayer->tickBase() = static_cast<int>(NetvarData::checkDifference(localPlayer->tickBase(), netvars.tickbase));
}

int EnginePrediction::getFlags() noexcept
{
    return localPlayerFlags;
}

Vector EnginePrediction::getVelocity() noexcept
{
    return localPlayerVelocity;
}

bool EnginePrediction::isInPrediction() noexcept
{
    return inPrediction;
}