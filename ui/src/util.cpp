module;
#include "waywallen/util.moc.h"

module waywallen;
import qextra;
import :util;
import :app;

using namespace Qt::Literals::StringLiterals;

namespace waywallen
{

Util* Util::instance() {
    static Util* the = new Util(App::instance());
    return the;
}

Util* Util::create(QQmlEngine*, QJSEngine*) {
    auto t = instance();
    QJSEngine::setObjectOwnership(t, QJSEngine::CppOwnership);
    return t;
}

Util::Util(QObject* parent): QObject(parent) {}
Util::~Util() = default;

Util::Desktop Util::desktop() const {
    // Mirrors `display::spawner::detect_de` on the daemon side: any
    // `XDG_CURRENT_DESKTOP` token matching a known DE wins. Cached on
    // first call since session DE doesn't change after process startup.
    static const Desktop result = []() {
        const QByteArray xdg = qgetenv("XDG_CURRENT_DESKTOP");
        if (xdg.isEmpty()) return Desktop::Unknown;
        const QString lower  = QString::fromLocal8Bit(xdg).toLower();
        const auto    tokens = lower.split(QLatin1Char(':'), Qt::SkipEmptyParts);
        for (const auto& t : tokens) {
            const auto v = t.trimmed();
            if (v == QLatin1String("kde")) return Desktop::Kde;
            if (v == QLatin1String("hyprland")) return Desktop::Hyprland;
            if (v == QLatin1String("sway")) return Desktop::Sway;
            if (v == QLatin1String("niri")) return Desktop::Niri;
            if (v == QLatin1String("gnome")) return Desktop::Gnome;
        }
        return Desktop::Unknown;
    }();
    return result;
}

bool Util::supportsDisplayRename() const {
    switch (desktop()) {
    case Desktop::Hyprland:
    case Desktop::Sway:
    case Desktop::Niri: return true;
    default: return false;
    }
}

bool Util::isFlatpak() const {
    static const bool result = ! qgetenv("FLATPAK_ID").isEmpty();
    return result;
}

// --- BBCode → display blocks ----------------------------------------------
//
// Provider descriptions are walked with a tag stack rather than paired
// regexes, so sloppy markup comes out the way Steam renders it: an open tag
// without its close stays open to the end (`[b]x[b]` bolds the rest), a
// stray close is dropped and unknown tags stay literal. [img] splits the
// output into blocks; the panel lays images out as items because StyledText
// inline images overlap fixed-height lines.

namespace
{

QString escapeHtml(QStringView s) {
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        if (c == u'&')
            out.append(u"&amp;"_s);
        else if (c == u'<')
            out.append(u"&lt;"_s);
        else if (c == u'>')
            out.append(u"&gt;"_s);
        else if (c == u'"')
            out.append(u"&quot;"_s);
        else
            out.append(c);
    }
    return out;
}

QString linkifyBareUrls(const QString& text) {
    static const QRegularExpression re(QStringLiteral("https?://[^\\s<\\[\\]]+"));
    QString                         out;
    out.reserve(text.size());
    qsizetype cursor = 0;
    auto      it     = re.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        out.append(text.mid(cursor, m.capturedStart() - cursor));
        out.append(QStringLiteral("<a href=\"%1\">%1</a>").arg(m.captured()));
        cursor = m.capturedEnd();
    }
    out.append(text.mid(cursor));
    return out;
}

bool isHttpUrl(QStringView s) {
    return s.startsWith(u"http://", Qt::CaseInsensitive) ||
           s.startsWith(u"https://", Qt::CaseInsensitive);
}

struct BbOpen {
    QString name;
    QString open;
    QString close;
    QString href;
};

class BbParser {
public:
    explicit BbParser(QString src): m_src(std::move(src)) { m_src.replace(u"\r\n"_s, u"\n"_s); }

