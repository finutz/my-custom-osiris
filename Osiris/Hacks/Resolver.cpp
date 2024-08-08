#include "hitscan.h"
#include "Ragebot.h"
#include "Backtrack.h"
#include "Animations.h"
#include "Resolver.h"

#include "../Logger.h"

#include "../SDK/GameEvent.h"
#include "../Vector2D.hpp"
#include "../GameData.h"
#include <DirectXMath.h>
#include <algorithm>

UserCmd* cmd;

#define TICK_INTERVAL            ( memory->globalVars->currenttime )
#define TIME_TO_TICKS( dt )        ( (int)( 0.5f + (float)(dt) / TICK_INTERVAL ) )
#define TICKS_TO_TIME( t )        ( TICK_INTERVAL *( t ) )
#define M_RADPI 57.295779513082f

std::deque<Resolver::SnapShot> snapshots;
static std::array<Animations::Players, 65> players{};
std::string log_rotation_side;

bool resolver = true;
bool occlusion = false;

void Resolver::CmdGrabber(UserCmd* cmd1)
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

    switch (fnv::hashRuntime(event->getName()))
    {
    case fnv::hash("round_start"):
    {
        // Reset all
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
        // Reset player
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
        if (hitgroup < HitGroup::Generic || hitgroup > HitGroup::RightLeg)
            break;

        snapshots.pop_front(); // Hit somebody so dont calculate
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

float build_server_abs_yaw(Entity* m_player, float angle)
{
    Vector velocity = m_player->velocity();
    const auto& anim_state = m_player->getAnimstate();
    const float m_fl_eye_yaw = angle;
    float m_fl_goal_feet_yaw = 0.f;

    const float eye_feet_delta = Helpers::angleDiff(m_fl_eye_yaw, m_fl_goal_feet_yaw);

    static auto get_smoothed_velocity = [](const float min_delta, const Vector a, const Vector b)
        {
            const Vector delta = a - b;
            const float delta_length = delta.length();

            if (delta_length <= min_delta)
            {
                if (-min_delta <= delta_length)
                    return a;
                const float i_radius = 1.0f / (delta_length + FLT_EPSILON);
                return b - delta * i_radius * min_delta;
            }

            const float i_radius = 1.0f / (delta_length + FLT_EPSILON);
            return b + delta * i_radius * min_delta;
        };

    if (const float spd = velocity.squareLength(); spd > std::powf(1.2f * 260.0f, 2.f))
    {
        const Vector velocity_normalized = velocity.normalized();
        velocity = velocity_normalized * (1.2f * 260.0f);
    }

    const float m_fl_choked_time = anim_state->lastUpdateTime;
    const float duck_additional = std::clamp(m_player->duckAmount() + anim_state->duckAdditional, 0.0f, 1.0f);
    const float duck_amount = anim_state->animDuckAmount;
    const float choked_time = m_fl_choked_time * 6.0f;
    float v28;

    // clamp
    if (duck_additional - duck_amount <= choked_time)
    {
        if (duck_additional - duck_amount >= -choked_time)
            v28 = duck_additional;
        else
            v28 = duck_amount - choked_time;
    }
    else
        v28 = duck_amount + choked_time;

    const float fl_duck_amount = std::clamp(v28, 0.0f, 1.0f);

    const Vector animation_velocity = get_smoothed_velocity(m_fl_choked_time * 2000.0f, velocity, m_player->velocity());
    const float speed = std::fminf(animation_velocity.length(), 260.0f);

    float fl_max_movement_speed = 260.0f;

    if (Entity* p_weapon = m_player->getActiveWeapon(); p_weapon && p_weapon->getWeaponData())
        fl_max_movement_speed = std::fmaxf(p_weapon->getWeaponData()->maxSpeed, 0.001f);

    float fl_running_speed = speed / (fl_max_movement_speed * 0.520f);
    float fl_ducking_speed = speed / (fl_max_movement_speed * 0.340f);
    fl_running_speed = std::clamp(fl_running_speed, 0.0f, 1.0f);

    float fl_yaw_modifier = (anim_state->walkToRunTransition * -0.3f - 0.2f) * fl_running_speed + 1.0f;

    if (fl_duck_amount > 0.0f)
    {
        float fl_ducking_speed2 = std::clamp(fl_ducking_speed, 0.0f, 1.0f);
        fl_yaw_modifier += (fl_ducking_speed2 * fl_duck_amount) * (0.5f - fl_yaw_modifier);
    }

    constexpr float v60 = -58.f;
    constexpr float v61 = 58.f;

    const float fl_min_yaw_modifier = v60 * fl_yaw_modifier;

    if (const float fl_max_yaw_modifier = v61 * fl_yaw_modifier; eye_feet_delta <= fl_max_yaw_modifier)
    {
        if (fl_min_yaw_modifier > eye_feet_delta)
            m_fl_goal_feet_yaw = fabs(fl_min_yaw_modifier) + m_fl_eye_yaw;
    }
    else
        m_fl_goal_feet_yaw = m_fl_eye_yaw - fabs(fl_max_yaw_modifier);

    Helpers::normalizeYaw(m_fl_goal_feet_yaw);

    if (speed > 0.1f || fabs(velocity.z) > 100.0f)
    {
        m_fl_goal_feet_yaw = Helpers::approachAngle(
            m_fl_eye_yaw,
            m_fl_goal_feet_yaw,
            (anim_state->walkToRunTransition * 20.0f + 30.0f)
            * m_fl_choked_time);
    }
    else
    {
        m_fl_goal_feet_yaw = Helpers::approachAngle(
            m_player->lby(),
            m_fl_goal_feet_yaw,
            m_fl_choked_time * 100.0f);
    }

    return m_fl_goal_feet_yaw;
}


float build_move_yaw(Entity* entity)
{
    /* m_PlayerAnimationStateCSGO( )->m_flMoveYawIdeal */
    float moveYawIdeal = Animations::getFootYaw();

    /* Rebuild m_flMoveYawIdeal */
    float velocityLengthXY = entity->velocity().length2D();
    if (velocityLengthXY > 0 && entity->flags() & FL_ONGROUND)
    {
        /* convert horizontal velocity vec to angular yaw*/
        float rawYawIdeal = (atan2(-entity->velocity()[1], -entity->velocity()[0]) * M_RADPI);
        if (rawYawIdeal < 0.0f)
            rawYawIdeal += 180.0f;

        moveYawIdeal = build_server_abs_yaw(entity, Helpers::angleNormalize(Helpers::angleDiff(rawYawIdeal, Animations::getFootYaw())));
    }

    /* Return m_flMoveYaw */
    return moveYawIdeal;
}

float resolve_move_yaw(Animations::Players player, Animations::Players prev_player, Entity* entity)
{

    float footYaw = Animations::getFootYaw();


    if (const auto eye_diff = Helpers::angleNormalize(Helpers::angleDiff(entity->eyeAngles().y, footYaw)); eye_diff > entity->getMaxDesyncAngle())
        player.rotation_side = ROTATION_SIDE_RIGHT;
    else if (entity->getMaxDesyncAngle() > eye_diff)
        player.rotation_side = ROTATION_SIDE_LEFT;
    else
        player.rotation_side = ROTATION_SIDE_CENTER;


    footYaw = Helpers::approachAngle(entity->eyeAngles().y, footYaw, (entity->getAnimstate()->walkToRunTransition * 20.0f + 30.0f) * entity->getAnimstate()->lastUpdateIncrement);


    player.rotation_side = player.last_side;
    return footYaw;
}



void Resolver::resolve_entity(Animations::Players player, Animations::Players prev_player, Entity* entity)
{
    if (entity->isBot())
        return;
    /* vars */
    float footYaw = Animations::getFootYaw();

    /* resolve shooting players separately. */
    if (player.shot)
    {
        if (!entity || !entity->isAlive())
            return;

        if (entity->getAnimstate()->velocityLengthXY > 0.1f && fabsf(entity->getAnimstate()->velocityLengthZ) > 100.0f)
        {

            player.anim_resolved = false;

            int m_side = 0;
            bool m_can_animate;

            /* animlayers.*/
            /* check if we have a previous record stored we can compare to, if we are currently running animations related to body lean and check if the movement speed (movement layer playback rate) of both records match. */
            if (!(player.layers[ANIMATION_LAYER_MOVEMENT_MOVE].weight * 1000.0f))
            {
                if (player.layers[ANIMATION_LAYER_MOVEMENT_MOVE].weight * 1000.0f == prev_player.layers[ANIMATION_LAYER_MOVEMENT_MOVE].weight * 1000.0f)
                {
                    /* compare networked layer to processed layer w / m_flFootYaw lerped with positive delta. */
                    const float delta_left = player.layers[ANIMATION_LAYER_MOVEMENT_MOVE].playbackRate;

                    /* compare networked layer to processed layer. */
                    const float delta_center = player.layers[ANIMATION_LAYER_MOVEMENT_MOVE].playbackRate;

                    /* compare networked layer to processed layer. quad */
                    const float delta_quad = player.layers[ANIMATION_LAYER_MOVEMENT_MOVE].playbackRate;

                    /* compare networked layer to processed layer w/ m_flFootYaw lerped with negative delta. */
                    const float delta_right = player.layers[ANIMATION_LAYER_MOVEMENT_MOVE].playbackRate;

                    bool m_finally_active{};
                    if (!(delta_quad * 1000.f))
                        m_finally_active = true;

                    m_side = 0;

                    float last_delta = abs(player.layers[ANIMATION_LAYER_MOVEMENT_MOVE].playbackRate - delta_quad);

                    if (delta_center * 1000.f || delta_quad < delta_center)
                        m_can_animate = m_finally_active;
                    else
                    {
                        player.rotation_mode = ROTATION_MOVE; /* record is mode resolve. */
                        m_can_animate = true;
                        m_side = ROTATION_SIDE_CENTER; /* record is desync. */
                        last_delta = delta_center;
                    }

                    if (!player.layers[ANIMATION_LAYER_MOVEMENT_MOVE].weight)
                        player.rotation_mode = ROTATION_MOVE;
                    m_can_animate = true;
                    m_side = ROTATION_SIDE_LEFT;
                    last_delta = delta_center;

                    if (!(delta_left * 1000.f) && last_delta >= delta_left)
                    {
                        m_can_animate = true;
                        m_side = ROTATION_SIDE_RIGHT; /* record is desync. */
                        last_delta = delta_left;
                    }

                    if (!(delta_right * 1000.f) && last_delta >= delta_right)
                    {
                        m_can_animate = true;
                        m_side = ROTATION_SIDE_LEFT; /* record is desync. */
                        return;
                    }
                }
                else
                {
                    m_can_animate = false;
                }
            }

            entity->getAnimstate()->setupVelocity(); // setup vel...

            footYaw = resolve_shot(player, prev_player, entity);
            footYaw = resolve_move_yaw(player, prev_player, entity);
            footYaw = build_move_yaw(entity);

            /* last hit. */
            player.anim_resolved = m_can_animate;
            player.rotation_side = m_side;
        }
    }

    return;
}









float Resolver::resolve_shot(const Animations::Players& player, Animations::Players prev_player, Entity* entity)
{
    const float fl_pseudo_fire_yaw = Helpers::angleNormalize(Helpers::angleDiff(localPlayer->origin().y, player.matrix[8].origin().y));

    float max_rotation = entity->lby();
    float resolve_value = entity->getMaxDesyncAngle();
    const int m_missed_shots = player.misses;

    float yaw_resolve = 0.f;

    /* miss & dsy val */
    if (m_missed_shots > 0)
        resolve_value *= -1;

    /* side layers */
    if (player.rotation_side != 0)
        resolve_value *= player.rotation_side; // Adjust the desync based on the rot side

    /* clamp angle */
    if (max_rotation < resolve_value)
        resolve_value = max_rotation;

    /* detect if entity is using max desync angle */
    yaw_resolve = resolve_value = max_rotation;

    /* vars side */
    const float fl_left_fire_yaw_delta = fabsf(Helpers::angleNormalize(fl_pseudo_fire_yaw - (entity->eyeAngles().y + ROTATION_SIDE_LEFT)));
    const float fl_right_fire_yaw_delta = fabsf(Helpers::angleNormalize(fl_pseudo_fire_yaw - (entity->eyeAngles().y - ROTATION_SIDE_RIGHT)));

    /* vars yaw */
    const auto side_res = player.rotation_side ? fl_left_fire_yaw_delta : fl_right_fire_yaw_delta;
    const float is_jitter = abs(fl_pseudo_fire_yaw - entity->eyeAngles().y) > entity->getMaxDesyncAngle();
    float velocityLengthXY = entity->velocity().length2D();
    const float stand = abs(velocityLengthXY < 260.0f);
    const float move = (!player.layers[ANIMATION_LAYER_MOVEMENT_MOVE].weight, velocityLengthXY > 260.0f);
    if (velocityLengthXY > 0 && entity->flags() & FL_ONGROUND)
    {
        // convert horizontal velocity vec to angular yaw
        float flRawYawIdeal = (velocityLengthXY > 0 && entity->flags() * M_RADPI);
        if (flRawYawIdeal < 0.0f)
            flRawYawIdeal += 360.0f;

        const auto m_flMoveYawIdeal = Helpers::angleNormalize(Helpers::angleDiff(flRawYawIdeal, Animations::getFootYaw()));
    }


    if (player.layers[ANIMATION_LAYER_ADJUST].sequence == 979, entity->eyeAngles().y > 119.f)
        if (player.layers[ANIMATION_LAYER_ADJUST].sequence == 0, entity->eyeAngles().y > 0.f)
        {
            const auto m_side = 2 * int(Helpers::angleNormalize(entity->eyeAngles().y) >= 0.f) - 1;
            const auto m_resolve_value = 60.f;
        }
    /* brute */
    if (is_jitter) /* jitter brute */
    {
        switch (m_missed_shots % 3)
        {
        case ROTATION_SIDE_LEFT:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y + yaw_resolve));
            break;
        case ROTATION_SIDE_CENTER:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y));
            break;
        case ROTATION_SIDE_RIGHT:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y - yaw_resolve));
            break;
        }
    }
    else /* last brute */
    {
        switch (m_missed_shots % 5)
        {
        case ROTATION_SIDE_CENTER:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y));
            break;
        case ROTATION_SIDE_LEFT:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y + 58.f));
            break;
        case ROTATION_SIDE_RIGHT:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y - 58.f));
            break;
        case ROTATION_SIDE_LOW_LEFT:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y + 30.f));
            break;
        case ROTATION_SIDE_LOW_RIGHT:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y - 30.f));
            break;
        }
    }

    if (move) /* move fix */
    {
        auto footYaw = resolve_move_yaw(player, prev_player, entity);
        return footYaw;

    }

    bool resolverMissed = false;
    if (!resolverMissed)
    {

        switch (m_missed_shots % 3)
        {
        case ROTATION_SIDE_CENTER:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y));
            break;
        case ROTATION_SIDE_LEFT:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y + 45.f));
            break;
        case ROTATION_SIDE_RIGHT:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y - 45.f));
            break;
        }


    }

    if (stand) /* stand fix */
    {


        const auto feet_delta = Helpers::angleNormalize(Helpers::angleDiff(Helpers::angleNormalize(entity->getAnimstate()->timeToAlignLowerBody), Helpers::angleNormalize((entity->eyeAngles().y))));

        const auto eye_diff = Helpers::angleDiff(entity->eyeAngles().y, (entity->getAnimstate()->eyeYaw));


        auto stopped_moving = (player.layers[ANIMATION_LAYER_ADJUST].sequence) == ACT_CSGO_IDLE_ADJUST_STOPPEDMOVING;


        auto balance_adjust = (player.layers[ANIMATION_LAYER_ADJUST].sequence) == ACT_CSGO_IDLE_TURN_BALANCEADJUST;


        bool stopped_moving_this_frame = false;
        if (entity->getAnimstate()->velocityLengthXY <= 0.1f)
        {
            auto  m_duration_moving = ROTATION_MOVE;
            auto m_duration_still = entity->getAnimstate()->lastUpdateIncrement;

            auto stopped_moving_this_frame = entity->getAnimstate()->velocityLengthXY < 0;
            m_duration_moving == 0.0f;
            m_duration_still += (entity->getAnimstate()->lastUpdateIncrement);

        }


        if (entity->getAnimstate()->velocityLengthXY == 0.0f && entity->getAnimstate()->landAnimMultiplier == 0.0f && entity->getAnimstate()->lastUpdateIncrement > 0.0f)
        {
            const auto current_feet_yaw = entity->getAnimstate()->eyeYaw;
            const auto goal_feet_yaw = entity->getAnimstate()->eyeYaw;
            auto eye_delta = current_feet_yaw - goal_feet_yaw;

            if (goal_feet_yaw < current_feet_yaw)
            {
                if (eye_delta >= 180.0f)
                    eye_delta -= 360.0f;
            }
            else if (eye_delta <= -180.0f)
                eye_delta += 360.0f;

            if (eye_delta / entity->getAnimstate()->lastUpdateIncrement > 120.f)
            {
                (player.layers[ANIMATION_LAYER_ADJUST].sequence) == 0.0f;
                (player.layers[ANIMATION_LAYER_ADJUST].sequence) == 0.0f;

            }
        }

    }
    else /* last brute */
    {
        switch (m_missed_shots % 5)
        {
        case ROTATION_SIDE_CENTER:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y));
            break;
        case ROTATION_SIDE_LEFT:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y + 58.f));
            break;
        case ROTATION_SIDE_RIGHT:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y - 58.f));
            break;
        case ROTATION_SIDE_LOW_LEFT:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y + 30.f));
            break;
        case ROTATION_SIDE_LOW_RIGHT:
            build_server_abs_yaw(entity, Helpers::angleNormalize(entity->eyeAngles().y - 30.f));
            break;
        }
    }

}



