#include "SurfaceBackend.h"

// The interface is header-only; this translation unit exists so that
// AUTOMOC pairs the Q_OBJECT header with a source and emits its meta-object.
SurfaceBackend::~SurfaceBackend() = default;
