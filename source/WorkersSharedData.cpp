#include "ProgArgs.h"
#include "WorkersSharedData.h"

bool WorkersSharedData::gotUserInterruptSignal = false;
bool WorkersSharedData::isPhaseTimeExpired = false;


/**
 * Increase number of finished worker threads, update first/last finisher stats and notify waiters
 * through condition.
 */
void WorkersSharedData::incNumWorkersDoneUnlocked()
{
	numWorkersDone++;

	if(numWorkersDone == 1)
		cpuUtilFirstDone.update();

	if(numWorkersDone == progArgs->getNumThreads() )
		cpuUtilLastDone.update();

	condition.notify_all();
}
