/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder_coll_scan.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/exchange.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

namespace mongo::stage_builder {
namespace {
/**
 * Checks whether a callback function should be created for a ScanStage and returns it, if so. The
 * logic in the provided callback will be executed when the ScanStage is opened or reopened.
 */
sbe::ScanOpenCallback makeOpenCallbackIfNeeded(const CollectionPtr& collection,
                                               const CollectionScanNode* csn) {
    if (csn->direction == CollectionScanParams::FORWARD && csn->shouldWaitForOplogVisibility) {
        invariant(!csn->tailable);
        invariant(collection->ns().isOplog());

        return [](OperationContext* opCtx, const CollectionPtr& collection, bool reOpen) {
            if (!reOpen) {
                // Forward, non-tailable scans from the oplog need to wait until all oplog entries
                // before the read begins to be visible. This isn't needed for reverse scans because
                // we only hide oplog entries from forward scans, and it isn't necessary for tailing
                // cursors because they ignore EOF and will eventually see all writes. Forward,
                // non-tailable scans are the only case where a meaningful EOF will be seen that
                // might not include writes that finished before the read started. This also must be
                // done before we create the cursor as that is when we establish the endpoint for
                // the cursor. Also call abandonSnapshot to make sure that we are using a fresh
                // storage engine snapshot while waiting. Otherwise, we will end up reading from the
                // snapshot where the oplog entries are not yet visible even after the wait.

                opCtx->recoveryUnit()->abandonSnapshot();
                collection->getRecordStore()->waitForAllEarlierOplogWritesToBeVisible(opCtx);
            }
        };
    }
    return {};
}

/**
 * If 'shouldTrackLatestOplogTimestamp' returns a vector holding the name of the oplog 'ts' field
 * along with another vector holding a SlotId to map this field to, as well as the standalone value
 * of the same SlotId (the latter is returned purely for convenience purposes).
 */
std::tuple<std::vector<std::string>, sbe::value::SlotVector, boost::optional<sbe::value::SlotId>>
makeOplogTimestampSlotsIfNeeded(const CollectionPtr& collection,
                                sbe::value::SlotIdGenerator* slotIdGenerator,
                                bool shouldTrackLatestOplogTimestamp) {
    if (shouldTrackLatestOplogTimestamp) {
        invariant(collection->ns().isOplog());

        auto tsSlot = slotIdGenerator->generate();
        return {{repl::OpTime::kTimestampFieldName.toString()}, sbe::makeSV(tsSlot), tsSlot};
    }
    return {};
};

/**
 * Creates a collection scan sub-tree optimized for oplog scans. We can built an optimized scan
 * when there is a predicted on the 'ts' field of the oplog collection.
 *
 *   1. If a lower bound on 'ts' is present, the collection scan will seek directly to the RecordId
 *      of an oplog entry as close to this lower bound as possible without going higher.
 *         1.1 If the query is just a lower bound on 'ts' on a forward scan, every document in the
 *             collection after the first matching one must also match. To avoid wasting time
 *             running the filter on every document to be returned, we will stop applying the filter
 *             once it finds the first match.
 *   2. If an upper bound on 'ts' is present, the collection scan will stop and return EOF the first
 *      time it fetches a document that does not pass the filter and has 'ts' greater than the upper
 *      bound.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateOptimizedOplogScan(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::FrameIdGenerator* frameIdGenerator,
    PlanYieldPolicy* yieldPolicy,
    sbe::RuntimeEnvironment* env,
    bool isTailableResumeBranch,
    sbe::LockAcquisitionCallback lockAcquisitionCallback) {
    invariant(collection->ns().isOplog());
    // The minRecord and maxRecord optimizations are not compatible with resumeAfterRecordId and can
    // only be done for a forward scan.
    invariant(!csn->resumeAfterRecordId);
    invariant(csn->direction == CollectionScanParams::FORWARD);

    auto resultSlot = slotIdGenerator->generate();
    auto recordIdSlot = slotIdGenerator->generate();

    // Start the scan from the RecordId stored in seekRecordId.
    // Otherwise, if we're building a collection scan for a resume branch of a special union
    // sub-tree implementing a tailable cursor scan, we can use the seekRecordIdSlot directly
    // to access the recordId to resume the scan from.
    auto [seekRecordId, seekRecordIdSlot] =
        [&]() -> std::pair<boost::optional<RecordId>, boost::optional<sbe::value::SlotId>> {
        if (isTailableResumeBranch) {
            auto resumeRecordIdSlot = env->getSlot("resumeRecordId"_sd);
            return {{}, resumeRecordIdSlot};
        } else if (csn->minRecord) {
            auto cursor = collection->getRecordStore()->getCursor(opCtx);
            auto startRec = cursor->seekNear(*csn->minRecord);
            if (startRec) {
                LOGV2_DEBUG(205841, 3, "Using direct oplog seek");
                return {startRec->id, slotIdGenerator->generate()};
            }
        }
        return {};
    }();

    // Check if we need to project out an oplog 'ts' field as part of the collection scan. We will
    // need it either when 'maxRecord' bound has been provided, so that we can apply an EOF filter,
    // of if we need to track the latest oplog timestamp.
    const auto shouldTrackLatestOplogTimestamp = !csn->stopApplyingFilterAfterFirstMatch &&
        (csn->maxRecord || csn->shouldTrackLatestOplogTimestamp);
    auto&& [fields, slots, tsSlot] = makeOplogTimestampSlotsIfNeeded(
        collection, slotIdGenerator, shouldTrackLatestOplogTimestamp);

    auto stage = sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                            resultSlot,
                                            recordIdSlot,
                                            std::move(fields),
                                            std::move(slots),
                                            seekRecordIdSlot,
                                            true /* forward */,
                                            yieldPolicy,
                                            csn->nodeId(),
                                            lockAcquisitionCallback,
                                            makeOpenCallbackIfNeeded(collection, csn));

