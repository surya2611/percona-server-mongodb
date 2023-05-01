/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/ce/ce_test_utils.h"
#include "mongo/db/query/optimizer/cascades/ce_heuristic.h"
#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {
namespace {

using namespace optimizer;
using namespace optimizer::cascades;

constexpr double kCollCard = 1000.0;
const std::string kCollName = "test";

constexpr double kOtherCollCard = 200.0;
const std::string kOtherCollName = "otherTest";

constexpr double kThirdCollCard = 50.0;
const std::string kThirdCollName = "thirdTest";

class DataflowCETester : public CETester {
public:
    DataflowCETester() : CETester(kCollName, kCollCard, kDefaultCETestPhaseSet) {}

protected:
    std::unique_ptr<CEInterface> getCETransport() const override {
        return std::make_unique<HeuristicCE>();
    }
};

TEST(CEDataflowTest, EstimateTrivialNodes) {
    DataflowCETester t;
    const auto matchCard = t.getMatchCE<optimizer::RootNode>("{a: 1}");

    // Verify 'CollationNode' estimate returns the input cardinality.
    ASSERT_CE(t, "[{$sort: {a: 1}}]", kCollCard);
    ASSERT_CE(t, "[{$sort: {a: -1, b: 1}}]", kCollCard);
    ASSERT_CE(t, "[{$match: {a: 1}}, {$sort: {a: 1, b: 1}}]", matchCard);

    // Verify 'EvaluationNode' estimate.
    ASSERT_CE(t, "[{$project: {a: {$add: [\"$a\", 1]}}}]", kCollCard);
    ASSERT_CE(t, "[{$match: {a: 1}}, {$project: {a: {$add: [\"$a\", 1]}}}]", matchCard);
}

TEST(CEDataflowTest, EstimateUnionNode) {
    auto makeUnionBranch = [](const std::string& collName) {
        auto scanVar = "scan_" + collName;
        auto scanNode = make<ScanNode>(scanVar, collName);
        auto evalPath =
            make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>(scanVar));
        return make<EvaluationNode>("a", std::move(evalPath), std::move(scanNode));
    };

    // Verify that the estimate of 'UnionNode' always returns the sum of estimates of its children.
    // In the following tests we force a simple plan to be generated by passing in a 'manually'
    // constructed ABT.
    {
        DataflowCETester t;
        t.addCollection(kOtherCollName, kOtherCollCard, {});
        t.addCollection(kThirdCollName, kThirdCollCard, {});
        {
            auto unionNode = make<UnionNode>(
                ProjectionNameVector{"a"},
                makeSeq(makeUnionBranch(kCollName), makeUnionBranch(kOtherCollName)));
            auto rootNode = make<RootNode>(
                properties::ProjectionRequirement{ProjectionNameVector{"a"}}, std::move(unionNode));
            ASSERT_CE(t, rootNode, kCollCard + kOtherCollCard);
        }
        {
            auto unionNode = make<UnionNode>(
                ProjectionNameVector{"a"},
                makeSeq(makeUnionBranch(kCollName), makeUnionBranch(kOtherCollName)));
            auto parentUnionNode =
                make<UnionNode>(ProjectionNameVector{"a"},
                                makeSeq(std::move(unionNode), makeUnionBranch(kThirdCollName)));
            auto rootNode =
                make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"a"}},
                               std::move(parentUnionNode));
            ASSERT_CE(t, rootNode, kCollCard + kOtherCollCard + kThirdCollCard);
        }
    }

    // The following plans include a UnionNode.
    {
        DataflowCETester t;
        t.setCollCard(2000);
        t.setIndexes(
            {{"indexA", makeIndexDefinition("a", CollationOp::Ascending, /* isMultiKey */ true)}});
        t.setDisableScan(true);
        ASSERT_MATCH_CE(t, {"{a: [12]}"}, 1);
    }
    {
        DataflowCETester t;
        t.setIndexes(
            {{"indexA", makeIndexDefinition("a", CollationOp::Ascending, /* isMultiKey */ false)},
             {"indexB", makeIndexDefinition("b", CollationOp::Ascending, /* isMultiKey */ false)}});
        t.setDisableScan(true);
        ASSERT_MATCH_CE(t, {"{a: 1, b: 2}"}, 5.62341);
    }
}

