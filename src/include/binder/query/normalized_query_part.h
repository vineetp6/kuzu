#pragma once

#include <optional>

#include "binder/query/reading_clause/bound_reading_clause.h"
#include "binder/query/return_with_clause/bound_projection_body.h"
#include "binder/query/updating_clause/bound_updating_clause.h"

namespace kuzu {
namespace binder {

class NormalizedQueryPart {
public:
    NormalizedQueryPart() = default;
    DELETE_COPY_DEFAULT_MOVE(NormalizedQueryPart);

    inline void addReadingClause(std::unique_ptr<BoundReadingClause> boundReadingClause) {
        readingClauses.push_back(std::move(boundReadingClause));
    }
    inline bool hasReadingClause() const { return !readingClauses.empty(); }
    inline uint32_t getNumReadingClause() const { return readingClauses.size(); }
    inline BoundReadingClause* getReadingClause(uint32_t idx) const {
        return readingClauses[idx].get();
    }

    inline void addUpdatingClause(std::unique_ptr<BoundUpdatingClause> boundUpdatingClause) {
        updatingClauses.push_back(std::move(boundUpdatingClause));
    }
    inline bool hasUpdatingClause() const { return !updatingClauses.empty(); }
    inline uint32_t getNumUpdatingClause() const { return updatingClauses.size(); }
    inline BoundUpdatingClause* getUpdatingClause(uint32_t idx) const {
        return updatingClauses[idx].get();
    }

    inline void setProjectionBody(BoundProjectionBody boundProjectionBody) {
        projectionBody = std::move(boundProjectionBody);
    }
    inline bool hasProjectionBody() const { return projectionBody.has_value(); }
    inline BoundProjectionBody* getProjectionBodyUnsafe() {
        KU_ASSERT(projectionBody.has_value());
        return &projectionBody.value();
    }
    inline const BoundProjectionBody* getProjectionBody() const {
        KU_ASSERT(projectionBody.has_value());
        return &projectionBody.value();
    }

    inline bool hasProjectionBodyPredicate() const { return projectionBodyPredicate != nullptr; }
    inline std::shared_ptr<Expression> getProjectionBodyPredicate() const {
        return projectionBodyPredicate;
    }
    inline void setProjectionBodyPredicate(const std::shared_ptr<Expression>& predicate) {
        projectionBodyPredicate = predicate;
    }

private:
    std::vector<std::unique_ptr<BoundReadingClause>> readingClauses;
    std::vector<std::unique_ptr<BoundUpdatingClause>> updatingClauses;
    std::optional<BoundProjectionBody> projectionBody;
    std::shared_ptr<Expression> projectionBodyPredicate;
};

} // namespace binder
} // namespace kuzu