void Resolver::runPreUpdate(Animations::Players player, Animations::Players prev_player, Entity* entity) noexcept
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

    
        resolve_entity(player, prev_player, entity);
}

void Resolver::runPostUpdate(Animations::Players player, Animations::Players prev_player, Entity* entity) noexcept
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

   
        resolve_entity(player, prev_player, entity);
}

void Resolver::processMissedShots() noexcept
{
    if (!resolver)
    {
        snapshots.clear();
        return;
    }

    if (!localPlayer || !localPlayer->isAlive())
    {
        snapshots.clear();
        return;
    }

    if (snapshots.empty())
        return;

    if (snapshots.front().time == -1) // Didnt get data yet
        return;

    auto snapshot = snapshots.front();
    snapshots.pop_front(); // got the info no need for this

    if (!snapshot.player.gotMatrix)
        return;

    const auto entity = interfaces->entityList->getEntity(snapshot.playerIndex);
    if (!entity)
        return;

    const auto angle = hitscan::calculateRelativeAngle(snapshot.eyePosition, snapshot.bulletImpact, Vector{ });
    const auto end = snapshot.bulletImpact + Vector::fromAngle(angle) * 2000.f;

    const auto matrix = snapshot.backtrackRecord <= -1 ? snapshot.player.matrix.data() : snapshot.player.backtrackRecords.at(snapshot.backtrackRecord).matrix;
    bool resolverMissed = false;

    for (int hitbox = 0; hitbox < Hitboxes::Max; hitbox++)
    {
        resolverMissed = true;

        std::string missed = std::string("missed shot on ") + entity->getPlayerName() + std::string(" due to resolver");
        std::string missedBT = std::string("missed shot on ") + entity->getPlayerName() + std::string(" due to invalid backtrack tick [") + std::to_string(snapshot.backtrackRecord) + "]";
        std::string missedPred = std::string("missed shot on ") + entity->getPlayerName() + std::string(" due to prediction error");
        std::string missedJitter = std::string("missed shot on ") + entity->getPlayerName() + std::string(" due to jitter resolver");

        if (snapshot.backtrackRecord > 1 && config->backtrack.enabled)
            Logger::addLog(missedBT);
        else
            Logger::addLog(missed);

        Animations::setPlayer(snapshot.playerIndex)->misses++;
        break;
    }

    if (!resolverMissed && snapshot.gotImpact)
        Logger::addLog(std::string("missed shot on ") + entity->getPlayerName() + std::string(" due to prediction error"));

    if (!resolverMissed)
        Logger::addLog(std::string("missed shot on ") + entity->getPlayerName() + std::string(" due to spread"));
}

void Resolver::updateEventListeners(bool forceRemove) noexcept
{
    class ImpactEventListener : public GameEventListener
    {
    public:
        void fireGameEvent(GameEvent* event)
        {
            getEvent(event);
        }
    };

    static ImpactEventListener listener[4];
    static bool listenerRegistered = false;

    if (resolver && !listenerRegistered)
    {
        interfaces->gameEventManager->addListener(&listener[0], "bullet_impact");
        interfaces->gameEventManager->addListener(&listener[1], "player_hurt");
        interfaces->gameEventManager->addListener(&listener[2], "round_start");
        interfaces->gameEventManager->addListener(&listener[3], "weapon_fire");
        listenerRegistered = true;
    }
    else if ((!resolver || forceRemove) && listenerRegistered)
    {
        interfaces->gameEventManager->removeListener(&listener[0]);
        interfaces->gameEventManager->removeListener(&listener[1]);
        interfaces->gameEventManager->removeListener(&listener[2]);
        interfaces->gameEventManager->removeListener(&listener[3]);
        listenerRegistered = false;
    }
}