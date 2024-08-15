#include "hitscan.h"
#include "Animations.h"
#include "Resolver.h"

#include "../Logger.h"

#include "../SDK/GameEvent.h"
#include "../Vector2D.hpp"
#include <DirectXMath.h>
#include <algorithm>
#include "Backtrack.h"
std::deque<Resolver::SnapShot> snapshots;
static std::array<Animations::Players, 65> players{};
extern UserCmd* cmd;
bool resolver = true;
bool occlusion = false;

void Resolver::CmdGrabber1(UserCmd* cmd1)
{
    cmd = cmd1;
}

void Resolver::reset() noexcept
{
    snapshots.clear();
}

void Resolver::saveRecord(int playerIndex, float playerSimulationTime) noexcept
{
    const auto entity = interfaces->entityList->getEntity(playerIndex);
    const auto player = Animations::getPlayer(playerIndex);
    if (!player.gotMatrix || !entity)
        return;

    SnapShot snapshot;
    snapshot.player = player;
    snapshot.playerIndex = playerIndex;
    snapshot.eyePosition = localPlayer->getEyePosition();
    snapshot.model = entity->getModel();

    if (player.simulationTime >= playerSimulationTime - 0.001f && player.simulationTime <= playerSimulationTime + 0.001f)
    {
        snapshots.push_back(snapshot);
        return;
    }

    for (int i = 0; i < static_cast<int>(player.backtrackRecords.size()); i++)
    {
        if (player.backtrackRecords.at(i).simulationTime >= playerSimulationTime - 0.001f && player.backtrackRecords.at(i).simulationTime <= playerSimulationTime + 0.001f)
        {
            snapshot.backtrackRecord = i;
            snapshots.push_back(snapshot);
            return;
        }
    }
}

void Resolver::getEvent(GameEvent* event) noexcept
{
    if (!event || !localPlayer || interfaces->engine->isHLTV())
        return;

    switch (fnv::hashRuntime(event->getName())) {
    case fnv::hash("round_start"):
    {
        //Reset all
        auto players = Animations::setPlayers();
        if (players->empty())
            break;

        for (int i = 0; i < static_cast<int>(players->size()); i++)
        {
            players->at(i).misses = 0;
        }
        snapshots.clear();
        break;
    }
    case fnv::hash("player_death"):
    {
        //Reset player
        const auto playerId = event->getInt("userid");
        if (playerId == localPlayer->getUserId())
            break;

        const auto index = interfaces->engine->getPlayerFromUserID(playerId);
        Animations::setPlayer(index)->misses = 0;
        break;
    }
    case fnv::hash("player_hurt"):
    {
        if (snapshots.empty())
            break;

        if (event->getInt("attacker") != localPlayer->getUserId())
            break;

        const auto hitgroup = event->getInt("hitgroup");
        if (hitgroup < HitGroup::Head || hitgroup > HitGroup::RightLeg)
            break;

        snapshots.pop_front(); //Hit somebody so dont calculate
        break;
    }
    case fnv::hash("bullet_impact"):
    {
        if (snapshots.empty())
            break;

        if (event->getInt("userid") != localPlayer->getUserId())
            break;

        auto& snapshot = snapshots.front();

        if (!snapshot.gotImpact)
        {
            snapshot.time = memory->globalVars->serverTime();
            snapshot.bulletImpact = Vector{ event->getFloat("x"), event->getFloat("y"), event->getFloat("z") };
            snapshot.gotImpact = true;
        }
        break;
    }
    default:
        break;
    }
    if (!resolver)
        snapshots.clear();
}

float get_backward_side(Entity* entity) {
    if (!entity->isAlive())
        return -1.f;
    const float result = Helpers::angleDiff(localPlayer->origin().y, entity->origin().y);
    return result;
}
float get_angle(Entity* entity) {
    return Helpers::angleNormalize(entity->eyeAngles().y);
}
float get_forward_yaw(Entity* entity) {
    return Helpers::angleNormalize(get_backward_side(entity) - 180.f);
}

