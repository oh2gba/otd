// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QStringList>

// What changed in this version, as the listener notices it: shown once
// after an upgrade. (The metainfo file carries the same list for the
// software stores, plus anything that is only inside.)
namespace ReleaseNotes
{
    QStringList current();
}
