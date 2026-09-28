// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

// CIRAF zones, the numbered target areas of the ITU that HFCC gives: a
// comma list of zones ("18,27,28"), a zone with a quadrant ("41NW") or a
// range ("27-29"). The names come from data/ciraf.json.
namespace Ciraf
{
// "40E,41NW" -> "Iran, Afghanistan (east), South Asia (northwest)", all 85
// zones -> "worldwide". With details, each zone's area after its number:
// "40 east: Afghanistan, Iran; 41 northwest: Bangladesh, ...". Empty when
// none of it is a known zone.
QString text(const QString& zones, bool details = false);
}