Vector calcAngle(Vector source, Vector entityPos) {
    Vector delta = {};
    delta.x = source.x - entityPos.x;
    delta.y = source.y - entityPos.y;
    delta.z = source.z - entityPos.z;
    Vector angles = {};
    Vector viewangles = cmd->viewangles;
    angles.x = Helpers::rad2deg(atan(delta.z / hypot(delta.x, delta.y))) - viewangles.x;
    angles.y = Helpers::rad2deg(atan(delta.y / delta.x)) - viewangles.y;
    angles.z = 0;
    if (delta.x >= 0.f)
        angles.y += 180;

    return angles;
}

float build_server_abs_yaw(Entity* entity, const float angle)
{
    float m_fl_goal_feet_yaw = 0;
     int sidecheck = 0;
    Resolver::detect_side(entity, &sidecheck);

    m_fl_goal_feet_yaw = entity->eyeAngles().y + (entity->getMaxDesyncAngle() * sidecheck);
    //abs_yaw = player->angles().y + desync * side;
    //entity->eyeAngles().y -player->angles().y
    //entity->getMaxDesyncAngle -desync
    
    return Helpers::normalizeYaw(m_fl_goal_feet_yaw);

}

void Resolver::detect_side(Entity* entity, int* side) {
    /* externals */
    Vector forward{};
    Vector right{};
    Vector up{};
    Trace tr;
    Helpers::AngleVectors(Vector(0, get_backward_side(entity), 0), &forward, &right, &up);
    /* filtering */

    const Vector src_3d = entity->getEyePosition();
    const Vector dst_3d = src_3d + forward * 384;

    /* back engine tracers */
    interfaces->engineTrace->traceRay({ src_3d, dst_3d }, MASK_SHOT, { entity }, tr);
    float back_two = (tr.endpos - tr.startpos).length();

    /* right engine tracers */
    interfaces->engineTrace->traceRay(Ray(src_3d + right * 35, dst_3d + right * 35), MASK_SHOT, { entity }, tr);
    float right_two = (tr.endpos - tr.startpos).length();

    /* left engine tracers */
    interfaces->engineTrace->traceRay(Ray(src_3d - right * 35, dst_3d - right * 35), MASK_SHOT, { entity }, tr);
    float left_two = (tr.endpos - tr.startpos).length();
    /* fix side */
    if (left_two > right_two) {
        *side = -1;
    }
    else if (right_two > left_two) {
        *side = 1;
    }
    else
        *side = 0;
}

bool DesyncDetect(Entity* entity)
{
    if (!localPlayer)
        return false;
    if (!entity || !entity->isAlive())
        return false;
    if (entity->isBot())
        return false;
    if (entity->team() == localPlayer->team())
        return false;
    if (entity->moveType() == MoveType::NOCLIP || entity->moveType() == MoveType::LADDER)
        return false;
    return true;
}

bool isSlowWalking(Entity* entity)
{
    float velocity_2D[64]{}, old_velocity_2D[64]{};
    if (entity->velocity().length2D() != velocity_2D[entity->index()] && entity->velocity().length2D() != NULL) {
        old_velocity_2D[entity->index()] = velocity_2D[entity->index()];
        velocity_2D[entity->index()] = entity->velocity().length2D();
    }
    Vector velocity = entity->velocity();
    Vector direction = entity->eyeAngles();

    float speed = velocity.length();
    direction.y = entity->eyeAngles().y - direction.y;
    //method 1
    if (velocity_2D[entity->index()] > 1) {
        int tick_counter[64]{};
        if (velocity_2D[entity->index()] == old_velocity_2D[entity->index()])
            tick_counter[entity->index()] += 1;
        else
            tick_counter[entity->index()] = 0;

        while (tick_counter[entity->index()] > (1 / memory->globalVars->intervalPerTick * fabsf(0.1f)))//should give use 100ms in ticks if their speed stays the same for that long they are definetely up to something..
            return true;
    }


    return false;
}

