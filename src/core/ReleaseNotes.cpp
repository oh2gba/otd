// SPDX-License-Identifier: GPL-3.0-or-later
#include "ReleaseNotes.h"

#include <QCoreApplication>

QStringList ReleaseNotes::current()
{
    auto tr = [](const char* text) { return QCoreApplication::translate("ReleaseNotes", text); };
    return {
        tr("Hop from station to station: Shift+Down and Shift+Up jump to the next station in the list, "
           "with or without a radio connected."),
        tr("Turning the mouse wheel over a digit of the big frequency now tunes your radio too."),
        tr("With a radio connected, the station list now appears straight away on your last frequency "
           "instead of staying empty until the radio answers."),
        tr("Aircraft, ship and other utility stations now show USB instead of AM, so the online receiver "
           "plays them the way they are sent."),
        tr("Opening a KiwiSDR's web page while you listen to it now hands the stream over to your browser, "
           "so the two no longer fight over the one connection most receivers allow."),
        tr("New installs start on The Buzzer (4625 kHz), which is on the air day and night, with a free "
           "online receiver already picked, so there is something to hear right away."),
    };
}
