/**
 * Test that $setWindowFields works as expected on time-series collections.
 *
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     assumes_unsharded_collection,
 *     do_not_wrap_aggregations_in_facets,
 *     does_not_support_stepdowns,
 *     does_not_support_transactions,
 *     requires_pipeline_optimization,
 *     requires_timeseries,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq.
load("jstests/libs/analyze_plan.js");         // For getAggPlanStage().

const coll = db.window_functions_on_timeseries_coll;

coll.drop();
assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: 'time', metaField: 'attributes'}}));

assert.commandWorked(coll.insert([
    {
        _id: 0,
        time: ISODate("2021-01-01T01:00:00Z"),
        attributes: {sensor: "S1", field: "a"},
        temperature: 55,
        language: "en",
        contributions: 10
    },
    {
        _id: 1,
        time: ISODate("2021-01-01T01:00:00Z"),
        attributes: {sensor: "S2", field: "a"},
        temperature: 74,
        language: "zh",
        contributions: 150
    },
    {
        _id: 2,
        time: ISODate("2021-01-01T01:05:00Z"),
        attributes: {sensor: "S1", field: "a"},
        temperature: 50,
        language: "zh",
        contributions: 20
    },
    {
        _id: 3,
        time: ISODate("2021-01-01T01:05:00Z"),
        attributes: {sensor: "S2", field: "a"},
        temperature: 60,
        language: "en",
    },
    {
        _id: 4,
        time: ISODate("2021-01-01T01:10:00Z"),
        attributes: {sensor: "S1", field: "a"},
        temperature: 51,
        language: "en",
        contributions: 10
    },
    {
        _id: 5,
        time: ISODate("2021-01-01T01:10:00Z"),
        attributes: {sensor: "S2", field: "a"},
        temperature: 100,
        language: "es",
        contributions: 35
    },
    {
        _id: 6,
        time: ISODate("2021-01-01T02:00:00Z"),
        attributes: {sensor: "S3", field: "b"},
        temperature: 90,
        language: "en",
        contributions: 40
    }
]));

/**
 * Runs the given 'pipeline' and asserts that the explain behavior is as expected and the pipeline
 * returns correct results.
 *
 * The 'expectedOpts' object contains:
 * - bucketFilter:  The predicate that is mapped onto the bucket-level, otherwise 'null' if no
 *                  bucket-level filtering is expected.
 * - bucketSort:    The sort key that is expected to be pushed down, otherwise 'null'.
 * - windowSort:    The sort key that is generated by the $setWindowFields desugaring and is not
 *                  able to be pushed down.
 * - inExcludeSpec: The include or exclude list for the $_internalUnpackBucket.
 */
function assertExplainBehaviorAndCorrectResults(pipeline, expectedOpts, expectedResults) {
    const explain = coll.explain().aggregate(pipeline);
    const winningPlan = explain.stages[0].$cursor.queryPlanner.winningPlan;
    assert.neq(null, winningPlan);
    if (expectedOpts.bucketFilter) {
        assert.eq(expectedOpts.bucketFilter, winningPlan.filter);
    }
    if (expectedOpts.bucketSort) {
        assert.eq("SORT", winningPlan.stage);
        assert.eq(expectedOpts.bucketSort, winningPlan.sortPattern);
        assert.eq(null, getAggPlanStage(explain, "$sort"));
    } else if (expectedOpts.windowSort) {
        const sort = getAggPlanStage(explain, "$sort").$sort;
        assert.neq(null, sort);
        assert.eq(expectedOpts.windowSort, sort.sortKey);
    }

    const unpackBucket = getAggPlanStage(explain, "$_internalUnpackBucket").$_internalUnpackBucket;
    assert.neq(null, unpackBucket);
    if (expectedOpts.inExcludeSpec.hasOwnProperty("include")) {
        assert.sameMembers(expectedOpts.inExcludeSpec.include, unpackBucket.include);
    } else {
        assert.sameMembers(expectedOpts.inExcludeSpec.exclude, unpackBucket.exclude);
    }

    assertArrayEq({expected: expectedResults, actual: coll.aggregate(pipeline).toArray()});
}

