/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <cstdlib>
#include <cstring>

#include "LosHandler.h"
#include "ModInfo.h"

#include "Sim/Units/Unit.h"
#include "Sim/Misc/TeamHandler.h"
#include "Map/ReadMap.h"
#include "System/Log/ILog.h"
#include "System/TimeProfiler.h"
#include "System/creg/STL_Deque.h"

using std::min;
using std::max;

CR_BIND(LosInstance, )
CR_BIND(CLosHandler, )
CR_BIND(CLosHandler::DelayedInstance, )

CR_REG_METADATA(LosInstance,(
	CR_IGNORED(losSquares),
	CR_MEMBER(losSize),
	CR_MEMBER(airLosSize),
	CR_MEMBER(refCount),
	CR_MEMBER(allyteam),
	CR_MEMBER(basePos),
	CR_MEMBER(baseSquare),
	CR_MEMBER(baseAirPos),
	CR_MEMBER(hashNum),
	CR_MEMBER(baseHeight),
	CR_MEMBER(toBeDeleted)
))

void CLosHandler::PostLoad()
{
	for (int a = 0; a < LOSHANDLER_MAGIC_PRIME; ++a) {
		for (LosInstance* li: instanceHash[a]) {
			if (li->refCount) {
				LosAdd(li);
			}
		}
	}
}

CR_REG_METADATA(CLosHandler,(
	CR_IGNORED(losMipLevel),
	CR_IGNORED(airMipLevel),
	CR_IGNORED(losDiv),
	CR_IGNORED(airDiv),
	CR_IGNORED(invLosDiv),
	CR_IGNORED(invAirDiv),
	CR_IGNORED(airSizeX),
	CR_IGNORED(airSizeY),
	CR_IGNORED(losSizeX),
	CR_IGNORED(losSizeY),
	CR_IGNORED(requireSonarUnderWater),

	CR_MEMBER(losAlgo),
	CR_MEMBER(losMaps),
	CR_MEMBER(airLosMaps),
	CR_MEMBER(instanceHash),
	CR_MEMBER(toBeDeleted),
	CR_MEMBER(delayQue),
	CR_POSTLOAD(PostLoad)
))

CR_REG_METADATA_SUB(CLosHandler,DelayedInstance, (
	CR_MEMBER(instance),
	CR_MEMBER(timeoutTime)))


//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////


CLosHandler* losHandler;


CLosHandler::CLosHandler() :
	losMaps(teamHandler->ActiveAllyTeams()),
	airLosMaps(teamHandler->ActiveAllyTeams()),
	// airAlgo(int2(airSizeX, airSizeY), -1e6f, 15, readMap->GetMIPHeightMapSynced(airMipLevel)),
	losMipLevel(modInfo.losMipLevel),
	airMipLevel(modInfo.airMipLevel),
	losDiv(SQUARE_SIZE * (1 << losMipLevel)),
	airDiv(SQUARE_SIZE * (1 << airMipLevel)),
	invLosDiv(1.0f / losDiv),
	invAirDiv(1.0f / airDiv),
	airSizeX(std::max(1, mapDims.mapx >> airMipLevel)),
	airSizeY(std::max(1, mapDims.mapy >> airMipLevel)),
	losSizeX(std::max(1, mapDims.mapx >> losMipLevel)),
	losSizeY(std::max(1, mapDims.mapy >> losMipLevel)),
	requireSonarUnderWater(modInfo.requireSonarUnderWater),
	losAlgo(int2(losSizeX, losSizeY), -1e6f, 15, readMap->GetMIPHeightMapSynced(losMipLevel))
{
	for (int a = 0; a < teamHandler->ActiveAllyTeams(); ++a) {
		losMaps[a].SetSize(losSizeX, losSizeY, true);
		airLosMaps[a].SetSize(airSizeX, airSizeY, false);
	}
}


CLosHandler::~CLosHandler()
{
	for (int a = 0; a < LOSHANDLER_MAGIC_PRIME; ++a) {
		for (LosInstance* li: instanceHash[a]) {
			delete li;
		}
	}

}


void CLosHandler::MoveUnit(CUnit* unit, bool redoCurrent)
{
	SCOPED_TIMER("LOSHandler::MoveUnit");

	// NOTE: under normal circumstances, this only gets called if a unit
	// has moved to a new map square since its last SlowUpdate cycle, so
	// any units that changed position between enabling and disabling of
	// globalLOS and *stopped moving* will still provide LOS at their old
	// square *after* it is disabled (until they start moving again)
	if (gs->globalLOS[unit->allyteam])
		return;
	if (unit->losRadius <= 0 && unit->airLosRadius <= 0)
		return;

	unit->lastLosUpdate = gs->frameNum;

	const float3& losPos = unit->midPos;
	const int allyteam = unit->allyteam;

	const int baseX = Round(losPos.x * invLosDiv);
	const int baseY = Round(losPos.z * invLosDiv);
	const int baseAirX = Round(losPos.x * invAirDiv);
	const int baseAirY = Round(losPos.z * invAirDiv);

	LosInstance* instance = NULL;
	if (redoCurrent) {
		if (!unit->los) {
			return;
		}
		instance = unit->los;
		CleanupInstance(instance);
		instance->losSquares.clear();
		instance->basePos.x = baseX;
		instance->basePos.y = baseY;
		instance->baseSquare = baseSquare; //this could be a problem if several units are sharing the same instance
		instance->baseAirPos.x = baseAirX;
		instance->baseAirPos.y = baseAirY;
	} else {
		if (unit->los && (unit->los->baseSquare == baseSquare)) {
			return;
		}

		FreeInstance(unit->los);
		const int hash = GetHashNum(unit);

		for (LosInstance* li: instanceHash[hash]) {
			if (li->baseSquare == baseSquare         &&
			    li->losSize    == unit->losRadius    &&
			    li->airLosSize == unit->airLosRadius &&
			    li->baseHeight == unit->losHeight    &&
			    li->allyteam   == allyteam) {
				AllocInstance(li);
				unit->los = li;
				return;
			}
		}

		instance = new LosInstance(
			unit->losRadius,
			unit->airLosRadius,
			allyteam,
			int2(baseX, baseY),
			baseSquare,
			int2(baseAirX, baseAirY),
			hash, unit->losHeight
		);

		instanceHash[hash].push_back(instance);
		unit->los = instance;
	}

	LosAdd(instance);
}


