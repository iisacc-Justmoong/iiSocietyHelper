#include "Status.h"

namespace iiSocietyHelper::Sync {
Status::Status()
    : query_([this](const ReadStatus &) { return value_.load(); }),
      changes_([](const ReadStatus &, int value) -> std::optional<int> { return value; }) {}
Status::~Status() = default;
Status::Query &Status::query() { return query_; }
Status::Changes &Status::changes() { return changes_; }
void Status::publish(StatusCode code) {
    const auto value = int(code);
    if (value_.exchange(value) != value) (void)changes_.publish(value);
}
}