assertExplainBehaviorAndCorrectResults(
    [{
        $setWindowFields: {
            partitionBy: "$attributes.sensor",
            sortBy: {time: 1},
            output: {
                posAvgTemp: {$avg: "$temperature", window: {documents: [-1, 1]}},
                timeAvgTemp: {$avg: "$temperature", window: {range: [-5, 0], unit: "day"}},
                tempRateOfChange: {
                    $derivative: {input: "$temperature", unit: "hour"},
                    window: {documents: [-1, 0]}
                }
            }
        }
    }],
    // The sort generated by $setWindowFields is on {attributes.sensor: 1, time: 1} which cannot be
    // pushed past $_internalUnpackBucket as it is not fully on the 'meta' field. Since we will be
    // returning the whole document the $_internalUnpackBucket will unpack all regular fields.
    {windowSort: {"attributes.sensor": 1, "time": 1}, inExcludeSpec: {exclude: []}},
    [
        {
            _id: 0,
            time: ISODate("2021-01-01T01:00:00Z"),
            attributes: {sensor: "S1", field: "a"},
            temperature: 55,
            language: "en",
            contributions: 10,
            posAvgTemp: 52.5,
            timeAvgTemp: 55,
            tempRateOfChange: null
        },
        {
            _id: 2,
            time: ISODate("2021-01-01T01:05:00Z"),
            attributes: {sensor: "S1", field: "a"},
            temperature: 50,
            language: "zh",
            contributions: 20,
            posAvgTemp: 52,
            timeAvgTemp: 52.5,
            tempRateOfChange: -60
        },
        {
            _id: 4,
            time: ISODate("2021-01-01T01:10:00Z"),
            attributes: {sensor: "S1", field: "a"},
            temperature: 51,
            language: "en",
            contributions: 10,
            posAvgTemp: 50.5,
            timeAvgTemp: 52,
            tempRateOfChange: 12
        },
        {
            _id: 1,
            time: ISODate("2021-01-01T01:00:00Z"),
            attributes: {sensor: "S2", field: "a"},
            temperature: 74,
            language: "zh",
            contributions: 150,
            posAvgTemp: 67,
            timeAvgTemp: 74,
            tempRateOfChange: null
        },
        {
            _id: 3,
            time: ISODate("2021-01-01T01:05:00Z"),
            attributes: {sensor: "S2", field: "a"},
            temperature: 60,
            language: "en",
            posAvgTemp: 78,
            timeAvgTemp: 67,
            tempRateOfChange: -168
        },
        {
            _id: 5,
            time: ISODate("2021-01-01T01:10:00Z"),
            attributes: {sensor: "S2", field: "a"},
            temperature: 100,
            language: "es",
            contributions: 35,
            posAvgTemp: 80,
            timeAvgTemp: 78,
            tempRateOfChange: 480
        },
        {
            _id: 6,
            time: ISODate("2021-01-01T02:00:00Z"),
            attributes: {sensor: "S3", field: "b"},
            temperature: 90,
            language: "en",
            contributions: 40,
            posAvgTemp: 90,
            timeAvgTemp: 90,
            tempRateOfChange: null
        }
    ]);

assertExplainBehaviorAndCorrectResults(
    [
        {
            $setWindowFields: {
                partitionBy: "$attributes.sensor",
                sortBy: {time: 1},
                output: {firstTemp: {$first: "$temperature"}, lastTemp: {$last: "$temperature"}}
            }
        },
        {$project: {firstTemp: 1, lastTemp: 1}}
    ],
    // The sort generated by $setWindowFields is on {attributes.sensor: 1, time: 1} which cannot be
    // pushed past $_internalUnpackBucket as it is not fully on the 'meta' field.
    // $_internalUnpackBucket can use dependency analysis to unpack only certain needed fields.
    {
        windowSort: {"attributes.sensor": 1, "time": 1},
        inExcludeSpec:
            {include: ["_id", "attributes", "time", "temperature", "firstTemp", "lastTemp"]}
    },
    [
        {_id: 0, firstTemp: 55, lastTemp: 51},
        {_id: 2, firstTemp: 55, lastTemp: 51},
        {_id: 4, firstTemp: 55, lastTemp: 51},
        {_id: 1, firstTemp: 74, lastTemp: 100},
        {_id: 3, firstTemp: 74, lastTemp: 100},
        {_id: 5, firstTemp: 74, lastTemp: 100},
        {_id: 6, firstTemp: 90, lastTemp: 90},
    ]);