int GetChokedPackets(Entity* entity) {
    int last_ticks[65]{};
    auto ticks = timeToTicks(entity->simulationTime() - entity->oldSimulationTime());
    if (ticks == 0 && last_ticks[entity->index()] > 0) {
        return last_ticks[entity->index()] - 1;
    }
    else {
        last_ticks[entity->index()] = ticks;
        return ticks;
    }
}
void Resolver::resolve_entity(const Animations::Players& player, Entity* entity) {
    // get the players max rotation.
    //fix
    if (DesyncDetect(entity) == false)
        return;
    float max_rotation = entity->getMaxDesyncAngle();
    int index = 0;
    const float eye_yaw = entity->getAnimstate()->eyeYaw;
    if (!player.extended && fabs(max_rotation) > 60.f)
    {
        max_rotation = max_rotation / 1.8f;
    }

    // resolve shooting players separately.
    if (player.shot) {
        entity->getAnimstate()->footYaw = eye_yaw + resolve_shot(player, entity);
        return;
    }
    if (entity->velocity().length2D() <= 2.5f) {
        const float angle_difference = Helpers::angleDiff(eye_yaw, entity->getAnimstate()->footYaw);
        index = 2 * angle_difference <= 0.0f ? 1 : -1;
    }
    else
    {
        if (!static_cast<int>(player.layers[12].weight * 1000.f) && entity->velocity().length2D() > 3.f) {
            const auto m_layer_delta1 = abs(player.layers[6].playbackRate - player.oldlayers[6].playbackRate);
            const auto m_layer_delta2 = abs(player.layers[6].playbackRate - player.oldlayers[6].playbackRate);

            if (const auto m_layer_delta3 = abs(player.layers[6].playbackRate - player.oldlayers[6].playbackRate); m_layer_delta1 < m_layer_delta2
                || m_layer_delta3 <= m_layer_delta2
                || static_cast<signed int>((m_layer_delta2 * 1000.0f)))
            {
                if (m_layer_delta1 >= m_layer_delta3
                    && m_layer_delta2 > m_layer_delta3
                    && !static_cast<signed int>((m_layer_delta3 * 1000.0f)))
                {
                    index = 1;
                }
            }
            else
            {
                index = -1;
            }
        }
    }

    switch (player.misses % 3) {
    case 0: //default
        entity->getAnimstate()->footYaw = build_server_abs_yaw(entity, entity->eyeAngles().y + max_rotation * static_cast<float>(index));
        break;
    case 1: //reverse
        entity->getAnimstate()->footYaw = build_server_abs_yaw(entity, entity->eyeAngles().y + max_rotation * static_cast<float>(-index));
        break;
    case 2: //middle
        entity->getAnimstate()->footYaw = build_server_abs_yaw(entity, entity->eyeAngles().y);
        break;
    default: break;
    }

}

bool IsAdjustingBalance(const Animations::Players& player, Entity* entity)
{
    for (int i = 0; i < 15; i++)
    {
        const int activity = entity->sequence();
        if (activity == 979)
        {
            return true;
        }
    }
    return false;
}

bool is_breaking_lby(const Animations::Players& player, Entity* entity, AnimationLayer cur_layer, AnimationLayer prev_layer)
{
    if (IsAdjustingBalance(player, entity))
    {
        if ((prev_layer.cycle != cur_layer.cycle) || cur_layer.weight == 1.f)
        {
            return true;
        }
        else if (cur_layer.cycle == 0.f && (prev_layer.cycle > 0.92f && cur_layer.cycle > 0.92f))
        {
            return true;
        }
    }

    return false;
}
float Resolver::resolve_shot(const Animations::Players& player, Entity* entity) {
    /* fix unrestricted shot */
    if (DesyncDetect(entity) == false)
        return 0;

    const float fl_pseudo_fire_yaw = Helpers::angleNormalize(Helpers::angleDiff(localPlayer->origin().y, player.matrix[8].origin().y));
    if (is_breaking_lby(player, entity, player.layers[3], player.oldlayers[3])) {
        const float fl_left_fire_yaw_delta = fabsf(Helpers::angleNormalize(fl_pseudo_fire_yaw - (entity->eyeAngles().y + 58.f)));
        const float fl_right_fire_yaw_delta = fabsf(Helpers::angleNormalize(fl_pseudo_fire_yaw - (entity->eyeAngles().y - 58.f)));
        return fl_left_fire_yaw_delta > fl_right_fire_yaw_delta ? -58.f : 58.f;
    }
    const float fl_left_fire_yaw_delta = fabsf(Helpers::angleNormalize(fl_pseudo_fire_yaw - (entity->eyeAngles().y + 28.f)));
    const float fl_right_fire_yaw_delta = fabsf(Helpers::angleNormalize(fl_pseudo_fire_yaw - (entity->eyeAngles().y - 28.f)));

    return fl_left_fire_yaw_delta > fl_right_fire_yaw_delta ? -28.f : 28.f;
}