    QVariantList run() {
        qsizetype pos = 0;
        while (pos < m_src.size()) {
            const auto bracket = m_src.indexOf(u'[', pos);
            if (bracket < 0) {
                text(QStringView(m_src).mid(pos));
                break;
            }
            text(QStringView(m_src).mid(pos, bracket - pos));
            pos = tag(bracket);
        }
        flush();
        return m_blocks;
    }

private:
    // Handles the tag at `at` and returns where parsing resumes.
    qsizetype tag(qsizetype at) {
        static const QRegularExpression re(
            QStringLiteral("\\[(/?)([a-z0-9]+|\\*)(?:=([^\\]\\n]*))?\\]"),
            QRegularExpression::CaseInsensitiveOption);
        const auto m = re.match(m_src,
                                at,
                                QRegularExpression::NormalMatch,
                                QRegularExpression::AnchorAtOffsetMatchOption);
        if (! m.hasMatch()) {
            text(QStringView(m_src).mid(at, 1));
            return at + 1;
        }
        const bool    closing = ! m.capturedView(1).isEmpty();
        const QString name    = m.captured(2).toLower();
        const QString arg     = m.captured(3).trimmed();
        const auto    end     = m.capturedEnd();

        if (closing) {
            if (name == u"*"_s || name == u"hr"_s || name == u"url"_s || pairs().contains(name)) {
                close(name);
                return end;
            }
            text(m.capturedView());
            return end;
        }

        if (name == u"noparse"_s || name == u"img"_s || (name == u"url"_s && arg.isEmpty())) {
            // Raw payload up to the matching close; without one the tag
            // is dropped and the payload parsed as plain text.
            const auto closeTag = u"[/"_s + name + u']';
            const auto stop     = m_src.indexOf(closeTag, end, Qt::CaseInsensitive);
            if (stop < 0) return end;
            const auto raw = QStringView(m_src).mid(end, stop - end);
            if (name == u"noparse"_s) {
                literal(raw);
            } else if (name == u"img"_s) {
                const auto url = raw.trimmed().toString();
                if (isHttpUrl(url))
                    image(url);
                else
                    text(raw);
            } else {
                const auto url = raw.trimmed().toString();
                m_body.append(u"<a href=\""_s + escapeHtml(href(url)) + u"\">"_s + escapeHtml(url) +
                              u"</a>"_s);
                m_visible = true;
            }
            return stop + closeTag.size();
        }

        if (name == u"hr"_s) {
            // StyledText has no <hr>; the panel draws a divider instead.
            flush();
            m_blocks.append(QVariantMap { { u"type"_s, u"rule"_s } });
        } else if (name == u"*"_s) {
            if (inList()) {
                trimBreaks();
                m_body.append(u"<li>"_s);
            } else {
                m_body.append(u"•"_s);
            }
        } else if (name == u"url"_s || pairs().contains(name)) {
            // Nesting this deep is pathological; further opens are dropped.
            if (m_stack.size() >= kMaxDepth) return end;
            if (name == u"url"_s)
                push(
                    name, u"<a href=\""_s + escapeHtml(href(arg)) + u"\">"_s, u"</a>"_s, href(arg));
            else
                openPair(name);
        } else {
            text(m.capturedView());
        }
        return end;
    }

    // Plain open/close pairs. Headings drop a tier so provider content
    // does not dwarf the panel.
    static const QHash<QString, QStringList>& pairs() {
        static const QHash<QString, QStringList> map {
            { u"b"_s, { u"<b>"_s, u"</b>"_s } },
            { u"i"_s, { u"<i>"_s, u"</i>"_s } },
            { u"u"_s, { u"<u>"_s, u"</u>"_s } },
            { u"s"_s, { u"<s>"_s, u"</s>"_s } },
            { u"strike"_s, { u"<s>"_s, u"</s>"_s } },
            { u"code"_s, { u"<tt>"_s, u"</tt>"_s } },
            { u"spoiler"_s, { u"<font color=\"#888888\">"_s, u"</font>"_s } },
            // quote=author is not surfaced; StyledText has no block quote.
            { u"quote"_s, { u"<i>“"_s, u"”</i>"_s } },
            { u"h1"_s, { u"<h2>"_s, u"</h2>"_s } },
            { u"h2"_s, { u"<h3>"_s, u"</h3>"_s } },
            { u"h3"_s, { u"<h4>"_s, u"</h4>"_s } },
            { u"list"_s, { u"<ul>"_s, u"</ul>"_s } },
            { u"olist"_s, { u"<ol>"_s, u"</ol>"_s } },
            { u"table"_s, { QString(), QString() } },
            { u"tr"_s, { QString(), u"<br>"_s } },
            { u"th"_s, { u"<b>"_s, u"</b>&nbsp;&nbsp;&nbsp;"_s } },
            { u"td"_s, { QString(), u"&nbsp;&nbsp;&nbsp;"_s } },
        };
        return map;
    }

    void openPair(const QString& name) {
        const auto& html = pairs().value(name);
        if (name == u"list"_s || name == u"olist"_s) trimBreaks();
        push(name, html.at(0), html.at(1));
    }

    // Steam accepts scheme-less links such as `[url=store.steampowered.com]`.
    static QString href(QString url) {
        if (url.size() >= 2 && (url.front() == u'"' || url.front() == u'\'') &&
            url.back() == url.front())
            url = url.mid(1, url.size() - 2);
        if (! url.isEmpty() && ! url.contains(u"://"_s)) url.prepend(u"https://"_s);
        return url;
    }

    void push(const QString& name, const QString& open, const QString& close,
              const QString& link = {}) {
        m_stack.append({ name, open, close, link });
        m_body.append(open);
    }

    // Closes the innermost `name`; tags opened inside it are reopened after
    // it, as a browser does for misnested formatting. Stray closes vanish.
    void close(const QString& name) {
        qsizetype idx = m_stack.size() - 1;
        while (idx >= 0 && m_stack.at(idx).name != name) --idx;
        if (idx < 0) return;
        if (name == u"list"_s || name == u"olist"_s) trimBreaks();
        const auto inner = m_stack.mid(idx + 1);
        for (qsizetype i = m_stack.size() - 1; i >= idx; --i) m_body.append(m_stack.at(i).close);
        m_stack.resize(idx);
        for (const auto& t : inner) push(t.name, t.open, t.close, t.href);
    }

