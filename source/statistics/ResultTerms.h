// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef RESULTTERMS_H_
#define RESULTTERMS_H_

#include <algorithm>
#include <cctype>
#include <string>

#include "statistics/LiveOps.h"
#include "ProgArgs.h"

/**
 * The words and rows of a phase result that depend on the benchmark mode. A plain phase has one
 * set of counters, an rwmix phase has a write set, a read set and the sum of both, a netbench
 * phase has a send set and a receive set (both as seen by the clients) and no sum. Chosen once per
 * phase, so the printers of live stats and results need no mode checks.
 */
struct ResultTerms
{
    bool haveSecondary{false}; // the phase has a second counter set (read or recv)
    bool showTotal{false}; // the sum of both sets is worth a row (rwmix)
    bool showEntries{false}; // the phase counts entries (dir mode, not netbench)
    std::string primary; // "" (plain), "write" or "send"
    std::string secondary; // "read" or "recv"
    std::string primaryShort; // "wr" or "send", for live stats brackets and latency labels
    std::string secondaryShort; // "rd" or "recv"
    std::string entriesPrimary; // "write" if the second set counts entries too (rwmix by threads)
    std::string servicesLabel{"Svc"}; // row label of the per-service values ("Clients" in netbench)
    std::string servicesKey{"services"}; // json key of the per-service values

    /**
     * @param haveSecondaryOps true if the second counter set has values in this phase.
     */
    static ResultTerms forPhase(const ProgArgs& progArgs, bool haveSecondaryOps)
    {
        ResultTerms terms;
        terms.haveSecondary = haveSecondaryOps;
        terms.showEntries = (progArgs.getBenchPathType() == BenchPathType_DIR) &&
            !progArgs.getNetBenchArgs().getUseNetBench();

        if(progArgs.getNetBenchArgs().getUseNetBench() )
        {
            terms.primary = terms.primaryShort = "send";
            terms.secondary = terms.secondaryShort = "recv";
            terms.servicesLabel = "Clients";
            terms.servicesKey = "clients";
        }
        else
        if(haveSecondaryOps)
        {
            terms.showTotal = true;
            terms.primary = "write";
            terms.secondary = "read";
            terms.primaryShort = "wr";
            terms.secondaryShort = "rd";

            if(progArgs.hasUserSetRWMixReadThreads() )
                terms.entriesPrimary = "write";
        }

        return terms;
    }

    static std::string suffix(const std::string& word) { return word.empty() ? "" : " " + word; }

    static std::string capitalized(std::string word)
    {
        if(!word.empty() )
            word[0] = toupper(word[0] );

        return word;
    }

    // "MiB/s write" for a mix, otherwise the plain label (e.g. "Throughput MiB/s")
    std::string label(const std::string& base, const std::string& plainLabel) const
        { return primary.empty() ? plainLabel : base + " " + primary; }
    std::string secondaryLabel(const std::string& base) const { return base + " " + secondary; }
    std::string totalLabel(const std::string& base) const { return base + " total"; }

    // the same for entries, which only an rwmix by threads splits into write and read
    std::string entriesLabel(const std::string& base, const std::string& plainLabel) const
        { return entriesPrimary.empty() ? plainLabel : base + " " + entriesPrimary; }

    // row names of the fullscreen live stats
    std::string primaryRow() const { return primary.empty() ? "Total" : capitalized(primary); }
    std::string secondaryRow() const { return capitalized(secondary); }

    /**
     * Add the second counter set to the first for a per-service value: all bytes and entries,
     * but the IOPS of both sets only where the result has a total row. Otherwise a round is one
     * send plus one receive and the larger set counts.
     */
    void combine(LiveOps& ops, const LiveOps& opsSecondary) const
    {
        ops.numBytesDone += opsSecondary.numBytesDone;
        ops.numEntriesDone += opsSecondary.numEntriesDone;
        ops.numIOPSDone = showTotal ? (ops.numIOPSDone + opsSecondary.numIOPSDone) :
            std::max(ops.numIOPSDone, opsSecondary.numIOPSDone);
    }
};

#endif /* RESULTTERMS_H_ */