    // Start the scan from the seekRecordId.
    if (seekRecordId) {
        invariant(seekRecordIdSlot);

        // Project the start RecordId as a seekRecordIdSlot and feed it to the inner side (scan).
        stage = sbe::makeS<sbe::LoopJoinStage>(
            sbe::makeProjectStage(
                sbe::makeS<sbe::LimitSkipStage>(
                    sbe::makeS<sbe::CoScanStage>(csn->nodeId()), 1, boost::none, csn->nodeId()),
                csn->nodeId(),
                *seekRecordIdSlot,
                makeConstant(sbe::value::TypeTags::RecordId, seekRecordId->getLong())),
            std::move(stage),
            sbe::makeSV(),
            sbe::makeSV(*seekRecordIdSlot),
            nullptr,
            csn->nodeId());
    }

    // Create a filter which checks the first document to ensure either that its 'ts' is less than
    // or equal the minimum timestamp that should not have rolled off the oplog, or that it is a
    // replica set initialization message. If this fails, then we throw
    // ErrorCodes::OplogQueryMinTsMissing. We avoid doing this check on the resumable branch of a
    // tailable scan; it only needs to be done once, when the initial branch is run.
    if (csn->assertTsHasNotFallenOffOplog && !isTailableResumeBranch) {
        invariant(csn->shouldTrackLatestOplogTimestamp);

        // We will be constructing a filter that needs to see the 'ts' field. We name it 'minTsSlot'
        // here so that it does not shadow the 'tsSlot' which we allocated earlier.
        auto&& [fields, minTsSlots, minTsSlot] = makeOplogTimestampSlotsIfNeeded(
            collection, slotIdGenerator, csn->shouldTrackLatestOplogTimestamp);

        // We should always have allocated a 'minTsSlot', and there should always be a 'tsSlot'
        // already allocated for the existing scan that we created previously.
        invariant(minTsSlot);
        invariant(tsSlot);

        // Our filter will also need to see the 'op' and 'o.msg' fields.
        auto opTypeSlot = slotIdGenerator->generate();
        auto oObjSlot = slotIdGenerator->generate();
        minTsSlots.push_back(opTypeSlot);
        minTsSlots.push_back(oObjSlot);
        fields.push_back("op");
        fields.push_back("o");

        // If the first entry we see in the oplog is the replset initialization, then it doesn't
        // matter if its timestamp is later than the specified minTs; no events earlier than the
        // minTs can have fallen off this oplog. Otherwise, we must verify that the timestamp of the
        // first observed oplog entry is earlier than or equal to the minTs time.
        //
        // To achieve this, we build a two-branch union subtree. The left branch is a scan with a
        // filter that checks the first entry in the oplog for the above criteria, throws via EFail
        // if they are not met, and EOFs otherwise. The right branch of the union plan is the tree
        // that we originally built above.
        //
        // union [s9, s10, s11] [
        //     [s6, s7, s8] efilter {if (ts <= minTs || op == "n" && isObject (o) &&
        //                      getField (o, "msg") == "initiating set", false, fail ( 326 ))}
        //     scan [s6 = ts, s7 = op, s8 = o] @oplog,
        //     <stage>

        // Set up the filter stage to be used in the left branch of the union. If the main body of
        // the expression does not match the input document, it throws OplogQueryMinTsMissing. If
        // the expression does match, then it returns 'false', which causes the filter (and as a
        // result, the branch) to EOF immediately. Note that the resultSlot and recordIdSlot
        // arguments to the ScanStage are boost::none, as we do not need them.
        auto minTsBranch = sbe::makeS<sbe::FilterStage<false, true>>(
            sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                       boost::none,
                                       boost::none,
                                       std::move(fields),
                                       minTsSlots, /* don't move this */
                                       boost::none,
                                       true /* forward */,
                                       yieldPolicy,
                                       csn->nodeId(),
                                       lockAcquisitionCallback),
            sbe::makeE<sbe::EIf>(
                makeBinaryOp(
                    sbe::EPrimBinary::logicOr,
                    makeBinaryOp(sbe::EPrimBinary::lessEq,
                                 makeVariable(*minTsSlot),
                                 makeConstant(sbe::value::TypeTags::Timestamp,
                                              csn->assertTsHasNotFallenOffOplog->asULL())),
                    makeBinaryOp(
                        sbe::EPrimBinary::logicAnd,
                        makeBinaryOp(
                            sbe::EPrimBinary::eq, makeVariable(opTypeSlot), makeConstant("n")),
                        makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                     makeFunction("isObject", makeVariable(oObjSlot)),
                                     makeBinaryOp(sbe::EPrimBinary::eq,
                                                  makeFunction("getField",
                                                               makeVariable(oObjSlot),
                                                               makeConstant("msg")),
                                                  makeConstant(repl::kInitiatingSetMsg))))),
                makeConstant(sbe::value::TypeTags::Boolean, false),
                sbe::makeE<sbe::EFail>(ErrorCodes::OplogQueryMinTsMissing,
                                       "Specified minTs has already fallen off the oplog")),
            csn->nodeId());

        // All branches of the UnionStage must have the same number of input and output slots, and
        // we want to remap all slots from the basic scan we constructed earlier through the union
        // stage to the output. We're lucky that the real scan happens to have the same number of
        // slots (resultSlot, recordSlot, tsSlot) as the minTs check branch (minTsSlot, opTypeSlot,
        // oObjSlot), so we don't have to compensate with any unused slots. Note that the minTsSlots
        // will never be mapped to output in practice, since the minTs branch either throws or EOFs.
        //
        // We also need to update the local variables for each slot to their remapped values, so
        // subsequent subtrees constructed by this function refer to the correct post-union slots.
        auto realSlots = sbe::makeSV(resultSlot, recordIdSlot, *tsSlot);
        resultSlot = slotIdGenerator->generate();
        recordIdSlot = slotIdGenerator->generate();
        tsSlot = slotIdGenerator->generate();
        auto outputSlots = sbe::makeSV(resultSlot, recordIdSlot, *tsSlot);

        // Create the union stage. The left branch, which runs first, is our resumability check.
        stage = sbe::makeS<sbe::UnionStage>(
            makeVector<std::unique_ptr<sbe::PlanStage>>(std::move(minTsBranch), std::move(stage)),
            makeVector<sbe::value::SlotVector>(std::move(minTsSlots), std::move(realSlots)),
            std::move(outputSlots),
            csn->nodeId());
    }

    // Add an EOF filter to stop the scan after we fetch the first document that has 'ts' greater
    // than the upper bound.
    if (csn->maxRecord) {
        // The 'maxRecord' optimization is not compatible with 'stopApplyingFilterAfterFirstMatch'.
        invariant(!csn->stopApplyingFilterAfterFirstMatch);
        invariant(tsSlot);

        stage = sbe::makeS<sbe::FilterStage<false, true>>(
            std::move(stage),
            makeBinaryOp(sbe::EPrimBinary::lessEq,
                         makeVariable(*tsSlot),
                         makeConstant(sbe::value::TypeTags::Timestamp, csn->maxRecord->getLong())),
            csn->nodeId());
    }

    // If csn->stopApplyingFilterAfterFirstMatch is true, assert that csn has a filter.
    invariant(!csn->stopApplyingFilterAfterFirstMatch || csn->filter);

    if (csn->filter) {
        auto relevantSlots = sbe::makeSV(resultSlot, recordIdSlot);
        if (tsSlot) {
            relevantSlots.push_back(*tsSlot);
        }

        std::tie(std::ignore, stage) = generateFilter(opCtx,
                                                      csn->filter.get(),
                                                      std::move(stage),
                                                      slotIdGenerator,
                                                      frameIdGenerator,
                                                      resultSlot,
                                                      env,
                                                      std::move(relevantSlots),
                                                      csn->nodeId());

        // We may be requested to stop applying the filter after the first match. This can happen
        // if the query is just a lower bound on 'ts' on a forward scan. In this case every document
        // in the collection after the first matching one must also match, so there is no need to
        // run the filter on such elements.
        //
        // To apply this optimization we will construct the following sub-tree:
        //
        //       nlj [] [seekRecordIdSlot]
        //           left
        //              limit 1
        //              filter <predicate>
        //              <stage>
        //           right
        //              seek seekRecordIdSlot resultSlot recordIdSlot @coll
        //
        // Here, the nested loop join outer branch is the collection scan we constructed above, with
        // a csn->filter predicate sitting on top. The 'limit 1' stage is to ensure this branch
        // returns a single row. Once executed, this branch will filter out documents which doesn't
        // satisfy the predicate, and will return the first document, along with a RecordId, that
        // matches. This RecordId is then used as a starting point of the collection scan in the
        // inner branch, and the execution will continue from this point further on, without
        // applying the filter.
        if (csn->stopApplyingFilterAfterFirstMatch) {
            invariant(csn->minRecord);
            invariant(csn->direction == CollectionScanParams::FORWARD);

            std::tie(fields, slots, tsSlot) = makeOplogTimestampSlotsIfNeeded(
                collection, slotIdGenerator, csn->shouldTrackLatestOplogTimestamp);

            seekRecordIdSlot = recordIdSlot;
            resultSlot = slotIdGenerator->generate();
            recordIdSlot = slotIdGenerator->generate();

            stage = sbe::makeS<sbe::LoopJoinStage>(
                sbe::makeS<sbe::LimitSkipStage>(std::move(stage), 1, boost::none, csn->nodeId()),
                sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                           resultSlot,
                                           recordIdSlot,
                                           std::move(fields),
                                           std::move(slots),
                                           seekRecordIdSlot,
                                           true /* forward */,
                                           yieldPolicy,
                                           csn->nodeId(),
                                           std::move(lockAcquisitionCallback)),
                sbe::makeSV(),
                sbe::makeSV(*seekRecordIdSlot),
                nullptr,
                csn->nodeId());
        }
    }

    // If csn->shouldTrackLatestOplogTimestamp is true, assert that we generated tsSlot.
    invariant(!csn->shouldTrackLatestOplogTimestamp || tsSlot);

    PlanStageSlots outputs;
    outputs.set(PlanStageSlots::kResult, resultSlot);
    outputs.set(PlanStageSlots::kRecordId, recordIdSlot);

    if (csn->shouldTrackLatestOplogTimestamp) {
        outputs.set(PlanStageSlots::kOplogTs, *tsSlot);
    }

    return {std::move(stage), std::move(outputs)};
}