    bool inList() const {
        for (const auto& t : m_stack)
            if (t.name == u"list"_s || t.name == u"olist"_s) return true;
        return false;
    }

    bool inLink() const {
        for (const auto& t : m_stack)
            if (t.name == u"url"_s) return true;
        return false;
    }

    // Whitespace between list/table structure tags carries no content.
    bool inStructure() const {
        if (m_stack.isEmpty()) return false;
        const auto& top = m_stack.back().name;
        return top == u"list"_s || top == u"olist"_s || top == u"table"_s || top == u"tr"_s;
    }

    void text(QStringView s) {
        if (s.isEmpty()) return;
        if (s.trimmed().isEmpty() && inStructure()) return;
        auto html = escapeHtml(s);
        if (! inLink()) html = linkifyBareUrls(html);
        html.replace(u'\n', u"<br>"_s);
        m_body.append(html);
        if (! s.trimmed().isEmpty()) m_visible = true;
    }

    void literal(QStringView s) {
        auto html = escapeHtml(s);
        html.replace(u'\n', u"<br>"_s);
        m_body.append(html);
        if (! s.trimmed().isEmpty()) m_visible = true;
    }

    void trimBreaks() {
        while (m_body.endsWith(u"<br>"_s)) m_body.chop(4);
    }

    void image(const QString& url) {
        flush();
        QString link;
        for (const auto& t : m_stack)
            if (! t.href.isEmpty()) link = t.href;
        // `text` is the fallback shown when the image cannot be loaded.
        const auto fallback = u"<a href=\""_s + escapeHtml(link.isEmpty() ? url : link) + u"\">"_s +
                              escapeHtml(url) + u"</a>"_s;
        m_blocks.append(QVariantMap { { u"type"_s, u"image"_s },
                                      { u"url"_s, url },
                                      { u"link"_s, link },
                                      { u"text"_s, fallback } });
    }

    // Emits the pending text as a block, closing open tags; the next block
    // reopens them so formatting carries across images.
    void flush() {
        if (m_visible) {
            while (m_body.startsWith(u"<br>"_s)) m_body.remove(0, 4);
            trimBreaks();
            QString html = m_prefix + m_body;
            for (auto it = m_stack.crbegin(); it != m_stack.crend(); ++it) html.append(it->close);
            m_blocks.append(QVariantMap { { u"type"_s, u"text"_s }, { u"text"_s, html } });
        }
        m_prefix.clear();
        for (const auto& t : m_stack) m_prefix.append(t.open);
        m_body.clear();
        m_visible = false;
    }

    static constexpr qsizetype kMaxDepth = 64;

    QString       m_src;
    QList<BbOpen> m_stack;
    QString       m_prefix;
    QString       m_body;
    bool          m_visible { false };
    QVariantList  m_blocks;
};

} // namespace

QVariantList Util::bbcodeToBlocks(const QString& src) const {
    if (src.isEmpty()) return {};
    return BbParser(src).run();
}

// --- WE wire color round-trip --------------------------------------------

namespace
{

QList<double> parseWireColor(const QString& s) {
    QList<double>                   out;
    static const QRegularExpression reSpaces(QStringLiteral("\\s+"));
    const auto                      parts = s.trimmed().split(reSpaces, Qt::SkipEmptyParts);
    out.reserve(parts.size());
    for (const auto& p : parts) {
        bool         ok = false;
        const double v  = p.toDouble(&ok);
        if (! ok) return {};
        out.append(v);
    }
    return out;
}

double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

} // namespace

QColor Util::colorFromWire(const QString& s) const {
    const auto nums = parseWireColor(s);
    if (nums.size() < 3) return QColor::fromRgbF(0.0f, 0.0f, 0.0f, 1.0f);
    const double a = nums.size() >= 4 ? clamp01(nums[3]) : 1.0;
    return QColor::fromRgbF(static_cast<float>(clamp01(nums[0])),
                            static_cast<float>(clamp01(nums[1])),
                            static_cast<float>(clamp01(nums[2])),
                            static_cast<float>(a));
}

QString Util::colorToWire(const QColor& c, bool includeAlpha) const {
    const QString r = QString::number(c.redF(), 'f', 4);
    const QString g = QString::number(c.greenF(), 'f', 4);
    const QString b = QString::number(c.blueF(), 'f', 4);
    if (! includeAlpha) return r + QLatin1Char(' ') + g + QLatin1Char(' ') + b;
    const QString a = QString::number(c.alphaF(), 'f', 4);
    return r + QLatin1Char(' ') + g + QLatin1Char(' ') + b + QLatin1Char(' ') + a;
}

bool Util::colorHasAlpha(const QString& s) const { return parseWireColor(s).size() >= 4; }

} // namespace waywallen

#include "waywallen/util.moc.cpp"
