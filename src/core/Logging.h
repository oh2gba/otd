// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

// Everything the program and Qt report, written to a file with a time
// stamp: for looking into a problem on a machine one cannot watch
// (otd --log file). Switches on the detailed messages of otd's own
// categories (otd.*) and Qt's sound output (qt.multimedia.*).
namespace Logging
{
bool toFile(const QString& path);
}
