#pragma once

// The writer has one canonical implementation. Keeping a second copy here
// previously left the Viewer project exposing a stale writer instead of the
// canonical WVZ4 v17 implementation.
#include "../wvz4_writer_typed.h"
