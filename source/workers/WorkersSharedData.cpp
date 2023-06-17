#include "ProgArgs.h"
#include "WorkersSharedData.h"

bool WorkersSharedData::gotUserInterruptSignal = false;
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
