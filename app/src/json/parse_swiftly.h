#ifndef PARSE_SWIFTLY_H
#define PARSE_SWIFTLY_H
#include "stop.h"

/** @brief A valid response that carries no upcoming departures.
 *
 * Distinct from every other non-zero return, which are genuine parse
 * failures: the fetch worked and the stop simply has nothing scheduled, so
 * callers must not count it against fetch health. Named because its value
 * has to agree with update_stop()'s translation of it, and the two lived in
 * different files silently disagreeing.
 */
#define PARSE_SWIFTLY_NO_DEPARTURES 5

int parse_swiftly_json(const char *const json_ptr, Stop *stop);
#endif  // PARSE_SWIFTLY_H