TEST(CEDataflowTest, EstimateLimitSkipNode) {
    DataflowCETester t;
    const CEType matchCard = t.getMatchCE<optimizer::RootNode>("{a: 1}");

    // Verify that 'LimitSkipNode' estimate with only a limit set is min(limit, inputCE).
    ASSERT_CE(t, "[{$limit: 1}]", 1.0);
    ASSERT_CE(t, "[{$limit: 50}]", 50.0);
    ASSERT_CE(t, "[{$limit: 1000}]", kCollCard);
    ASSERT_CE(t, "[{$limit: 10000}]", kCollCard);
    ASSERT_CE(t, "[{$match: {a: 1}}, {$limit: 1}]", 1.0);
    ASSERT_CE(t, "[{$match: {a: 1}}, {$limit: 5}]", 5.0);
    ASSERT_CE(t, "[{$match: {a: 1}}, {$limit: 50}]", matchCard);
    ASSERT_CE(t, "[{$match: {a: 1}}, {$limit: 1000}]", matchCard);

    // Verify that 'LimitSkipNode' estimate with only a skip set is max(inputCE - skip, 0).
    ASSERT_CE(t, "[{$skip: 0}]", kCollCard);
    ASSERT_CE(t, "[{$skip: 1}]", kCollCard - 1.0);
    ASSERT_CE(t, "[{$skip: 50}]", kCollCard - 50.0);
    ASSERT_CE(t, "[{$skip: 1000}]", 0.0);
    ASSERT_CE(t, "[{$skip: 10000}]", 0.0);
    ASSERT_CE(t, "[{$match: {a: 1}}, {$skip: 1}]", matchCard - 1.0);
    ASSERT_CE(t, "[{$match: {a: 1}}, {$skip: 5}]", matchCard - 5.0);
    ASSERT_CE(t, "[{$match: {a: 1}}, {$skip: 50}]", 0.0);
    ASSERT_CE(t, "[{$match: {a: 1}}, {$skip: 1000}]", 0.0);

    // Test estimates for combinations of $limit & $skip.
    ASSERT_CE(t, "[{$limit: 1}, {$skip: 1}]", 0.0);
    ASSERT_CE(t, "[{$skip: 1}, {$limit: 1}]", 1.0);
    ASSERT_CE(t, "[{$limit: 1}, {$skip: 50}]", 0.0);
    ASSERT_CE(t, "[{$skip: 50}, {$limit: 1}]", 1.0);
    ASSERT_CE(t, "[{$limit: 50}, {$skip: 1}]", 49.0);
    ASSERT_CE(t, "[{$skip: 1}, {$limit: 50}]", 50.0);
    ASSERT_CE(t, "[{$limit: 50}, {$skip: 50}]", 0.0);
    ASSERT_CE(t, "[{$skip: 50}, {$limit: 50}]", 50.0);
    ASSERT_CE(t, "[{$limit: 1000}, {$skip: 50}]", kCollCard - 50.0);
    ASSERT_CE(t, "[{$skip: 50}, {$limit: 1000}]", kCollCard - 50.0);
    ASSERT_CE(t, "[{$limit: 50}, {$skip: 1000}]", 0.0);
    ASSERT_CE(t, "[{$skip: 1000}, {$limit: 50}]", 0.0);
    ASSERT_CE(t, "[{$limit: 1000}, {$skip: 1000}]", 0.0);
    ASSERT_CE(t, "[{$skip: 1000}, {$limit: 1000}]", 0.0);

    // Test estimates for combinations of $limit & $skip separated by a $match.
    ASSERT_CE(t, "[{$limit: 1}, {$match: {a: 1}}, {$skip: 1}]", 0.0);
    ASSERT_CE(t, "[{$limit: 1}, {$match: {a: 1}}, {$skip: 50}]", 0.0);

    // Input card to $match: 50. $match selectivity here is sqrt(50)/50.
    ASSERT_CE(t, "[{$limit: 50}, {$match: {a: 1}}, {$skip: 1}]", 6.07107);
    ASSERT_CE(t, "[{$limit: 50}, {$match: {a: 1}}, {$skip: 50}]", 0.0);
    ASSERT_CE(t, "[{$limit: 50}, {$match: {a: 1}}, {$skip: 1000}]", 0.0);

    // Input card to $match is kCollCard. However, our estimate is larger than matchCard because we
    // have a FilterNode that does not get converted to a SargableNode in this case. The $match
    // selectivity here is sqrt(1000)/1000.
    ASSERT_CE(t, "[{$limit: 1000}, {$match: {a: 1}}, {$skip: 1}]", 30.6228);
    ASSERT_CE(t, "[{$limit: 1000}, {$match: {a: 1}}, {$skip: 20}]", 11.6228);
    ASSERT_CE(t, "[{$limit: 1000}, {$match: {a: 1}}, {$skip: 1000}]", 0.0);

    // Input card to $match: 999. $match selectivity here is sqrt(999)/999.
    ASSERT_CE(t, "[{$skip: 1}, {$match: {a: 1}}, {$limit: 1}]", 1.0);
    ASSERT_CE(t, "[{$skip: 1}, {$match: {a: 1}}, {$limit: 20}]", 20.0);
    ASSERT_CE(t, "[{$skip: 1}, {$match: {a: 1}}, {$limit: 1000}]", 31.607);

    // Input card to $match: 950. $match selectivity here is sqrt(950)/950.
    ASSERT_CE(t, "[{$skip: 50}, {$match: {a: 1}}, {$limit: 1}]", 1.0);
    ASSERT_CE(t, "[{$skip: 50}, {$match: {a: 1}}, {$limit: 20}]", 20.0);
    ASSERT_CE(t, "[{$skip: 50}, {$match: {a: 1}}, {$limit: 1000}]", 30.8221);

    // Input card to $match is 0.0.
    ASSERT_CE(t, "[{$skip: 1000}, {$match: {a: 1}}, {$limit: 50}]", 0.0);
    ASSERT_CE(t, "[{$skip: 1000}, {$match: {a: 1}}, {$limit: 1000}]", 0.0);
}

TEST(CEDataflowTest, EstimateUnwindNode) {
    DataflowCETester t;
    const CEType matchCard = t.getMatchCE<optimizer::RootNode>("{a: 1}");

    // We assume that arrays on average have ~10 elements, so we estimate this as inputCard*10.
    ASSERT_CE(t, "[{$unwind: '$a'}]", 10 * kCollCard);
    ASSERT_CE(t, "[{$match: {a: 1}}, {$unwind: '$a'}]", 10 * matchCard);
    ASSERT_CE(t, "[{$unwind: {path: '$a', preserveNullAndEmptyArrays: true}}]", 10 * kCollCard);
    ASSERT_CE(t,
              "[{$match: {a: 1}}, {$unwind: {path: '$a', preserveNullAndEmptyArrays: true}}]",
              10 * matchCard);

    // TODO SERVER-70035: implement histogram estimation of $unwind.
}

}  // namespace
}  // namespace mongo::ce
