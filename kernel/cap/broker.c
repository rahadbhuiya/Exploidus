#include "broker.h"
#include "../audit/audit.h"

bool broker_check(uint32_t pid, cap_token_t token, uint64_t required_rights)
{
    bool result = cap_validate(token, pid, required_rights);
    if (!result) {
        audit_record(AUDIT_CAP_DENIED, pid, token.upper, required_rights);
    }
    return result;
}
