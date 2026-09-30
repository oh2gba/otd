// SPDX-License-Identifier: GPL-3.0-or-later
#include "Format.h"

#include <cmath>

QString Format::kHz(double kHz)
{
    QString s = QString::number(kHz, 'f', 3);
    const int dot = s.indexOf(QLatin1Char('.'));
    for (int i = dot - 3; i > 0; i -= 3)
        s.insert(i, QLatin1Char(' '));
    return s;
}

double Format::digitStep(const QString& text, int index)
{
    if (index < 0 || index >= text.size() || !text.at(index).isDigit())
        return 0.0;
    // the digit's weight: count the digits between it and the decimal point
    const int dot = text.indexOf(QLatin1Char('.'));
    int place = 0;
    if (index < dot)
    {
        for (int i = index + 1; i < dot; ++i)
            if (text.at(i).isDigit())
                ++place;
    }
    else
    {
        for (int i = dot + 1; i <= index; ++i)
            if (text.at(i).isDigit())
                --place;
    }
    return std::pow(10.0, place);
}