/**
 * Generates a generic collecion scan sub-tree. If a resume token has been provided, the scan will
 * start from a RecordId contained within this token, otherwise from the beginning of the
 * collection.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateGenericCollScan(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::FrameIdGenerator* frameIdGenerator,
    PlanYieldPolicy* yieldPolicy,
    sbe::RuntimeEnvironment* env,
    bool isTailableResumeBranch,
    sbe::LockAcquisitionCallback lockAcquisitionCallback) {
    const auto forward = csn->direction == CollectionScanParams::FORWARD;

    invariant(!csn->shouldTrackLatestOplogTimestamp || collection->ns().isOplog());
    invariant(!csn->resumeAfterRecordId || forward);
    invariant(!csn->resumeAfterRecordId || !csn->tailable);

    auto resultSlot = slotIdGenerator->generate();
    auto recordIdSlot = slotIdGenerator->generate();
    auto seekRecordIdSlot = [&]() -> boost::optional<sbe::value::SlotId> {
        if (csn->resumeAfterRecordId) {
            return slotIdGenerator->generate();
        } else if (isTailableResumeBranch) {
            auto resumeRecordIdSlot = env->getSlot("resumeRecordId"_sd);
            invariant(resumeRecordIdSlot);
            return resumeRecordIdSlot;
        }
        return {};
    }();

    // See if we need to project out an oplog latest timestamp.
    auto&& [fields, slots, tsSlot] = makeOplogTimestampSlotsIfNeeded(
        collection, slotIdGenerator, csn->shouldTrackLatestOplogTimestamp);

    auto stage = sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                            resultSlot,
                                            recordIdSlot,
                                            std::move(fields),
                                            std::move(slots),
                                            seekRecordIdSlot,
                                            forward,
                                            yieldPolicy,
                                            csn->nodeId(),
                                            lockAcquisitionCallback,
                                            makeOpenCallbackIfNeeded(collection, csn));

    // Check if the scan should be started after the provided resume RecordId and construct a nested
    // loop join sub-tree to project out the resume RecordId as a seekRecordIdSlot and feed it to
    // the inner side (scan). We will also construct a union sub-tree as an outer side of the loop
    // join to implement the check that the record we're trying to reposition the scan exists.
    if (seekRecordIdSlot && !isTailableResumeBranch) {
        // Project out the RecordId we want to resume from as 'seekSlot'.
        auto seekSlot = slotIdGenerator->generate();
        auto projStage = sbe::makeProjectStage(
            sbe::makeS<sbe::LimitSkipStage>(
                sbe::makeS<sbe::CoScanStage>(csn->nodeId()), 1, boost::none, csn->nodeId()),
            csn->nodeId(),
            seekSlot,
            makeConstant(sbe::value::TypeTags::RecordId, csn->resumeAfterRecordId->getLong()));

        // Construct a 'seek' branch of the 'union'. If we're succeeded to reposition the cursor,
        // the branch will output  the 'seekSlot' to start the real scan from, otherwise it will
        // produce EOF.
        auto seekBranch =
            sbe::makeS<sbe::LoopJoinStage>(std::move(projStage),
                                           sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                                                      boost::none,
                                                                      boost::none,
                                                                      std::vector<std::string>{},
                                                                      sbe::makeSV(),
                                                                      seekSlot,
                                                                      forward,
                                                                      yieldPolicy,
                                                                      csn->nodeId(),
                                                                      lockAcquisitionCallback),
                                           sbe::makeSV(seekSlot),
                                           sbe::makeSV(seekSlot),
                                           nullptr,
                                           csn->nodeId());

        // Construct a 'fail' branch of the union. The 'unusedSlot' is needed as each union branch
        // must have the same number of slots, and we use just one in the 'seek' branch above. This
        // branch will only be executed if the 'seek' branch produces EOF, which can only happen if
        // if the seek did not find the record id specified in $_resumeAfter.
        auto unusedSlot = slotIdGenerator->generate();
        auto failBranch = sbe::makeProjectStage(
            sbe::makeS<sbe::CoScanStage>(csn->nodeId()),
            csn->nodeId(),
            unusedSlot,
            sbe::makeE<sbe::EFail>(
                ErrorCodes::KeyNotFound,
                str::stream() << "Failed to resume collection scan: the recordId from which we are "
                              << "attempting to resume no longer exists in the collection: "
                              << csn->resumeAfterRecordId));

        // Construct a union stage from the 'seek' and 'fail' branches. Note that this stage will
        // ever produce a single call to getNext() due to a 'limit 1' sitting on top of it.
        auto unionStage = sbe::makeS<sbe::UnionStage>(
            makeVector<std::unique_ptr<sbe::PlanStage>>(std::move(seekBranch),
                                                        std::move(failBranch)),
            std::vector<sbe::value::SlotVector>{sbe::makeSV(seekSlot), sbe::makeSV(unusedSlot)},
            sbe::makeSV(*seekRecordIdSlot),
            csn->nodeId());

        // Construct the final loop join. Note that we also inject a 'skip 1' stage on top of the
        // inner branch, as we need to start _after_ the resume RecordId, and a 'limit 1' stage on
        // top of the outer branch, as it should produce just a single seek recordId.
        stage = sbe::makeS<sbe::LoopJoinStage>(
            sbe::makeS<sbe::LimitSkipStage>(std::move(unionStage), 1, boost::none, csn->nodeId()),
            sbe::makeS<sbe::LimitSkipStage>(std::move(stage), boost::none, 1, csn->nodeId()),
            sbe::makeSV(),
            sbe::makeSV(*seekRecordIdSlot),
            nullptr,
            csn->nodeId());
    }

    if (csn->filter) {
        // The 'stopApplyingFilterAfterFirstMatch' optimization is only applicable when the 'ts'
        // lower bound is also provided for an oplog scan, and is handled in
        // 'generateOptimizedOplogScan()'.
        invariant(!csn->stopApplyingFilterAfterFirstMatch);

        auto relevantSlots = sbe::makeSV(resultSlot, recordIdSlot);
        if (tsSlot) {
            relevantSlots.push_back(*tsSlot);
        }

        std::tie(std::ignore, stage) = generateFilter(opCtx,
                                                      csn->filter.get(),
                                                      std::move(stage),
                                                      slotIdGenerator,
                                                      frameIdGenerator,
                                                      resultSlot,
                                                      env,
                                                      std::move(relevantSlots),
                                                      csn->nodeId());
    }

    PlanStageSlots outputs;
    outputs.set(PlanStageSlots::kResult, resultSlot);
    outputs.set(PlanStageSlots::kRecordId, recordIdSlot);

    if (tsSlot) {
        outputs.set(PlanStageSlots::kOplogTs, *tsSlot);
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateCollScan(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::FrameIdGenerator* frameIdGenerator,
    PlanYieldPolicy* yieldPolicy,
    sbe::RuntimeEnvironment* env,
    bool isTailableResumeBranch,
    sbe::LockAcquisitionCallback lockAcquisitionCallback) {
    if (csn->minRecord || csn->maxRecord) {
        return generateOptimizedOplogScan(opCtx,
                                          collection,
                                          csn,
                                          slotIdGenerator,
                                          frameIdGenerator,
                                          yieldPolicy,
                                          env,
                                          isTailableResumeBranch,
                                          std::move(lockAcquisitionCallback));
    } else {
        return generateGenericCollScan(opCtx,
                                       collection,
                                       csn,
                                       slotIdGenerator,
                                       frameIdGenerator,
                                       yieldPolicy,
                                       env,
                                       isTailableResumeBranch,
                                       std::move(lockAcquisitionCallback));
    }
}
}  // namespace mongo::stage_builder
