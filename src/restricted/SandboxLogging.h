#pragma once

#include <QLoggingCategory>

// One logging category for the entire ui_qml sandbox — url interceptor, network
// deny factory, engine setup. Defined once in SandboxLogging.cpp so DenyAllReply
// (which lives in a translation unit that used to have no logging at all) can
// share the same category as RestrictedUrlInterceptor without duplicating the
// Q_LOGGING_CATEGORY definition.
Q_DECLARE_LOGGING_CATEGORY(lcBasecampSandbox)
