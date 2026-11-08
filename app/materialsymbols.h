#pragma once

#include <QString>
#include <QStringList>

// The bundled Material Symbols Outlined font (a static instance of Google's, Apache 2.0), for
// the in-stream menu's icons. Main thread.
class MaterialSymbols
{
public:
    // The font's family, registered with Qt on first use, or empty if it couldn't be loaded
    static QString family();

    // The text that draws an icon in the font, or empty for an unknown icon
    static QString glyph(const QString& name);

    // Every icon's name, in alphabetical order
    static const QStringList& names();
};
