#include "hitscan.h"
#include "Animations.h"
#include "Resolver.h"
#include <DirectXMath.h>
#include <algorithm>
#include "../Logger.h"
#include "Backtrack.h"
#include "../SDK/GameEvent.h"
#include "../Vector2D.hpp"

std::deque<Resolver::SnapShot> snapshots;

bool resolver = true;

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

	if (player.simulationTime == playerSimulationTime)
	{
		snapshots.push_back(snapshot);
		return;
	}

	for (int i = 0; i < static_cast<int>(player.backtrackRecords.size()); i++)
	{
		if (player.backtrackRecords.at(i).simulationTime == playerSimulationTime)
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

		const auto index = interfaces->engine->getPlayerForUserID(playerId);
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

	if (snapshots.front().time == -1) //Didnt get data yet
		return;

	auto snapshot = snapshots.front();
	snapshots.pop_front(); //got the info no need for this
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

	const auto angle = AimbotFunction::calculateRelativeAngle(snapshot.eyePosition, snapshot.bulletImpact, Vector{ });
	const auto end = snapshot.bulletImpact + Vector::fromAngle(angle) * 2000.f;

	const auto matrix = snapshot.backtrackRecord == -1 ? snapshot.player.matrix.data() : snapshot.player.backtrackRecords.at(snapshot.backtrackRecord).matrix;

	bool resolverMissed = false;

	for (int hitbox = 0; hitbox < Hitboxes::Max; hitbox++)
	{
		if (AimbotFunction::hitboxIntersection(matrix, hitbox, set, snapshot.eyePosition, end))
		{
			resolverMissed = true;
			std::string missed = "Missed " + entity->getPlayerName() + " due to resolver";
			if (snapshot.backtrackRecord > 0)
				missed += "BT[" + std::to_string(snapshot.backtrackRecord) + "]";
			Logger::addLog(missed);
			Animations::setPlayer(snapshot.playerIndex)->misses++;
			break;
		}
	}
	if (!resolverMissed)
		Logger::addLog("Missed due to spread");
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

void Resolver::runPreUpdate(Animations::Players player, Entity* entity) noexcept
{
	if (!resolver)
		return;

	const auto misses = player.misses;
	if (!entity || !entity->isAlive())
		return;

	if (player.chokedPackets <= 0)
		return;
}

void Resolver::runPostUpdate(Animations::Players player, Entity* entity) noexcept
{
	if (!resolver)
		return;

	const auto misses = player.misses;
	if (!entity || !entity->isAlive())
		return;

	if (player.chokedPackets <= 0)
		return;
}

float build_server_abs_yaw(Entity* entity, const float angle)
{

	Vector velocity = entity->velocity();
	const auto& anim_state = entity->getAnimstate();
	const float m_fl_eye_yaw = angle;
	float m_fl_goal_feet_yaw = 0.f;

	const float eye_feet_delta = Helpers::angleDiff(m_fl_eye_yaw, m_fl_goal_feet_yaw);

	static auto get_smoothed_velocity = [](const float min_delta, const Vector a, const Vector b) {
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
	const float duck_additional = std::clamp(entity->duckAmount() + anim_state->duckAdditional, 0.0f, 1.0f);
	const float duck_amount = anim_state->animDuckAmount;
	const float choked_time = m_fl_choked_time * 6.0f;
	float v28;

	// clamp
	if (duck_additional - duck_amount <= choked_time)
		if (duck_additional - duck_amount >= -choked_time)
			v28 = duck_additional;
		else
			v28 = duck_amount - choked_time;
	else
		v28 = duck_amount + choked_time;

	const float fl_duck_amount = std::clamp(v28, 0.0f, 1.0f);

	const Vector animation_velocity = get_smoothed_velocity(m_fl_choked_time * 2000.0f, velocity, entity->velocity());
	const float speed = std::fminf(animation_velocity.length(), 260.0f);

	float fl_max_movement_speed = 260.0f;

	if (Entity* p_weapon = entity->getActiveWeapon(); p_weapon && p_weapon->getWeaponData())
		fl_max_movement_speed = std::fmaxf(p_weapon->getWeaponData()->maxSpeedAlt, 0.001f);

	float fl_running_speed = speed / (fl_max_movement_speed * 0.520f);

	fl_running_speed = std::clamp(fl_running_speed, 0.0f, 1.0f);

	float fl_yaw_modifier = (anim_state->walkToRunTransition * -0.30000001f - 0.19999999f) * fl_running_speed + 1.0f;

	if (fl_duck_amount > 0.0f)
	{
		// float fl_ducking_speed = std::clamp(fl_ducking_speed, 0.0f, 1.0f);
		fl_yaw_modifier += fl_duck_amount /* fl_ducking_speed */ * (0.5f - fl_yaw_modifier);
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
	{
		m_fl_goal_feet_yaw = m_fl_eye_yaw - fabs(fl_max_yaw_modifier);
	}

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
			entity->lby(),
			m_fl_goal_feet_yaw,
			m_fl_choked_time * 100.0f);
	}

	return m_fl_goal_feet_yaw;
}

void Resolver::resolve_entity(const Animations::Players& player, Entity* entity) {
	// get the players max rotation.
	float max_rotation = entity->getMaxDesyncAngle();
	int index = 0;
	const float eye_yaw = entity->getAnimstate()->eyeYaw;
	if (const bool extended = player.extended; !extended && fabs(max_rotation) > 60.f)
	{
		max_rotation = max_rotation / 1.8f;
	}

	// resolve shooting players separately.
	if (player.shot) {
		entity->getAnimstate()->footYaw = eye_yaw + resolve_shot(player, entity);
		return;
	}
	if (entity->velocity().length2D() <= 0.1f) {
		const float angle_difference = Helpers::angleDiff(eye_yaw, entity->getAnimstate()->footYaw);
		index = 2 * angle_difference <= 0.0f ? 1 : -1;
	}
	else
	{
		if (!static_cast<int>(player.layers[12].weight * 1000.f) && entity->velocity().length2D() > 0.1f) {
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

float Resolver::resolve_shot(const Animations::Players& player, Entity* entity) {
	/* fix unrestricted shot */
	const float fl_pseudo_fire_yaw = Helpers::angleNormalize(Helpers::angleDiff(localPlayer->origin().y, player.matrix[8].origin().y));
	if (player.extended) {
		const float fl_left_fire_yaw_delta = fabsf(Helpers::angleNormalize(fl_pseudo_fire_yaw - (entity->eyeAngles().y + 58.f)));
		const float fl_right_fire_yaw_delta = fabsf(Helpers::angleNormalize(fl_pseudo_fire_yaw - (entity->eyeAngles().y - 58.f)));
		return fl_left_fire_yaw_delta > fl_right_fire_yaw_delta ? -58.f : 58.f;
	}
	const float fl_left_fire_yaw_delta = fabsf(Helpers::angleNormalize(fl_pseudo_fire_yaw - (entity->eyeAngles().y + 28.f)));
	const float fl_right_fire_yaw_delta = fabsf(Helpers::angleNormalize(fl_pseudo_fire_yaw - (entity->eyeAngles().y - 28.f)));

	return fl_left_fire_yaw_delta > fl_right_fire_yaw_delta ? -28.f : 28.f;
}


void Resolver::detect_side(Entity* entity, int* side) {
	/* externals */
	Vector forward{};
	Vector right{};
	Vector up{};
	Trace tr;
	Helpers::angleVectors(Vector(0, get_backward_side(entity), 0), &forward, &right, &up);
	/* filtering */

	const Vector src_3d = entity->getEyePosition();
	const Vector dst_3d = src_3d + forward * 384;

	/* back engine tracers */
	// interfaces->engineTrace->traceRay({ src_3d, dst_3d }, 0x200400B, { entity }, tr);
	// float back_two = (tr.endpos - tr.startpos).length();

	/* right engine tracers */
	interfaces->engineTrace->traceRay(Ray(src_3d + right * 35, dst_3d + right * 35), 0x200400B, { entity }, tr);
	const float right_two = (tr.endpos - tr.startpos).length();

	/* left engine tracers */
	interfaces->engineTrace->traceRay(Ray(src_3d - right * 35, dst_3d - right * 35), 0x200400B, { entity }, tr);

	/* fix side */
	if (const float left_two = (tr.endpos - tr.startpos).length(); left_two > right_two) {
		*side = -1;
	}
	else if (right_two > left_two) {
		*side = 1;
	}
	else
		*side = 0;
}



void Resolver::updateEventListeners(bool forceRemove) noexcept
{
	class ImpactEventListener : public GameEventListener {
	public:
		void fireGameEvent(GameEvent* event) {
			getEvent(event);
		}
	};

	static ImpactEventListener listener;
	static bool listenerRegistered = false;

	if (resolver && !listenerRegistered) {
		interfaces->gameEventManager->addListener(&listener, "bullet_impact");
		listenerRegistered = true;
	}
	else if ((!resolver || forceRemove) && listenerRegistered) {
		interfaces->gameEventManager->removeListener(&listener);
		listenerRegistered = false;
	}
}