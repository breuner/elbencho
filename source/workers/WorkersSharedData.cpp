// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "ProgArgs.h"
#include "WorkersSharedData.h"

uint32_t WorkersSharedData::gotUserInterruptSignalT = 0;
bool WorkersSharedData::isPhaseTimeExpired = false;


/**
 * Increase number of finished worker threads, update first/last finisher stats and notify waiters
 * through condition.
 *
 * @triggerStoneWall true if this thread triggers the "first done" stonewall stats. This cannot be
 * 		checked via "numWorkersDone==1" because the first finisher might just not have gotten any
 * 		work assigned and thus doesn't trigger the stonewall stats.
 */
void WorkersSharedData::incNumWorkersDoneUnlocked(bool triggerStoneWall)
{
	numWorkersDone++;

	if(triggerStoneWall)
		cpuUtilFirstDone.update();

	if(numWorkersDone == progArgs->getNumThreads() )
		cpuUtilLastDone.update();

	condition.notify_all();
}
