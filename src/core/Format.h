// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

// How frequencies read on screen.
namespace Format
{
    // 7125.94 -> "7 125.940": three decimals, a space between the thousands
    // (always a space, whatever the machine's number format)
    QString kHz(double kHz);

    // The tuning step of one character of such a text ("6 070.000 kHz"):
    // 1000 for the 6, 10 for the 7, 0.001 for the last decimal; 0 when the
    // character is not a digit.
    double digitStep(const QString& text, int index);
}
