#pragma once

// The writer has one canonical implementation. Keeping a second copy here
// previously left the Viewer project exposing a stale WVZ4 v13 writer while
// the runtime used v15.
#include "../wvz4_writer_typed.h"
