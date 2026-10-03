#ifndef LOGOS_BASECAMP_MODULE_CONFIG_OPTION_H
#define LOGOS_BASECAMP_MODULE_CONFIG_OPTION_H

#include <QString>

namespace LogosBasecamp {

// Outcome of resolving the operator's --module-config request.
struct ModuleConfigResolution {
    // False when the operator asked for something we could not honour (an
    // unreadable file, malformed JSON, not a mapping of module names). The app
    // aborts rather than start modules without the configuration asked for.
    bool ok = true;
    // {"<module>": <document>, ...}; empty for none.
    QString configJson;
    QString error;
};

// Resolve one --module-config argument:
//   ""                     -> none (the default)
//   text starting with '{' -> inline JSON
//   anything else          -> a path to a JSON file, read from disk
// It must be a JSON object keyed by module names; each document is the module's.
ModuleConfigResolution resolveModuleConfig(const QString& arg);

} // namespace LogosBasecamp

#endif // LOGOS_BASECAMP_MODULE_CONFIG_OPTION_H