void Resolver::setup_detect(Animations::Players& player, Entity* entity) {

    // detect if player is using maximum desync.
    if (is_breaking_lby(player, entity, player.layers[3], player.oldlayers[3]))
    {
        player.extended = true;
    }
    /* calling detect side */
    detect_side(entity, &player.side);
    const int side = player.side;
    /* brute-forcing vars */
    float resolve_value = 50.f;
    static float brute = 0.f;
    const float fl_max_rotation = entity->getMaxDesyncAngle();
    const float fl_eye_yaw = entity->getAnimstate()->eyeYaw;
    const bool fl_forward = fabsf(Helpers::angleNormalize(get_angle(entity) - get_forward_yaw(entity))) < 90.f;
    const int fl_shots = player.misses;

    /* clamp angle */
    if (fl_max_rotation < resolve_value) {
        resolve_value = fl_max_rotation;
    }

    /* detect if entity is using max desync angle */
    if (player.extended) {
        resolve_value = fl_max_rotation;
    }

    const float perfect_resolve_yaw = resolve_value;

    /* setup brute-forcing */
    if (fl_shots == 0) {
        brute = perfect_resolve_yaw * static_cast<float>(fl_forward ? -side : side);
    }
    else {
        switch (fl_shots % 3) {
        case 0: {
            brute = perfect_resolve_yaw * static_cast<float>(fl_forward ? -side : side);
        } break;
        case 1: {
            brute = perfect_resolve_yaw * static_cast<float>(fl_forward ? side : -side);
        } break;
        case 2: {
            brute = 0;
        } break;
        default: break;
        }
    }

    /* fix goal feet yaw */
    entity->getAnimstate()->footYaw = fl_eye_yaw + brute;
}

Vector calc_angle(const Vector source, const Vector entity_pos) {
    const Vector delta{ source.x - entity_pos.x, source.y - entity_pos.y, source.z - entity_pos.z };
    const auto& [x, y, z] = cmd->viewangles;
    Vector angles{ Helpers::rad2deg(atan(delta.z / hypot(delta.x, delta.y))) - x, Helpers::rad2deg(atan(delta.y / delta.x)) - y, 0.f };
    if (delta.x >= 0.f)
        angles.y += 180;
    return angles;
}

