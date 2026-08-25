#ifndef LOGOS_BASECAMP_ACCESS_POLICY_OPTION_H
#define LOGOS_BASECAMP_ACCESS_POLICY_OPTION_H

#include <QString>

namespace LogosBasecamp {

// The bare deny-by-default document. `mode` is the runtime's own switch (see
// liblogos access_policy.h): "enforce" turns restrictions into denials, and
// with no explicit `restrictions` the runtime derives them from the declared
// dependency graph — a module may only call the modules it declared. This
// spelling lets an operator arm that without hand-writing JSON; it is NOT a
// second switch, it expands to exactly this document. Same alias the logoscore
// CLI accepts, deliberately.
inline constexpr const char* kAccessPolicyEnforceAlias = "enforce";
inline constexpr const char* kAccessPolicyEnforceEnvelope =
    R"({"version":1,"mode":"enforce","restrictions":{}})";

// Outcome of resolving the operator's --access-policy request.
struct AccessPolicyResolution {
    // False only when the operator asked for something we could not honour
    // (unreadable file, malformed JSON). The app must abort rather than boot:
    // silently falling back to "no policy" would hand back a wide-open runtime
    // to someone who explicitly asked to lock it down.
    bool ok = true;
    // Empty ⇒ install no policy at all, i.e. logos_core_set_access_policy(nullptr):
    // enforcement off, which is Basecamp's default and pre-existing behaviour.
    QString policyJson;
    // Human-readable reason when !ok.
    QString error;
};

// Resolve one --access-policy argument:
//   ""                     -> no policy (enforcement off; the default)
//   "enforce"              -> kAccessPolicyEnforceEnvelope (deny-by-default)
//   text starting with '{' -> inline JSON, used as-is
//   anything else          -> a path to a JSON file, read from disk
//
// The result is parse-checked here; schema enforcement is the runtime's job.
AccessPolicyResolution resolveAccessPolicy(const QString& arg);

} // namespace LogosBasecamp

#endif // LOGOS_BASECAMP_ACCESS_POLICY_OPTION_H