assertExplainBehaviorAndCorrectResults(
    [
        {
            $setWindowFields: {
                partitionBy: "$language",
                sortBy: {_id: 1},
                output: {sensorsSoFar: {$addToSet: "$attributes.sensor", window: {range: [-4, 0]}}}
            }
        },
        {$project: {sensorsSoFar: 1}}
    ],
    // The sort generated by $setWindowFields is on {language: 1, _id: 1} which cannot be pushed
    // past $_internalUnpackBucket as it is not fully on the 'meta' field. $_internalUnpackBucket
    // can use dependency analysis to unpack only certain needed fields.
    {
        windowSort: {"language": 1, "_id": 1},
        inExcludeSpec: {include: ["_id", "language", "sensorsSoFar", "attributes"]}
    },
    [
        {_id: 0, sensorsSoFar: ["S1"]},
        {_id: 3, sensorsSoFar: ["S1", "S2"]},
        {_id: 4, sensorsSoFar: ["S1", "S2"]},
        {_id: 6, sensorsSoFar: ["S1", "S2", "S3"]},
        {_id: 1, sensorsSoFar: ["S2"]},
        {_id: 2, sensorsSoFar: ["S2", "S1"]},
        {_id: 5, sensorsSoFar: ["S2"]},
    ]);

assertExplainBehaviorAndCorrectResults(
    [
        {
            $setWindowFields:
                {partitionBy: "$attributes.sensor", output: {total: {$sum: "$contributions"}}}
        },
        {$project: {percentOfTotal: {$divide: ["$contributions", "$total"]}}}
    ],
    // The sort generated by $setWindowFields is on {attributes.sensor: 1} which can be pushed past
    // $_internalUnpackBucket. $_internalUnpackBucket can use dependency analysis to unpack only
    // certain needed fields.
    {
        bucketSort: {"meta.sensor": 1},
        inExcludeSpec: {include: ["_id", "attributes", "contributions", "total"]}
    },
    [
        {_id: 2, percentOfTotal: 20 / 40},
        {_id: 0, percentOfTotal: 10 / 40},
        {_id: 4, percentOfTotal: 10 / 40},
        {_id: 1, percentOfTotal: 150 / 185},
        {_id: 5, percentOfTotal: 35 / 185},
        {_id: 3, percentOfTotal: null},
        {_id: 6, percentOfTotal: 40 / 40},
    ]);

assertExplainBehaviorAndCorrectResults(
    [
        {
            $setWindowFields: {
                partitionBy: "$attributes.sensor",
                sortBy: {contributions: -1},
                output: {rank: {$rank: {}}}
            }
        },
        {$match: {"attributes.field": {$eq: "a"}}},
        {$project: {rank: 1}}
    ],
    // The sort generated by $setWindowFields is on {attributes.sensor: 1, contributions: -1} which
    // cannot be pushed past $_internalUnpackBucket as it is not fully on the 'meta' field. Once
    // the $match can swap with $setWindowFields it can also be pushed past $_internalUnpackBucket.
    // TODO SERVER-56419: Add a check for bucketFilter once $match can be pushed down.
    {
        windowSort: {"attributes.sensor": 1, "contributions": -1},
        inExcludeSpec: {include: ["_id", "attributes", "contributions", "rank"]}
    },
    [
        {_id: 2, rank: 1},
        {_id: 0, rank: 2},
        {_id: 4, rank: 2},
        {_id: 1, rank: 1},
        {_id: 5, rank: 2},
        {_id: 3, rank: 3},
    ]);
})();