void Resolver::processMissedShots() noexcept
{
    if (!resolver)
    {
        snapshots.clear();
        return;
    }

    if (!localPlayer)
    {
        snapshots.clear();
        return;
    }

    if (snapshots.empty())
        return;

    if (snapshots.front().time == -1) //Didnt get data yet
        return;

    auto snapshot = snapshots.front();
    snapshots.pop_front(); //got the info no need for this
    const auto& time = localPlayer->isAlive() ? localPlayer->tickBase() * memory->globalVars->intervalPerTick : memory->globalVars->currenttime;
    if (fabs(time - snapshot.time) > 1.f)
    {
        if (snapshot.gotImpact)
            Logger::addLog("Missed shot due to ping");
        else
            Logger::addLog("Missed shot due to server rejection");
        snapshots.clear();
        return;
    }
    if (!snapshot.player.gotMatrix)
        return;

    const auto entity = interfaces->entityList->getEntity(snapshot.playerIndex);
    if (!entity)
        return;

    const Model* model = snapshot.model;
    if (!model)
        return;

    StudioHdr* hdr = interfaces->modelInfo->getStudioModel(model);
    if (!hdr)
        return;

    StudioHitboxSet* set = hdr->getHitboxSet(0);
    if (!set)
        return;

    const auto angle = hitscan::calculateRelativeAngle(snapshot.eyePosition, snapshot.bulletImpact, Vector{ });
    const auto end = snapshot.bulletImpact + Vector::fromAngle(angle) * 2000.f;

    const auto matrix = snapshot.backtrackRecord <= -1 ? snapshot.player.matrix.data() : snapshot.player.backtrackRecords.at(snapshot.backtrackRecord).matrix;

    bool resolverMissed = false;
    for (int hitbox = 0; hitbox < Hitboxes::Max; hitbox++)
    {
        if (hitscan::hitboxIntersection(matrix, hitbox, set, snapshot.eyePosition, end))
        {
            resolverMissed = true;
            std::string missed = std::string("Missed shot on ") + entity->getPlayerName() + std::string(" due to resolver");
            std::string missedBT = std::string("Missed shot on ") + entity->getPlayerName() + std::string(" due to invalid backtrack tick [") + std::to_string(snapshot.backtrackRecord) + "]";
            std::string missedPred = std::string("Missed shot on ") + entity->getPlayerName() + std::string(" due to prediction error");
            std::string missedJitter = std::string("Missed shot on ") + entity->getPlayerName() + std::string(" due to jitter");
            if (snapshot.backtrackRecord == 1 && config->backtrack.enabled)
                Logger::addLog(missedJitter);
            else if (snapshot.backtrackRecord > 1 && config->backtrack.enabled)
                Logger::addLog(missedBT);
            else
                Logger::addLog(missed);
            Animations::setPlayer(snapshot.playerIndex)->misses++;
            break;
        }
    }
    if (!resolverMissed)
        Logger::addLog(std::string("Missed shot due to spread"));
}

void Resolver::runPreUpdate(Animations::Players player, Entity* entity) noexcept
{
    if (!resolver)
        return;

    const auto misses = player.misses;
    if (!entity || !entity->isAlive())
        return;

    if (!entity->isDormant())
        return;

    if (player.chokedPackets <= 0)
        return;

    if (snapshots.empty())
        return;

    auto& [snapshot_player, model, eyePosition, bulletImpact, gotImpact, time, playerIndex, backtrackRecord] = snapshots.front();
    setup_detect(player, entity);
    resolve_entity(player, entity);
}

void Resolver::runPostUpdate(Animations::Players player, Entity* entity) noexcept
{
    if (!resolver)
        return;

    const auto misses = player.misses;
    if (!entity || !entity->isAlive())
        return;

    if (!entity->isDormant())
        return;

    if (player.chokedPackets <= 0)
        return;

    if (snapshots.empty())
        return;

    //auto& [snapshot_player, model, eyePosition, bulletImpact, gotImpact, time, playerIndex, backtrackRecord] = snapshots.front();
    setup_detect(player, entity);
    resolve_entity(player, entity);
}

void Resolver::updateEventListeners(bool forceRemove) noexcept
{
    class ImpactEventListener : public GameEventListener {
    public:
        void fireGameEvent(GameEvent* event) {
            getEvent(event);
        }
    };

    static ImpactEventListener listener[4];
    static bool listenerRegistered = false;

    if (resolver && !listenerRegistered) {
        interfaces->gameEventManager->addListener(&listener[0], "bullet_impact");
        interfaces->gameEventManager->addListener(&listener[1], "player_hurt");
        interfaces->gameEventManager->addListener(&listener[2], "round_start");
        interfaces->gameEventManager->addListener(&listener[3], "weapon_fire");
        listenerRegistered = true;
    }

    else if ((!resolver || forceRemove) && listenerRegistered) {
        interfaces->gameEventManager->removeListener(&listener[0]);
        interfaces->gameEventManager->removeListener(&listener[1]);
        interfaces->gameEventManager->removeListener(&listener[2]);
        interfaces->gameEventManager->removeListener(&listener[3]);
        listenerRegistered = false;
    }
}