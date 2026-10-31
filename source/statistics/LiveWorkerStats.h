// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef LIVEWORKERSTATS_H_
#define LIVEWORKERSTATS_H_

#include "statistics/LiveLatency.h"
#include "statistics/LiveOps.h"

/**
 * The live stats of one worker for the latest live stats interval. Updated once per interval by
 * the live stats thread through Worker::updateLiveStats(), read by the live stats printers
 * (console and csv), so all of them see the same values.
 */
struct LiveWorkerStats
{
    LiveOps done; // ops done since phase start (write or all)
    LiveOps doneReadMix; // rwmix read (netbench recv) ops done since phase start
    LiveOps intervalOps; // ops done within the interval
    LiveOps intervalOpsReadMix;
    LiveOps perSec; // intervalOps per second
    LiveOps perSecReadMix;
    LiveLatency latency; // latency within the interval

    void setToZero()
    {
        done.setToZero();
        doneReadMix.setToZero();
        intervalOps.setToZero();
        intervalOpsReadMix.setToZero();
        perSec.setToZero();
        perSecReadMix.setToZero();
        latency.setToZero();
    }
};

#endif /* LIVEWORKERSTATS_H_ */
