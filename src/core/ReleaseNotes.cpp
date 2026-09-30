// SPDX-License-Identifier: GPL-3.0-or-later
#include "ReleaseNotes.h"

#include <QCoreApplication>

QStringList ReleaseNotes::current()
{
    auto tr = [](const char* text) { return QCoreApplication::translate("ReleaseNotes", text); };
    return {
        tr("A first start opens on The Buzzer, 4625 kHz, with Follow rig off, On air only on, and a free "
           "public KiwiSDR receiver picked at random"),
        tr("Shift+Down and Shift+Up tune to the next station down or up the list, with or without a rig; "
           "the mouse wheel over a digit of the big frequency tunes the rig too"),
        tr("Stations in the aeronautical, maritime and amateur bands whose schedule gives no mode are shown "
           "as USB instead of AM"),
        tr("KiwiSDR: opening the receiver's own web page while listening to it hands the connection to the "
           "browser, as most receivers take one connection per address"),
    };
}
