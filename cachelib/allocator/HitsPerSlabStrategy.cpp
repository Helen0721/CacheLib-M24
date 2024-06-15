/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cachelib/allocator/HitsPerSlabStrategy.h"

#include <folly/logging/xlog.h>

#include <algorithm>
#include <functional>

#include "cachelib/allocator/Util.h"

namespace facebook::cachelib {

HitsPerSlabStrategy::HitsPerSlabStrategy(Config config)
    : RebalanceStrategy(HitsPerSlab), config_(std::move(config)) {
   
    std::cout << "HPS::HPS(Config config): ";
    printf("minDiff:%d,diffRatio:%f,minSlabs:%u,numSlabsFreeMem:%u,minLruTailAge:%u,maxLruTailAge:%u \n",
	config.minDiff,config.diffRatio,config.numSlabsFreeMem,
	config.minLruTailAge,config.maxLruTailAge);
}


// The list of allocation classes to be rebalanced is determined by:
//
// 0. Filter out classes that have below minSlabThreshold_
//
// 1. Filter out classes that have just gained a slab recently
//
// 2. pick victim from the one that has poorest hitsPerSlab
ClassId HitsPerSlabStrategy::pickVictim(const Config& config,
                                        const CacheBase& cache,
                                        PoolId pid,
                                        const PoolStats& stats) {
  auto victims = stats.getClassIds();

  // ignore allocation classes that have fewer than the threshold of slabs.
  victims =
      filterByNumEvictableSlabs(stats, std::move(victims), config.minSlabs);

  if (victims.empty()) {
    std::cout<<"all alloc classes have below minSlabs " << config.minSlabs <<". ";
  }

  // ignore allocation classes that recently gained a slab. These will be
  // growing in their eviction age and we want to let the evicitons stabilize
  // before we consider  them again.
  victims = filterVictimsByHoldOff(pid, stats, std::move(victims));
  
  if (victims.empty()) std::cout<<"v candidates under hold-off. ";
  
  // we are only concerned about the eviction age and not the projected age.
  const auto poolEvictionAgeStats =
      cache.getPoolEvictionAgeStats(pid, /* projectionLength */ 0);
  // filter out alloc classes with less than the minimum tail age
  if (config.minLruTailAge != 0) {
    victims =
        filterByMinTailAge(stats, std::move(victims), config.minLruTailAge);
    if (victims.empty()) std::cout<<"v candidates below minLTA "<<config.minLruTailAge << ". ";
  }

  if (victims.empty()) {
    return Slab::kInvalidClassId;
  }

  const auto& poolState = getPoolState(pid);
  auto victimClassId = pickVictimByFreeMem(
      victims, stats, config.getFreeMemThreshold(), poolState);

  if (victimClassId != Slab::kInvalidClassId) {
    return victimClassId;
  }

  // prioritize victims with max LRU tail age
  if (config.maxLruTailAge != 0) {
    auto maxAgeVictims = filter(
        victims,
        [&](ClassId cid) {
          return stats.evictionAgeForClass(cid) < config.maxLruTailAge;
        },
        folly::sformat(" all candidates with less than {} seconds for tail age",
                       config.maxLruTailAge));
    if (!maxAgeVictims.empty()) {
      victims = std::move(maxAgeVictims);
    }
    else std::cout<<"v candidates above maxLTA "<< config.maxLruTailAge << ". ";
  }

  return *std::min_element(
      victims.begin(), victims.end(), [&](ClassId a, ClassId b) {
        double weight_a =
            config.getWeight ? config.getWeight(pid, a, stats) : 1;
        double weight_b =
            config.getWeight ? config.getWeight(pid, b, stats) : 1;
        return poolState.at(a).projectedDeltaHitsPerSlab(stats) * weight_a <
               poolState.at(b).projectedDeltaHitsPerSlab(stats) * weight_b;
      });
}

// The list of allocation classes to be receiver is determined by:
//
// 0. Filter out classes that have no evictions
//
// 1. Filter out classes that have no slabs
//
// 2. pick receiver from the one that has highest hitsPerSlab
ClassId HitsPerSlabStrategy::pickReceiver(const Config& config,
                                          PoolId pid,
                                          const PoolStats& stats,
                                          ClassId victim) const {
  auto receivers = stats.getClassIds();
  receivers.erase(victim);

  const auto& poolState = getPoolState(pid);
  // filter out alloc classes that are not evicting
  receivers = filterByNoEvictions(stats, std::move(receivers), poolState);
  
  if (receivers.empty()) std::cout<<"r candidates aren't evicting. ";

  // filter out receivers who currently dont have any slabs. Their delta hits
  // do not make much sense.
  receivers = filterByNumEvictableSlabs(stats, std::move(receivers), 0);
  
  if (receivers.empty()) std::cout<<"r candidates has no slabs. ";

  // filter out alloc classes with more than the maximum tail age
  if (config.maxLruTailAge != 0) {
    auto candidates =
        filterByMaxTailAge(stats, receivers, config.maxLruTailAge);
    // if all the candidates exceed the max eviction age then fallback to the
    // hits-based mechanism
    if (!candidates.empty()) {
      receivers = std::move(candidates);
    }
  }

  if (receivers.empty()) {
    std::cout<<"r candidates above max LTA " << config.maxLruTailAge << ". ";
    return Slab::kInvalidClassId;
  }

  return *std::max_element(
      receivers.begin(), receivers.end(), [&](ClassId a, ClassId b) {
        double weight_a =
            config.getWeight ? config.getWeight(pid, a, stats) : 1;
        double weight_b =
            config.getWeight ? config.getWeight(pid, b, stats) : 1;
        return poolState.at(a).deltaHitsPerSlab(stats) * weight_a <
               poolState.at(b).deltaHitsPerSlab(stats) * weight_b;
      });
}

RebalanceContext HitsPerSlabStrategy::pickVictimAndReceiverImpl(
    const CacheBase& cache, PoolId pid, const PoolStats& poolStats) {

  std::cout << "HPS-pickVAndRImpl...";

  if (!cache.getPool(pid).allSlabsAllocated()) {
    XLOGF(DBG,
          "Pool Id: {}"
          " does not have all its slabs allocated"
          " and does not need rebalancing.",
          static_cast<int>(pid));
    return kNoOpContext;
  }

  const auto config = getConfigCopy();

  RebalanceContext ctx;
  ctx.victimClassId = pickVictim(config, cache, pid, poolStats);
  ctx.receiverClassId = pickReceiver(config, pid, poolStats, ctx.victimClassId);
  
  std::cout << "HPS-v:" << static_cast<int>(ctx.victimClassId) << ". r:" << static_cast<int>(ctx.receiverClassId) << ". " ;

  if (ctx.victimClassId == ctx.receiverClassId ||
      ctx.victimClassId == Slab::kInvalidClassId ||
      ctx.receiverClassId == Slab::kInvalidClassId) {
    std::cout << "HPS-invalid class id." << std::endl << std::flush;
    return kNoOpContext;
  }

  auto& poolState = getPoolState(pid);
  double weightVictim = 1;
  double weightReceiver = 1;
  if (config.getWeight) {
    weightReceiver = config.getWeight(pid, ctx.receiverClassId, poolStats);
    weightVictim = config.getWeight(pid, ctx.victimClassId, poolStats);
  }
  const auto victimProjectedDeltaHitsPerSlab =
      poolState.at(ctx.victimClassId).projectedDeltaHitsPerSlab(poolStats) *
      weightVictim;
  const auto receiverDeltaHitsPerSlab =
      poolState.at(ctx.receiverClassId).deltaHitsPerSlab(poolStats) *
      weightReceiver;

  XLOGF(DBG,
        "Rebalancing: receiver = {}, receiver delta hits per slab = {}, victim "
        "= {}, victim projected delta hits per slab = {}",
        static_cast<int>(ctx.receiverClassId), receiverDeltaHitsPerSlab,
        static_cast<int>(ctx.victimClassId), victimProjectedDeltaHitsPerSlab);

  const auto improvement =
      receiverDeltaHitsPerSlab - victimProjectedDeltaHitsPerSlab;
  if (receiverDeltaHitsPerSlab < victimProjectedDeltaHitsPerSlab ||
      improvement < config.minDiff ||
      improvement < config.diffRatio * static_cast<long double>(
                                           victimProjectedDeltaHitsPerSlab)) {
    XLOG(DBG, " not enough to trigger slab rebalancing");

    std::cout<<"rDHpS: "<< receiverDeltaHitsPerSlab<<" ,vPDHpS: "<< victimProjectedDeltaHitsPerSlab <<"...";
    if (receiverDeltaHitsPerSlab < victimProjectedDeltaHitsPerSlab){
	std::cout<<"rDHpS < vPDHpS. ";
    }
    else{
    	if (improvement < config.minDiff) {
		std::cout<<"improv.< minDiff " << config.minDiff << ". " ;}
    	if (improvement < config.diffRatio * static_cast<long double>(
        victimProjectedDeltaHitsPerSlab)) {
		std::cout<<"improv.< diffRatio * vPDHpS " << config.diffRatio <<". ";
	}
   }
    std::cout << std::endl << std::flush;
    return kNoOpContext;
  }

  // start a hold off so that the receiver does not become a victim soon
  // enough.
  poolState.at(ctx.receiverClassId).startHoldOff();

  // update all alloc classes' hits state to current hits so that next time we
  // only look at the delta hits sicne the last rebalance.
  for (const auto i : poolStats.getClassIds()) {
    poolState[i].updateHits(poolStats);
  }
  std::cout << "HPS-hold off started." << std::endl << std::flush;

  return ctx;
}

ClassId HitsPerSlabStrategy::pickVictimImpl(const CacheBase& cache,
                                            PoolId pid,
                                            const PoolStats& poolStats) {
  const auto config = getConfigCopy();
  auto victimClassId = pickVictim(config, cache, pid, poolStats);

  auto& poolState = getPoolState(pid);
  // update all alloc classes' hits state to current hits so that next time we
  // only look at the delta hits sicne the last resize.
  for (const auto i : poolStats.getClassIds()) {
    poolState[i].updateHits(poolStats);
  }

  return victimClassId;
}
} // namespace facebook::cachelib
