#pragma once

#include "aggregate_input.h"
#include "function/aggregate_function.h"
#include "processor/operator/sink.h"

namespace kuzu {
namespace processor {

class BaseAggregateSharedState {
protected:
    explicit BaseAggregateSharedState(
        const std::vector<std::unique_ptr<function::AggregateFunction>>& aggregateFunctions);

    virtual std::pair<uint64_t, uint64_t> getNextRangeToRead() = 0;

    ~BaseAggregateSharedState() = default;

protected:
    std::mutex mtx;
    uint64_t currentOffset;
    std::vector<std::unique_ptr<function::AggregateFunction>> aggregateFunctions;
};

class BaseAggregate : public Sink {
protected:
    BaseAggregate(std::unique_ptr<ResultSetDescriptor> resultSetDescriptor,
        std::vector<std::unique_ptr<function::AggregateFunction>> aggregateFunctions,
        std::vector<AggregateInfo> aggInfos, std::unique_ptr<PhysicalOperator> child, uint32_t id,
        const std::string& paramsString)
        : Sink{std::move(resultSetDescriptor), PhysicalOperatorType::AGGREGATE, std::move(child),
              id, paramsString},
          aggregateFunctions{std::move(aggregateFunctions)}, aggInfos{std::move(aggInfos)} {}

    void initLocalStateInternal(ResultSet* resultSet, ExecutionContext* context) override;

    bool canParallel() const final { return !containDistinctAggregate(); }

    void finalize(ExecutionContext* context) override = 0;

    std::vector<std::unique_ptr<function::AggregateFunction>> cloneAggFunctions();
    std::unique_ptr<PhysicalOperator> clone() override = 0;

private:
    bool containDistinctAggregate() const;

protected:
    std::vector<std::unique_ptr<function::AggregateFunction>> aggregateFunctions;
    std::vector<AggregateInfo> aggInfos;
    std::vector<AggregateInput> aggInputs;
};

} // namespace processor
} // namespace kuzu
