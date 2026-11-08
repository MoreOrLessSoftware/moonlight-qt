#include "materialsymbols.h"
#include "path.h"

#include "SDL_compat.h"

#include <QFontDatabase>
#include <QHash>

namespace {

struct Symbols
{
    QString family;
    QHash<QString, uint> codepoints;
    QStringList names;
};

const Symbols& symbols()
{
    static Symbols symbols = [] {
        Symbols s;

        int id = QFontDatabase::addApplicationFontFromData(Path::readDataFile("MaterialSymbolsOutlined.ttf"));
        if (id >= 0 && !QFontDatabase::applicationFontFamilies(id).isEmpty()) {
            s.family = QFontDatabase::applicationFontFamilies(id).first();
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to load the Material Symbols font; the overlay menu will have no icons");
        }

        // Each line is "<name> <hex codepoint>"
        const QList<QByteArray> lines = Path::readDataFile("MaterialSymbolsOutlined.codepoints").split('\n');
        for (const QByteArray& line : lines) {
            QList<QByteArray> fields = line.trimmed().split(' ');
            if (fields.size() == 2) {
                bool ok;
                uint codepoint = fields[1].toUInt(&ok, 16);
                if (ok) {
                    QString name = QString::fromLatin1(fields[0]);
                    if (!s.codepoints.contains(name)) {
                        s.names.append(name);
                    }
                    s.codepoints.insert(name, codepoint);
                }
            }
        }
        s.names.sort();

        return s;
    }();

    return symbols;
}

}

QString MaterialSymbols::family()
{
    return symbols().family;
}

QString MaterialSymbols::glyph(const QString& name)
{
    uint codepoint = symbols().codepoints.value(name, 0);
    if (codepoint == 0) {
        return QString();
    }

    // Some of the icons are outside the Basic Multilingual Plane
    char32_t ucs4 = codepoint;
    return QString::fromUcs4(&ucs4, 1);
}

const QStringList& MaterialSymbols::names()
{
    return symbols().names;
}