void CLosHandler::LosAdd(LosInstance* instance)
{
	assert(instance);
	assert(teamHandler->IsValidAllyTeam(instance->allyteam));

	losAlgo.LosAdd(instance->basePos, instance->losSize, instance->baseHeight, instance->losSquares);

	if (instance->losSize > 0) { losMaps[instance->allyteam].AddMapSquares(instance->losSquares, instance->allyteam, 1); }
	if (instance->airLosSize > 0) { airLosMaps[instance->allyteam].AddMapArea(instance->baseAirPos, instance->allyteam, instance->airLosSize, 1); }
}


void CLosHandler::FreeInstance(LosInstance* instance)
{
	if (instance == nullptr)
		return;

	instance->refCount--;

	if (instance->refCount > 0) {
		return;
	}

	CleanupInstance(instance);

	if (!instance->toBeDeleted) {
		instance->toBeDeleted = true;
		toBeDeleted.push_back(instance);
	}

	if (instance->hashNum >= LOSHANDLER_MAGIC_PRIME || instance->hashNum < 0) {
		LOG_L(L_WARNING,
				"[LosHandler::FreeInstance][1] bad LOS-instance hash (%d)",
				instance->hashNum);
	}

	if (toBeDeleted.size() > 500) {
		LosInstance* i = toBeDeleted.front();
		toBeDeleted.pop_front();

		if (i->hashNum >= LOSHANDLER_MAGIC_PRIME || i->hashNum < 0) {
			LOG_L(L_WARNING,
					"[LosHandler::FreeInstance][2] bad LOS-instance hash (%d)",
					i->hashNum);
			return;
		}

		i->toBeDeleted = false;

		if (i->refCount == 0) {
			auto& cont = instanceHash[i->hashNum];
			auto it = std::find(cont.begin(), cont.end(), i);
			cont.erase(it);
			delete i;
		}
	}
}


int CLosHandler::GetHashNum(CUnit* unit)
{
	const unsigned int t =
		(unit->mapSquare * unit->losRadius + unit->allyteam) ^
		(*(unsigned int*) &unit->losHeight);
	//! hash-value range is [0, LOSHANDLER_MAGIC_PRIME - 1]
	return (t % LOSHANDLER_MAGIC_PRIME);
}


void CLosHandler::AllocInstance(LosInstance* instance)
{
	if (instance->refCount == 0) {
		LosAdd(instance);
	}
	instance->refCount++;
}


void CLosHandler::CleanupInstance(LosInstance* instance)
{
	if (instance->losSize > 0) { losMaps[instance->allyteam].AddMapSquares(instance->losSquares, instance->allyteam, -1); }
	if (instance->airLosSize > 0) { airLosMaps[instance->allyteam].AddMapArea(instance->baseAirPos, instance->allyteam, instance->airLosSize, -1); }
}


void CLosHandler::Update()
{
	while (!delayQue.empty() && delayQue.front().timeoutTime < gs->frameNum) {
		FreeInstance(delayQue.front().instance);
		delayQue.pop_front();
	}
}


void CLosHandler::DelayedFreeInstance(LosInstance* instance)
{
	DelayedInstance di;
	di.instance = instance;
	di.timeoutTime = (gs->frameNum + (GAME_SPEED + (GAME_SPEED >> 1)));

	delayQue.push_back(di);
}


bool CLosHandler::InLos(const CUnit* unit, int allyTeam) const
{
	// NOTE: units are treated differently than world objects in two ways:
	//   1. they can be cloaked (has to be checked BEFORE all other cases)
	//   2. when underwater, they are only considered to be in LOS if they
	//      are also in radar ("sonar") coverage if requireSonarUnderWater
	//      is enabled --> underwater units can NOT BE SEEN AT ALL without
	//      active radar!
	#ifdef LOSHANDLER_ALWAYSVISIBLE_OVERRIDES_CLOAKED
	if (unit->alwaysVisible)
		return true;
	if (unit->isCloaked)
		return false;
	#else
	if (unit->isCloaked)
		return false;
	if (unit->alwaysVisible)
		return true;
	#endif

	// isCloaked always overrides globalLOS
	if (gs->globalLOS[allyTeam])
		return true;
	if (unit->useAirLos)
		return (InAirLos(unit->pos, allyTeam) || InAirLos(unit->pos + unit->speed, allyTeam));

	if (requireSonarUnderWater) {
		if (unit->IsUnderWater() && !radarHandler->InRadar(unit, allyTeam)) {
			return false;
		}
	}

	return (InLos(unit->pos, allyTeam) || InLos(unit->pos + unit->speed, allyTeam));
}
