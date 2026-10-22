// MiniPDF - ein schlanker PDF-Betrachter im Stil von SumatraPDF
// Qt6 (Widgets) + Poppler-Qt6.  Lizenz: Public Domain / CC0.
//
// Merkmale: Tabs, kontinuierliches Scrollen, Doppelseitenansicht, Zoom-Modi,
// Seitenzahl-Eingabe, Volltextsuche, Inhaltsverzeichnis (F12), Links,
// Text kopieren (Shift+Ziehen), Auto-Reload bei Dateiänderung,
// letzte Seite/Zoom pro Datei merken, Drag&Drop, Passwortabfrage.

#include <QtWidgets>
#include <QtGui>
#include <QtPrintSupport>
#include <QDesktopServices>
#include <poppler-qt6.h>

#include <algorithm>
#include <climits>
#include <map>
#include <memory>
#include <vector>

namespace {
const QString kAppName = QStringLiteral("MiniPDF");
constexpr int kMargin = 10;                 // Rand um die Seiten
constexpr int kGap = 8;                     // Abstand zwischen Seiten
constexpr double kPxPerPt100 = 96.0 / 72.0; // 100 % = 96 dpi
constexpr double kMinZoom = 0.05;
constexpr double kMaxZoom = 6.4;
const QString kFitPage = QStringLiteral("Seite einpassen");
const QString kFitWidth = QStringLiteral("Breite einpassen");
} // namespace

enum class ZoomMode : int { Custom = 0, FitPage = 1, FitWidth = 2 };

// Öffnet ein Dokument, fragt ggf. nach einem Passwort.
static std::unique_ptr<Poppler::Document> openDocument(const QString &path, QString &password,
                                                       QWidget *parent, QString *error,
                                                       bool interactive = true)
{
    auto doc = Poppler::Document::load(path);
    if (!doc) {
        if (error) *error = QStringLiteral("Die Datei konnte nicht geöffnet werden:\n%1").arg(path);
        return nullptr;
    }
    if (doc->isLocked()) {
        for (int attempt = 0; attempt < 3 && doc->isLocked(); ++attempt) {
            if (password.isEmpty()) {
                if (!interactive) return nullptr;
                bool ok = false;
                password = QInputDialog::getText(parent, kAppName,
                                                 QStringLiteral("Das Dokument ist passwortgeschützt.\nPasswort:"),
                                                 QLineEdit::Password, QString(), &ok);
                if (!ok) {
                    if (error) error->clear();
                    return nullptr;
                }
            }
            doc->unlock(password.toUtf8(), password.toUtf8());
            if (doc->isLocked()) password.clear();
        }
        if (doc->isLocked()) {
            if (error) *error = QStringLiteral("Falsches Passwort.");
            return nullptr;
        }
    }
    if (doc->numPages() <= 0) {
        if (error) *error = QStringLiteral("Das Dokument enthält keine Seiten.");
        return nullptr;
    }
    return doc;
}

// ---------------------------------------------------------------------------
// DocumentView: eine scrollbare, kontinuierliche Seitenansicht
// ---------------------------------------------------------------------------
class DocumentView : public QAbstractScrollArea
{
    Q_OBJECT
    struct Anchor {
        int page = -1;
        double fx = 0.5, fy = 0.5;
    };

public:
    explicit DocumentView(QWidget *parent = nullptr)
        : QAbstractScrollArea(parent), m_cache(192 * 1024) // ~192 MB
    {
        setFrameShape(QFrame::NoFrame);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setFocusPolicy(Qt::StrongFocus);
        viewport()->setMouseTracking(true);
        viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
        verticalScrollBar()->setSingleStep(48);
        horizontalScrollBar()->setSingleStep(48);

        connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
            viewport()->update();
            updateCurrentPage();
        });
        connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { viewport()->update(); });

        m_reloadTimer.setSingleShot(true);
        m_reloadTimer.setInterval(600);
        connect(&m_reloadTimer, &QTimer::timeout, this, &DocumentView::reload);
        connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] {
            m_reloadTries = 0;
            m_reloadTimer.start();
        });
    }

    // --- Laden -----------------------------------------------------------
    bool load(const QString &path, QString *error)
    {
        QString pw;
        auto doc = openDocument(path, pw, this, error);
        if (!doc) return false;
        m_path = QFileInfo(path).absoluteFilePath();
        m_password = pw;
        adopt(std::move(doc));
        m_watcher.addPath(m_path);
        m_pendingPage = 0;
        relayout(Anchor{});
        m_current = 0;
        return true;
    }

    void applyState(ZoomMode mode, double zoom, bool facing, int page)
    {
        m_mode = mode;
        m_zoom = qBound(kMinZoom, zoom, kMaxZoom);
        m_facing = facing;
        relayout(Anchor{});
        m_pendingPage = qBound(0, page, qMax(0, m_pageCount - 1));
        scrollToPage(m_pendingPage);
    }

    void reload()
    {
        if (m_path.isEmpty()) return;
        if (QFile::exists(m_path) && !m_watcher.files().contains(m_path)) m_watcher.addPath(m_path);
        QString err;
        auto doc = openDocument(m_path, m_password, nullptr, &err, false);
        if (!doc) {
            if (++m_reloadTries < 10) m_reloadTimer.start(); // Datei wird evtl. gerade geschrieben
            return;
        }
        const Anchor a = anchor();
        adopt(std::move(doc));
        relayout(a);
        updateCurrentPage();
    }

    // --- Zustand ---------------------------------------------------------
    QString path() const { return m_path; }
    int pageCount() const { return m_pageCount; }
    int currentPage() const { return m_current; }
    double zoom() const { return m_zoom; }
    ZoomMode mode() const { return m_mode; }
    bool facing() const { return m_facing; }

    void emitState()
    {
        emit pageChanged(m_current, m_pageCount);
        emit zoomChanged(m_zoom, int(m_mode));
    }

    // --- Zoom / Layout ---------------------------------------------------
    void setZoomMode(ZoomMode m)
    {
        m_mode = m;
        relayout(anchor());
    }
    void setZoom(double z)
    {
        m_mode = ZoomMode::Custom;
        m_zoom = qBound(kMinZoom, z, kMaxZoom);
        relayout(anchor());
    }
    void zoomBy(double factor) { setZoom(m_zoom * factor); }
    void setFacing(bool f)
    {
        if (m_facing == f) return;
        m_facing = f;
        relayout(anchor());
    }

    // --- Navigation ------------------------------------------------------
    void goToPage(int page)
    {
        m_pendingPage = -1;
        scrollToPage(page);
    }
    void nextPage() { goToPage(m_current + (m_facing ? 2 : 1)); }
    void prevPage() { goToPage(m_current - (m_facing ? 2 : 1)); }

    // --- Inhaltsverzeichnis ---------------------------------------------
    QVector<Poppler::OutlineItem> outline() const
    {
        return m_doc ? m_doc->outline() : QVector<Poppler::OutlineItem>();
    }
    int outlineTargetPage(const Poppler::OutlineItem &it) const
    {
        const auto dest = it.destination();
        return dest ? dest->pageNumber() - 1 : -1;
    }

    // --- Suche -----------------------------------------------------------
    void setSearchTerm(const QString &t)
    {
        m_term = t;
        m_hits.clear();
        m_hitPage = m_hitIdx = -1;
        viewport()->update();
    }

    bool findNext(bool forward)
    {
        if (!m_doc || m_term.isEmpty()) return false;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        int page = m_hitPage >= 0 ? m_hitPage : m_current;
        int idx = m_hitPage >= 0 ? m_hitIdx : (forward ? -1 : INT_MAX);
        bool found = false;
        for (int n = 0; n <= m_pageCount && !found; ++n) {
            const QList<QRectF> h = hitsFor(page);
            const int cand = forward ? idx + 1 : qMin(idx, int(h.size())) - 1;
            if (cand >= 0 && cand < h.size()) {
                m_hitPage = page;
                m_hitIdx = cand;
                found = true;
                break;
            }
            page = (page + (forward ? 1 : -1) + m_pageCount) % m_pageCount;
            idx = forward ? -1 : INT_MAX;
        }
        QApplication::restoreOverrideCursor();
        if (found) showHit();
        viewport()->update();
        return found;
    }

    // --- Drucken ---------------------------------------------------------
    void printDocument()
    {
        if (!m_doc) return;
        QPrinter printer(QPrinter::HighResolution);
        printer.setDocName(QFileInfo(m_path).fileName());
        printer.setFromTo(1, m_pageCount);
        QPrintDialog dlg(&printer, this);
        dlg.setWindowTitle(QStringLiteral("Drucken"));
        dlg.setOptions(QAbstractPrintDialog::PrintToFile | QAbstractPrintDialog::PrintPageRange |
                       QAbstractPrintDialog::PrintCollateCopies | QAbstractPrintDialog::PrintCurrentPage);
        dlg.setMinMax(1, m_pageCount);
        if (dlg.exec() != QDialog::Accepted) return;

        int from = 1, to = m_pageCount;
        if (printer.printRange() == QPrinter::PageRange) {
            from = qBound(1, printer.fromPage(), m_pageCount);
            to = qBound(from, printer.toPage(), m_pageCount);
        } else if (printer.printRange() == QPrinter::CurrentPage) {
            from = to = m_current + 1;
        }
        QList<int> pages;
        for (int i = from; i <= to; ++i) pages << i - 1;
        if (printer.pageOrder() == QPrinter::LastPageFirst) std::reverse(pages.begin(), pages.end());

        // Kopien selbst erzeugen, falls der Druckertreiber das nicht übernimmt
        const int copies = printer.supportsMultipleCopies() ? 1 : qMax(1, printer.copyCount());
        QList<int> sequence;
        if (printer.collateCopies() || copies == 1) {
            for (int c = 0; c < copies; ++c) sequence << pages;
        } else {
            for (int p : pages) for (int c = 0; c < copies; ++c) sequence << p;
        }

        QPainter painter;
        if (!painter.begin(&printer)) {
            QMessageBox::warning(this, kAppName, QStringLiteral("Der Drucker konnte nicht initialisiert werden."));
            return;
        }
        QProgressDialog progress(QStringLiteral("Drucke…"), QStringLiteral("Abbrechen"), 0, sequence.size(), this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(400);
        bool aborted = false;
        for (int n = 0; n < sequence.size(); ++n) {
            progress.setValue(n); // verarbeitet auch Ereignisse (Abbrechen-Knopf)
            if (progress.wasCanceled()) { aborted = true; break; }
            if (n > 0) printer.newPage();
            printPage(painter, printer, sequence[n]);
        }
        progress.setValue(sequence.size());
        if (aborted) printer.abort();
        painter.end();
    }

signals:
    void pageChanged(int current, int total);
    void zoomChanged(double zoom, int mode);
    void textCopied(const QString &text);

protected:
    // --- Zeichnen --------------------------------------------------------
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(viewport());
        p.fillRect(viewport()->rect(), QColor(0xE9, 0xE9, 0xE9));
        if (!m_doc) return;
        const int sx = horizontalScrollBar()->value(), sy = verticalScrollBar()->value();
        const QRect vp = viewport()->rect();
        const int n = qMin(m_rects.size(), m_pageCount);
        for (int i = 0; i < n; ++i) {
            const QRect dst = m_rects[i].translated(-sx, -sy);
            if (!dst.intersects(vp)) continue;
            drawPage(p, i, dst);
            drawHits(p, i, dst);
        }
        if (!m_sel.isNull()) p.fillRect(m_sel.translated(-sx, -sy), QColor(51, 153, 255, 70));
    }

    void resizeEvent(QResizeEvent *e) override
    {
        QAbstractScrollArea::resizeEvent(e);
        if (!m_doc) return;
        if (m_pendingPage >= 0) {
            relayout(Anchor{});
            scrollToPage(m_pendingPage);
        } else {
            relayout(anchor());
        }
    }

    // --- Eingaben --------------------------------------------------------
    void wheelEvent(QWheelEvent *e) override
    {
        m_pendingPage = -1;
        if (e->modifiers() & Qt::ControlModifier) {
            const int d = e->angleDelta().y();
            if (d) zoomBy(d > 0 ? 1.1 : 1.0 / 1.1);
            e->accept();
            return;
        }
        if (m_mode == ZoomMode::FitPage && m_doc) {
            m_wheelAcc += e->angleDelta().y();
            while (m_wheelAcc >= 120) { m_wheelAcc -= 120; prevPage(); }
            while (m_wheelAcc <= -120) { m_wheelAcc += 120; nextPage(); }
            e->accept();
            return;
        }
        QAbstractScrollArea::wheelEvent(e);
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        m_pendingPage = -1;
        QScrollBar *vs = verticalScrollBar();
        const bool plain = e->modifiers() == Qt::NoModifier;
        if (m_mode == ZoomMode::FitPage && m_doc) {
            const bool shift = e->modifiers() == Qt::ShiftModifier;
            if ((plain || shift) && e->key() == Qt::Key_Space) { shift ? prevPage() : nextPage(); return; }
            if (plain && (e->key() == Qt::Key_Down || e->key() == Qt::Key_PageDown)) { nextPage(); return; }
            if (plain && (e->key() == Qt::Key_Up || e->key() == Qt::Key_PageUp)) { prevPage(); return; }
        }
        switch (e->key()) {
        case Qt::Key_N:
            if (plain) { nextPage(); return; }
            break;
        case Qt::Key_P:
            if (plain) { prevPage(); return; }
            break;
        case Qt::Key_J:
            if (plain) { vs->setValue(vs->value() + vs->singleStep() * 2); return; }
            break;
        case Qt::Key_K:
            if (plain) { vs->setValue(vs->value() - vs->singleStep() * 2); return; }
            break;
        case Qt::Key_Space:
            vs->setValue(vs->value() + ((e->modifiers() & Qt::ShiftModifier) ? -1 : 1) * (viewport()->height() - 40));
            return;
        case Qt::Key_Right:
            if (plain && horizontalScrollBar()->maximum() == 0) { nextPage(); return; }
            break;
        case Qt::Key_Left:
            if (plain && horizontalScrollBar()->maximum() == 0) { prevPage(); return; }
            break;
        default:
            break;
        }
        QAbstractScrollArea::keyPressEvent(e);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) { QAbstractScrollArea::mousePressEvent(e); return; }
        m_pendingPage = -1;
        setFocus();
        m_pressPos = m_lastPos = e->pos();
        m_dragging = false;
        m_selecting = (e->modifiers() & Qt::ShiftModifier);
        m_sel = QRect();
        if (m_selecting) m_selStart = e->pos() + scrollOffset();
        viewport()->update();
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (e->buttons() & Qt::LeftButton) {
            if (m_selecting) {
                m_sel = QRect(m_selStart, e->pos() + scrollOffset()).normalized();
                viewport()->update();
            } else {
                if (!m_dragging && (e->pos() - m_pressPos).manhattanLength() > 4) {
                    m_dragging = true;
                    viewport()->setCursor(Qt::ClosedHandCursor);
                }
                if (m_dragging) {
                    const QPoint d = e->pos() - m_lastPos;
                    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
                    verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
                }
                m_lastPos = e->pos();
            }
            return;
        }
        viewport()->setCursor(linkAt(e->pos() + scrollOffset()) ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) { QAbstractScrollArea::mouseReleaseEvent(e); return; }
        if (m_selecting) {
            m_selecting = false;
            copySelection();
            return;
        }
        if (m_dragging) {
            m_dragging = false;
            viewport()->setCursor(Qt::ArrowCursor);
            return;
        }
        if (Poppler::Link *l = linkAt(e->pos() + scrollOffset())) activateLink(l);
    }

private:
    QPoint scrollOffset() const { return QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value()); }

    void adopt(std::unique_ptr<Poppler::Document> doc)
    {
        m_cache.clear();
        m_hits.clear();
        m_links.clear();
        m_hitPage = m_hitIdx = -1;
        m_sel = QRect();
        m_doc = std::move(doc);
        m_doc->setRenderHint(Poppler::Document::Antialiasing, true);
        m_doc->setRenderHint(Poppler::Document::TextAntialiasing, true);
        m_pageCount = m_doc->numPages();
        m_sizes.resize(m_pageCount);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        for (int i = 0; i < m_pageCount; ++i) {
            auto page = m_doc->page(i);
            QSizeF s = page ? page->pageSizeF() : QSizeF();
            m_sizes[i] = s.isEmpty() ? QSizeF(595, 842) : s;
        }
        QApplication::restoreOverrideCursor();
    }

    // Seite, in deren Nähe der Mittelpunkt des Viewports liegt
    Anchor anchor() const
    {
        Anchor a;
        if (m_rects.isEmpty() || m_rects.size() != m_pageCount) return a;
        const QPoint c(horizontalScrollBar()->value() + m_vpSize.width() / 2,
                       verticalScrollBar()->value() + m_vpSize.height() / 2);
        int best = 0, bestD = INT_MAX;
        for (int i = 0; i < m_rects.size(); ++i) {
            const QRect &r = m_rects[i];
            const int dx = qMax(qMax(r.left() - c.x(), c.x() - r.right()), 0);
            const int dy = qMax(qMax(r.top() - c.y(), c.y() - r.bottom()), 0);
            if (dx + dy < bestD) { bestD = dx + dy; best = i; }
        }
        const QRect &r = m_rects[best];
        a.page = best;
        a.fx = double(c.x() - r.left()) / qMax(1, r.width());
        a.fy = double(c.y() - r.top()) / qMax(1, r.height());
        return a;
    }

    void applyAnchor(const Anchor &a)
    {
        if (a.page < 0 || a.page >= m_rects.size()) return;
        const QRect &r = m_rects[a.page];
        const int cx = r.left() + int(a.fx * r.width());
        const int cy = r.top() + int(a.fy * r.height());
        horizontalScrollBar()->setValue(cx - viewport()->width() / 2);
        verticalScrollBar()->setValue(cy - viewport()->height() / 2);
    }

    void relayout(const Anchor &a)
    {
        if (!m_doc || m_pageCount <= 0) return;
        const int vw = viewport()->width(), vh = viewport()->height();
        const int per = m_facing ? 2 : 1;

        double maxRowW = 1, maxPageH = 1;
        for (int i = 0; i < m_pageCount; i += per) {
            double rw = 0;
            for (int j = i; j < qMin(i + per, m_pageCount); ++j) {
                rw += m_sizes[j].width();
                maxPageH = qMax(maxPageH, m_sizes[j].height());
            }
            maxRowW = qMax(maxRowW, rw);
        }

        const double fitW = qMax(50.0, double(vw - 2 * kMargin - (per - 1) * kGap)) / maxRowW;
        if (m_mode == ZoomMode::FitWidth)
            m_scale = fitW;
        else if (m_mode == ZoomMode::FitPage)
            m_scale = qMin(fitW, qMax(50.0, double(vh - 2 * kMargin)) / maxPageH);
        else
            m_scale = m_zoom * kPxPerPt100;
        m_scale = qBound(kMinZoom * kPxPerPt100, m_scale, kMaxZoom * kPxPerPt100);
        m_zoom = m_scale / kPxPerPt100;

        struct Row { int first, count, w, h; };
        QVector<Row> rows;
        int maxRowPx = 0;
        for (int i = 0; i < m_pageCount; i += per) {
            Row r{i, qMin(per, m_pageCount - i), 0, 0};
            for (int j = i; j < i + r.count; ++j) {
                r.w += qMax(1, qRound(m_sizes[j].width() * m_scale)) + (j > i ? kGap : 0);
                r.h = qMax(r.h, qMax(1, qRound(m_sizes[j].height() * m_scale)));
            }
            if (m_mode == ZoomMode::FitPage) r.h = qMax(r.h, vh - 2 * kMargin);
            maxRowPx = qMax(maxRowPx, r.w);
            rows.push_back(r);
        }

        const int totalW = qMax(vw, maxRowPx + 2 * kMargin);
        const int rowGap = (m_mode == ZoomMode::FitPage) ? qMax(kGap, vh) : kGap;
        m_rects.resize(m_pageCount);
        int y = kMargin;
        for (const Row &r : rows) {
            int x = (totalW - r.w) / 2;
            for (int j = r.first; j < r.first + r.count; ++j) {
                const int w = qMax(1, qRound(m_sizes[j].width() * m_scale));
                const int h = qMax(1, qRound(m_sizes[j].height() * m_scale));
                m_rects[j] = QRect(x, y + (r.h - h) / 2, w, h);
                x += w + kGap;
            }
            y += r.h + rowGap;
        }
        m_contentW = totalW;
        m_contentH = y - rowGap + kMargin;
        m_vpSize = viewport()->size();

        verticalScrollBar()->setRange(0, qMax(0, m_contentH - vh));
        verticalScrollBar()->setPageStep(vh);
        horizontalScrollBar()->setRange(0, qMax(0, m_contentW - vw));
        horizontalScrollBar()->setPageStep(vw);
        m_sel = QRect();

        applyAnchor(a);
        viewport()->update();
        updateCurrentPage();
        emit zoomChanged(m_zoom, int(m_mode));
    }

    void updateCurrentPage()
    {
        if (m_rects.isEmpty() || m_rects.size() != m_pageCount) return;
        const QRect view(horizontalScrollBar()->value(), verticalScrollBar()->value(),
                         viewport()->width(), viewport()->height());
        int best = -1;
        qint64 bestArea = 0;
        for (int i = 0; i < m_rects.size(); ++i) {
            const QRect r = m_rects[i] & view;
            if (r.isEmpty()) continue;
            const qint64 area = qint64(r.width()) * r.height();
            if (area > bestArea) { bestArea = area; best = i; }
        }
        QScrollBar *vs = verticalScrollBar();
        if (vs->maximum() > 0 && vs->value() >= vs->maximum()) best = m_pageCount - 1;
        if (best >= 0 && best != m_current) {
            m_current = best;
            emit pageChanged(m_current, m_pageCount);
        }
    }

    void scrollToPage(int page)
    {
        if (m_rects.isEmpty() || m_rects.size() != m_pageCount) return;
        page = qBound(0, page, m_pageCount - 1);
        if (m_mode == ZoomMode::FitPage)
            verticalScrollBar()->setValue(m_rects[page].center().y() - viewport()->height() / 2);
        else
            verticalScrollBar()->setValue(m_rects[page].top() - kMargin);
        if (m_current != page) {
            m_current = page;
            emit pageChanged(m_current, m_pageCount);
        }
    }

    // Eine Seite auf das aktuelle Druckblatt bringen: einpassen (nie vergrößern),
    // zentrieren, bei Hoch-/Querformat-Unterschied um 90° drehen.
    void printPage(QPainter &painter, QPrinter &printer, int idx)
    {
        const QSizeF ps = m_sizes[idx]; // in Punkt
        const QSize area(painter.device()->width(), painter.device()->height());
        const double res = printer.resolution();
        const bool rotate = (ps.width() > ps.height()) != (area.width() > area.height());
        const double aw = rotate ? area.height() : area.width();
        const double ah = rotate ? area.width() : area.height();
        const double s = qMin(qMin(aw / ps.width(), ah / ps.height()), res / 72.0); // Geräte-Pixel pro Punkt
        const double ox = (aw - ps.width() * s) / 2.0, oy = (ah - ps.height() * s) / 2.0;
        const double rpp = qMin(s, 600.0 / 72.0); // Render-Pixel pro Punkt (max. 600 dpi)
        const double ratio = s / rpp;
        const double dpi = rpp * 72.0;
        const int W = qMax(1, qRound(ps.width() * rpp));
        const int H = qMax(1, qRound(ps.height() * rpp));

        painter.save();
        if (rotate) {
            painter.translate(area.width(), 0);
            painter.rotate(90);
        }
        painter.setRenderHint(QPainter::SmoothPixmapTransform, qAbs(ratio - 1.0) > 0.01);
        const int band = 1024; // in Streifen rendern, um Speicher zu sparen
        for (int y = 0; y < H; y += band) {
            const int h = qMin(band, H - y);
            const QImage img = renderPage(idx, dpi, 0, y, W, h);
            if (img.isNull()) continue;
            const double extra = (y + h < H) ? 1.0 : 0.0; // 1 px Überlappung gegen Haarlinien
            painter.drawImage(QRectF(ox, oy + y * ratio, W * ratio, h * ratio + extra), img);
        }
        painter.restore();
    }

    // --- Seiten rendern --------------------------------------------------
    QImage renderPage(int i, double dpi, int x, int y, int w, int h) const
    {
        auto page = m_doc->page(i);
        if (!page) return QImage();
        return page->renderToImage(dpi, dpi, x, y, w, h);
    }

    void drawPage(QPainter &p, int i, const QRect &dst)
    {
        p.fillRect(dst, Qt::white);
        p.setPen(QColor(0xB0, 0xB0, 0xB0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(dst.adjusted(-1, -1, 0, 0));

        const qreal dpr = devicePixelRatioF();
        const double dev = m_scale * dpr;
        const int fw = qMax(1, qRound(m_sizes[i].width() * dev));
        const int fh = qMax(1, qRound(m_sizes[i].height() * dev));
        const double dpi = dev * 72.0;

        if (qint64(fw) * fh <= 4000000) { // ganze Seite rendern und cachen
            const quint64 key = (quint64(i) << 32) | quint32(qRound(dev * 1000.0));
            if (QImage *cached = m_cache.object(key)) {
                p.drawImage(dst, *cached);
                return;
            }
            const QImage img = renderPage(i, dpi, -1, -1, -1, -1);
            if (img.isNull()) return;
            p.drawImage(dst, img);
            m_cache.insert(key, new QImage(img), int(img.sizeInBytes() / 1024));
        } else { // sehr große Darstellung: nur den sichtbaren Ausschnitt rendern
            const QRect vis = dst & viewport()->rect();
            if (vis.isEmpty()) return;
            const int x = qRound((vis.x() - dst.x()) * dpr);
            const int y = qRound((vis.y() - dst.y()) * dpr);
            const int w = qMax(1, qMin(fw - x, int(qCeil(vis.width() * dpr))));
            const int h = qMax(1, qMin(fh - y, int(qCeil(vis.height() * dpr))));
            QImage img = renderPage(i, dpi, x, y, w, h);
            if (img.isNull()) return;
            img.setDevicePixelRatio(dpr);
            p.drawImage(vis.topLeft(), img);
        }
    }

    void drawHits(QPainter &p, int i, const QRect &dst)
    {
        if (m_term.isEmpty()) return;
        const QList<QRectF> hits = hitsFor(i);
        for (int k = 0; k < hits.size(); ++k) {
            const QRectF &h = hits[k];
            const QRect r(dst.x() + qFloor(h.x() * m_scale), dst.y() + qFloor(h.y() * m_scale),
                          qCeil(h.width() * m_scale), qCeil(h.height() * m_scale));
            const bool cur = (i == m_hitPage && k == m_hitIdx);
            p.fillRect(r, cur ? QColor(255, 140, 0, 120) : QColor(255, 230, 0, 110));
        }
    }

    // --- Suche -----------------------------------------------------------
    QList<QRectF> hitsFor(int page)
    {
        auto it = m_hits.constFind(page);
        if (it != m_hits.constEnd()) return it.value();
        QList<QRectF> r;
        if (!m_term.isEmpty()) {
            auto p = m_doc->page(page);
            if (p) r = p->search(m_term, Poppler::Page::IgnoreCase);
        }
        m_hits.insert(page, r);
        return r;
    }

    void showHit()
    {
        if (m_hitPage < 0 || m_hitPage >= m_rects.size()) return;
        const QList<QRectF> hits = hitsFor(m_hitPage);
        if (m_hitIdx < 0 || m_hitIdx >= hits.size()) return;
        const QRect pr = m_rects[m_hitPage];
        const QRectF h = hits[m_hitIdx];
        const int x = pr.x() + int(h.center().x() * m_scale);
        const int y = pr.y() + int(h.center().y() * m_scale);
        m_pendingPage = -1;
        const QRect view(horizontalScrollBar()->value(), verticalScrollBar()->value(),
                         viewport()->width(), viewport()->height());
        if (!view.adjusted(40, 40, -40, -40).contains(QPoint(x, y))) {
            verticalScrollBar()->setValue(y - viewport()->height() / 3);
            horizontalScrollBar()->setValue(x - viewport()->width() / 2);
        }
    }

    // --- Links -----------------------------------------------------------
    const std::vector<std::unique_ptr<Poppler::Link>> &linksFor(int i)
    {
        auto it = m_links.find(i);
        if (it != m_links.end()) return it->second;
        std::vector<std::unique_ptr<Poppler::Link>> v;
        if (auto page = m_doc->page(i)) v = page->links();
        return m_links.emplace(i, std::move(v)).first->second;
    }

    Poppler::Link *linkAt(const QPoint &c)
    {
        if (!m_doc) return nullptr;
        for (int i = 0; i < m_rects.size() && i < m_pageCount; ++i) {
            const QRect &r = m_rects[i];
            if (!r.contains(c)) continue;
            const QPointF n(double(c.x() - r.x()) / r.width(), double(c.y() - r.y()) / r.height());
            for (const auto &l : linksFor(i))
                if (l->linkArea().normalized().contains(n)) return l.get();
            return nullptr;
        }
        return nullptr;
    }

    void activateLink(Poppler::Link *l)
    {
        switch (l->linkType()) {
        case Poppler::Link::Goto: {
            auto *g = static_cast<Poppler::LinkGoto *>(l);
            if (!g->isExternal()) {
                const int pg = g->destination().pageNumber();
                if (pg > 0) goToPage(pg - 1);
            }
            break;
        }
        case Poppler::Link::Browse: {
            const QUrl url(static_cast<Poppler::LinkBrowse *>(l)->url());
            const QString s = url.scheme().toLower();
            if (s == QLatin1String("http") || s == QLatin1String("https") || s == QLatin1String("mailto"))
                QDesktopServices::openUrl(url);
            break;
        }
        default:
            break;
        }
    }

    // --- Text kopieren ---------------------------------------------------
    void copySelection()
    {
        if (m_sel.isNull() || !m_doc) return;
        QStringList parts;
        for (int i = 0; i < m_rects.size() && i < m_pageCount; ++i) {
            const QRect r = m_sel & m_rects[i];
            if (r.isEmpty()) continue;
            const QRectF pts((r.x() - m_rects[i].x()) / m_scale, (r.y() - m_rects[i].y()) / m_scale,
                             r.width() / m_scale, r.height() / m_scale);
            if (auto page = m_doc->page(i)) {
                const QString t = page->text(pts).trimmed();
                if (!t.isEmpty()) parts << t;
            }
        }
        const QString text = parts.join(QLatin1Char('\n'));
        if (!text.isEmpty()) {
            QGuiApplication::clipboard()->setText(text);
            emit textCopied(text);
        }
    }

    // --- Daten -----------------------------------------------------------
    std::unique_ptr<Poppler::Document> m_doc;
    QString m_path, m_password;
    int m_pageCount = 0;
    QVector<QSizeF> m_sizes;  // Seitengrößen in Punkt
    QVector<QRect> m_rects;   // Seitenpositionen in Inhaltskoordinaten
    int m_contentW = 0, m_contentH = 0;
    QSize m_vpSize;

    ZoomMode m_mode = ZoomMode::FitWidth;
    double m_zoom = 1.0;      // 1.0 = 100 %
    double m_scale = 1.0;     // Pixel pro Punkt
    bool m_facing = false;
    int m_current = 0;
    int m_wheelAcc = 0;       // aufsummierte Mausrad-Rasten (Modus "Ganze Seite")
    int m_pendingPage = -1;   // Seite, die nach dem ersten echten Layout angezeigt werden soll

    QCache<quint64, QImage> m_cache;
    std::map<int, std::vector<std::unique_ptr<Poppler::Link>>> m_links;

    QString m_term;
    QHash<int, QList<QRectF>> m_hits;
    int m_hitPage = -1, m_hitIdx = -1;

    QPoint m_pressPos, m_lastPos, m_selStart;
    bool m_dragging = false, m_selecting = false;
    QRect m_sel;

    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer;
    int m_reloadTries = 0;
};

// Selbstgezeichnete Symbole: Seitenbreite (Doppelpfeil) / Ganze Seite (Eckmarken)
static QIcon fitIcon(bool width, const QPalette &pal)
{
    QIcon icon;
    for (int sz : {16, 24, 32, 48, 64}) {
        QPixmap pm(sz, sz);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(pal.color(QPalette::ButtonText), qMax(1.0, sz / 16.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const double m = sz * 0.10, a = sz * 0.14;
        if (width) {
            p.drawRect(QRectF(sz * 0.30, sz * 0.12, sz * 0.40, sz * 0.76));
            const double y = sz / 2.0;
            p.drawLine(QPointF(m, y), QPointF(sz - m, y));
            p.drawLine(QPointF(m, y), QPointF(m + a, y - a));
            p.drawLine(QPointF(m, y), QPointF(m + a, y + a));
            p.drawLine(QPointF(sz - m, y), QPointF(sz - m - a, y - a));
            p.drawLine(QPointF(sz - m, y), QPointF(sz - m - a, y + a));
        } else {
            p.drawRect(QRectF(sz * 0.30, sz * 0.24, sz * 0.40, sz * 0.52));
            const double l = sz * 0.22, e = sz - m;
            p.drawPolyline(QPolygonF({QPointF(m, m + l), QPointF(m, m), QPointF(m + l, m)}));
            p.drawPolyline(QPolygonF({QPointF(e - l, m), QPointF(e, m), QPointF(e, m + l)}));
            p.drawPolyline(QPolygonF({QPointF(m, e - l), QPointF(m, e), QPointF(m + l, e)}));
            p.drawPolyline(QPolygonF({QPointF(e - l, e), QPointF(e, e), QPointF(e, e - l)}));
        }
        icon.addPixmap(pm);
    }
    return icon;
}

static QIcon facingIcon(const QPalette &pal)
{
    QIcon icon;
    for (int sz : {16, 24, 32, 48, 64}) {
        QPixmap pm(sz, sz);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(pal.color(QPalette::ButtonText), qMax(1.0, sz / 16.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const double top = sz * 0.14, h = sz * 0.72, w = sz * 0.31, gap = sz * 0.05, mid = sz / 2.0;
        p.drawRect(QRectF(mid - gap - w, top, w, h)); // linke Seite
        p.drawRect(QRectF(mid + gap, top, w, h));     // rechte Seite
        icon.addPixmap(pm);
    }
    return icon;
}

static QIcon printIcon(const QPalette &pal)
{
    QIcon icon;
    for (int sz : {16, 24, 32, 48, 64}) {
        QPixmap pm(sz, sz);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(pal.color(QPalette::ButtonText), qMax(1.0, sz / 16.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawRect(QRectF(sz * 0.28, sz * 0.10, sz * 0.44, sz * 0.26));                  // Papier oben
        p.drawRoundedRect(QRectF(sz * 0.10, sz * 0.34, sz * 0.80, sz * 0.34), sz * 0.06, sz * 0.06); // Gerät
        p.setBrush(pal.color(QPalette::Button));
        p.drawRect(QRectF(sz * 0.26, sz * 0.54, sz * 0.48, sz * 0.36));                  // Papier unten
        icon.addPixmap(pm);
    }
    return icon;
}

static QIcon chevronIcon(bool left, const QPalette &pal)
{
    QIcon icon;
    for (int sz : {16, 24, 32, 48, 64}) {
        QPixmap pm(sz, sz);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(pal.color(QPalette::ButtonText), qMax(1.5, sz / 10.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const double x1 = left ? sz * 0.62 : sz * 0.38, x2 = left ? sz * 0.38 : sz * 0.62;
        p.drawPolyline(QPolygonF({QPointF(x1, sz * 0.24), QPointF(x2, sz * 0.5), QPointF(x1, sz * 0.76)}));
        icon.addPixmap(pm);
    }
    return icon;
}

// ---------------------------------------------------------------------------
// MainWindow
// ---------------------------------------------------------------------------
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow()
    {
        setWindowTitle(kAppName);
        resize(1100, 800);
        setAcceptDrops(true);
        buildUi();
        QSettings s;
        restoreGeometry(s.value("window/geometry").toByteArray());
        refreshRecent();
        onTabChanged();
    }

    void openFile(const QString &path)
    {
        const QString abs = QFileInfo(path).absoluteFilePath();
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (view(i)->path() == abs) {
                m_tabs->setCurrentIndex(i);
                return;
            }
        }
        auto *v = new DocumentView;
        QString err;
        if (!v->load(abs, &err)) {
            delete v;
            if (!err.isEmpty()) QMessageBox::warning(this, kAppName, err);
            return;
        }
        QSettings s;
        const QString key = stateKey(abs);
        v->applyState(ZoomMode(s.value(key + "/mode", int(ZoomMode::FitWidth)).toInt()),
                      s.value(key + "/zoom", 1.0).toDouble(), s.value(key + "/facing", false).toBool(),
                      s.value(key + "/page", 0).toInt());

        connect(v, &DocumentView::pageChanged, this, [this, v](int cur, int total) {
            if (v == currentView()) updatePageUi(cur, total);
        });
        connect(v, &DocumentView::zoomChanged, this, [this, v](double z, int mode) {
            if (v == currentView()) updateZoomUi(z, mode);
        });
        connect(v, &DocumentView::textCopied, this, [](const QString &) {
            QToolTip::showText(QCursor::pos(), QStringLiteral("Text in die Zwischenablage kopiert"));
        });

        const int idx = m_tabs->addTab(v, QFileInfo(abs).fileName());
        m_tabs->setTabToolTip(idx, abs);
        m_stack->setCurrentWidget(m_tabs);
        m_tabs->setCurrentIndex(idx);
        addRecent(abs);
        v->setFocus();
    }

protected:
    void closeEvent(QCloseEvent *e) override
    {
        for (int i = 0; i < m_tabs->count(); ++i) saveState(view(i));
        QSettings s;
        if (!isFullScreen()) s.setValue("window/geometry", saveGeometry());
        e->accept();
    }
    void dragEnterEvent(QDragEnterEvent *e) override
    {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    }
    void dropEvent(QDropEvent *e) override
    {
        for (const QUrl &u : e->mimeData()->urls())
            if (u.isLocalFile()) openFile(u.toLocalFile());
    }

private:
    // --- Hilfen ----------------------------------------------------------
    DocumentView *view(int i) const { return qobject_cast<DocumentView *>(m_tabs->widget(i)); }
    DocumentView *currentView() const { return qobject_cast<DocumentView *>(m_tabs->currentWidget()); }

    static QString stateKey(const QString &path)
    {
        return QStringLiteral("files/") +
               QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex());
    }

    void saveState(DocumentView *v)
    {
        if (!v) return;
        QSettings s;
        const QString key = stateKey(v->path());
        s.setValue(key + "/page", v->currentPage());
        s.setValue(key + "/mode", int(v->mode()));
        s.setValue(key + "/zoom", v->zoom());
        s.setValue(key + "/facing", v->facing());
    }

    void addRecent(const QString &path)
    {
        QSettings s;
        QStringList r = s.value("recent").toStringList();
        r.removeAll(path);
        r.prepend(path);
        while (r.size() > 15) r.removeLast();
        s.setValue("recent", r);
        refreshRecent();
    }

    void refreshRecent()
    {
        m_recent->clear();
        for (const QString &p : QSettings().value("recent").toStringList()) {
            if (!QFile::exists(p)) continue;
            auto *it = new QListWidgetItem(QFileInfo(p).fileName(), m_recent);
            it->setToolTip(p);
            it->setData(Qt::UserRole, p);
        }
    }

    QAction *makeAction(const QString &text, const QStringList &keys, const std::function<void()> &fn)
    {
        auto *a = new QAction(text, this);
        QList<QKeySequence> seqs;
        for (const QString &k : keys) seqs << QKeySequence(k);
        if (!seqs.isEmpty()) a->setShortcuts(seqs);
        connect(a, &QAction::triggered, this, [fn] { fn(); });
        addAction(a); // damit Tastenkürzel auch bei verstecktem Menü funktionieren
        return a;
    }

    // --- Oberfläche ------------------------------------------------------
    void buildUi()
    {
        // Startseite
        auto *welcome = new QWidget;
        auto *wl = new QVBoxLayout(welcome);
        wl->setContentsMargins(40, 30, 40, 30);
        auto *title = new QLabel(kAppName);
        QFont tf = title->font();
        tf.setPointSize(tf.pointSize() + 14);
        tf.setBold(true);
        title->setFont(tf);
        wl->addWidget(title);
        wl->addWidget(new QLabel(QStringLiteral("PDF hierher ziehen oder Strg+O drücken.   Menü: F9   Inhaltsverzeichnis: F12")));
        wl->addSpacing(16);
        wl->addWidget(new QLabel(QStringLiteral("Zuletzt geöffnet")));
        m_recent = new QListWidget;
        m_recent->setFrameShape(QFrame::NoFrame);
        wl->addWidget(m_recent, 1);
        connect(m_recent, &QListWidget::itemActivated, this,
                [this](QListWidgetItem *it) { openFile(it->data(Qt::UserRole).toString()); });

        // Tabs
        m_tabs = new QTabWidget;
        m_tabs->setDocumentMode(true);
        m_tabs->setTabsClosable(true);
        m_tabs->setMovable(true);
        m_tabs->setElideMode(Qt::ElideRight);
        m_tabs->tabBar()->setExpanding(false);
        connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int i) { closeTab(i); });
        connect(m_tabs, &QTabWidget::currentChanged, this, [this] { onTabChanged(); });

        m_stack = new QStackedWidget;
        m_stack->addWidget(welcome);
        m_stack->addWidget(m_tabs);

        // Suchfeld (liegt dauerhaft rechts in der Werkzeugleiste)
        m_findEdit = new QLineEdit;
        m_findEdit->setClearButtonEnabled(true);
        m_findEdit->setPlaceholderText(QStringLiteral("Suchen…"));
        m_findEdit->setFixedWidth(180);
        connect(m_findEdit, &QLineEdit::textChanged, this, [this](const QString &t) {
            if (auto *v = currentView()) {
                v->setSearchTerm(t);
                setFindState(t.isEmpty() || v->findNext(true));
            }
        });
        connect(m_findEdit, &QLineEdit::returnPressed, this,
                [this] { findStep(!(QApplication::keyboardModifiers() & Qt::ShiftModifier)); });

        setCentralWidget(m_stack);

        // Inhaltsverzeichnis
        m_tocTree = new QTreeWidget;
        m_tocTree->setHeaderHidden(true);
        m_tocDock = new QDockWidget(QStringLiteral("Inhaltsverzeichnis"), this);
        m_tocDock->setObjectName("toc");
        m_tocDock->setWidget(m_tocTree);
        addDockWidget(Qt::LeftDockWidgetArea, m_tocDock);
        m_tocDock->hide();
        auto gotoToc = [this](QTreeWidgetItem *it) {
            if (!it) return;
            const int pg = it->data(0, Qt::UserRole).toInt();
            if (auto *v = currentView()) if (pg >= 0) v->goToPage(pg);
        };
        connect(m_tocTree, &QTreeWidget::itemClicked, this, [gotoToc](QTreeWidgetItem *it) { gotoToc(it); });
        connect(m_tocTree, &QTreeWidget::itemActivated, this, [gotoToc](QTreeWidgetItem *it) { gotoToc(it); });

        // Aktionen
        auto cur = [this] { return currentView(); };
        QAction *aOpen = makeAction(QStringLiteral("Öffnen…"), {"Ctrl+O"}, [this] { openDialog(); });
        aOpen->setIcon(QIcon::fromTheme("document-open"));
        aOpen->setToolTip(QStringLiteral("Öffnen (Strg+O)"));
        QAction *aPrint = makeAction(QStringLiteral("Drucken…"), {"Ctrl+P"}, [this] {
            if (auto *v = currentView()) v->printDocument();
        });
        aPrint->setIcon(QIcon::fromTheme("document-print", printIcon(palette())));
        aPrint->setToolTip(QStringLiteral("Drucken (Strg+P)"));
        QAction *aClose = makeAction(QStringLiteral("Tab schließen"), {"Ctrl+W", "Ctrl+F4"},
                                     [this] { if (m_tabs->count()) closeTab(m_tabs->currentIndex()); });
        QAction *aQuit = makeAction(QStringLiteral("Beenden"), {"Ctrl+Q"}, [this] { close(); });
        makeAction(QStringLiteral("Nächster Tab"), {"Ctrl+Tab", "Ctrl+PgDown"}, [this] {
            if (m_tabs->count()) m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
        });
        makeAction(QStringLiteral("Vorheriger Tab"), {"Ctrl+Shift+Tab", "Ctrl+PgUp"}, [this] {
            if (m_tabs->count()) m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count());
        });

        QAction *aPrev = makeAction(QStringLiteral("Vorherige Seite"), {}, [cur] { if (auto *v = cur()) v->prevPage(); });
        aPrev->setIcon(QIcon::fromTheme("go-previous"));
        aPrev->setIconText(QStringLiteral("◀"));
        aPrev->setToolTip(QStringLiteral("Vorherige Seite"));
        QAction *aNext = makeAction(QStringLiteral("Nächste Seite"), {}, [cur] { if (auto *v = cur()) v->nextPage(); });
        aNext->setIcon(QIcon::fromTheme("go-next"));
        aNext->setIconText(QStringLiteral("▶"));
        aNext->setToolTip(QStringLiteral("Nächste Seite"));
        QAction *aGoto = makeAction(QStringLiteral("Gehe zu Seite…"), {"Ctrl+G"}, [this] {
            m_pageEdit->setFocus();
            m_pageEdit->selectAll();
        });

        QAction *aZoomIn = makeAction(QStringLiteral("Vergrößern"), {"Ctrl++", "Ctrl+=", "Ctrl+Plus"},
                                      [cur] { if (auto *v = cur()) v->zoomBy(1.25); });
        aZoomIn->setIcon(QIcon::fromTheme("zoom-in"));
        aZoomIn->setIconText(QStringLiteral("+"));
        aZoomIn->setToolTip(QStringLiteral("Vergrößern (Strg++)"));
        QAction *aZoomOut = makeAction(QStringLiteral("Verkleinern"), {"Ctrl+-", "Ctrl+Minus"},
                                       [cur] { if (auto *v = cur()) v->zoomBy(0.8); });
        aZoomOut->setIcon(QIcon::fromTheme("zoom-out"));
        aZoomOut->setIconText(QStringLiteral("−"));
        aZoomOut->setToolTip(QStringLiteral("Verkleinern (Strg+-)"));
        QAction *aFitPage = makeAction(kFitPage, {"Ctrl+0", "Ctrl+2"},
                                       [cur] { if (auto *v = cur()) v->setZoomMode(ZoomMode::FitPage); });
        aFitPage->setIcon(fitIcon(false, palette()));
        aFitPage->setToolTip(QStringLiteral("Ganze Seite anpassen (Strg+0)"));
        aFitPage->setCheckable(true);
        m_aFitPage = aFitPage;
        QAction *aFitWidth = makeAction(kFitWidth, {"Ctrl+3"},
                                        [cur] { if (auto *v = cur()) v->setZoomMode(ZoomMode::FitWidth); });
        aFitWidth->setIcon(fitIcon(true, palette()));
        aFitWidth->setToolTip(QStringLiteral("Auf Seitenbreite anpassen (Strg+3)"));
        aFitWidth->setCheckable(true);
        m_aFitWidth = aFitWidth;
        QAction *aActual = makeAction(QStringLiteral("Originalgröße (100 %)"), {"Ctrl+1"},
                                      [cur] { if (auto *v = cur()) v->setZoom(1.0); });
        m_aFacing = makeAction(QStringLiteral("Doppelseitenansicht"), {"Ctrl+7"}, [this] {
            if (auto *v = currentView()) v->setFacing(m_aFacing->isChecked());
        });
        m_aFacing->setCheckable(true);

        QAction *aFind = makeAction(QStringLiteral("Suchen…"), {"Ctrl+F", "/"}, [this] { focusSearch(); });
        QAction *aFindNext = makeAction(QStringLiteral("Weitersuchen"), {"F3"}, [this] { findStep(true); });
        QAction *aFindPrev = makeAction(QStringLiteral("Rückwärts suchen"), {"Shift+F3"}, [this] { findStep(false); });
        aFindPrev->setIcon(chevronIcon(true, palette()));
        aFindPrev->setToolTip(QStringLiteral("Vorheriger Treffer (Shift+F3)"));
        aFindNext->setIcon(chevronIcon(false, palette()));
        aFindNext->setToolTip(QStringLiteral("Nächster Treffer (F3)"));
        QAction *aReload = makeAction(QStringLiteral("Neu laden"), {"F5", "Ctrl+R"},
                                      [cur] { if (auto *v = cur()) v->reload(); });
        QAction *aFull = makeAction(QStringLiteral("Vollbild"), {"F11"}, [this] { toggleFullscreen(); });
        QAction *aMenu = makeAction(QStringLiteral("Menüleiste ein/aus"), {"F9"},
                                    [this] { menuBar()->setVisible(!menuBar()->isVisible()); });
        QAction *aTool = makeAction(QStringLiteral("Werkzeugleiste ein/aus"), {"F8"},
                                    [this] { m_toolbar->setVisible(!m_toolbar->isVisible()); });
        QAction *aToc = m_tocDock->toggleViewAction();
        aToc->setText(QStringLiteral("Inhaltsverzeichnis"));
        aToc->setShortcut(QKeySequence(Qt::Key_F12));
        addAction(aToc);

        auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), this, [this] {
            if (m_findEdit->hasFocus() || !m_findEdit->text().isEmpty()) {
                m_findEdit->clear(); // entfernt auch die Hervorhebungen
                if (auto *v = currentView()) v->setFocus();
            } else if (isFullScreen()) {
                toggleFullscreen();
            }
        });
        esc->setContext(Qt::WindowShortcut);

        // Menüs (standardmäßig versteckt, F9)
        QMenu *mFile = menuBar()->addMenu(QStringLiteral("&Datei"));
        mFile->addActions({aOpen, aPrint, aReload, aClose});
        mFile->addSeparator();
        mFile->addAction(aQuit);
        QMenu *mView = menuBar()->addMenu(QStringLiteral("&Ansicht"));
        mView->addActions({aZoomIn, aZoomOut, aFitPage, aFitWidth, aActual});
        mView->addSeparator();
        mView->addActions({m_aFacing, aToc, aTool, aFull});
        QMenu *mGo = menuBar()->addMenu(QStringLiteral("&Gehe zu"));
        mGo->addActions({aGoto, aPrev, aNext});
        mGo->addSeparator();
        mGo->addActions({aFind, aFindNext, aFindPrev});
        menuBar()->hide();
        Q_UNUSED(aMenu);

        // Werkzeugleiste
        m_toolbar = addToolBar(QStringLiteral("Werkzeuge"));
        m_toolbar->setMovable(false);
        m_toolbar->setContextMenuPolicy(Qt::PreventContextMenu);
        m_toolbar->addAction(aOpen);
        m_toolbar->addAction(aPrint);
        m_toolbar->addSeparator();
        m_toolbar->addAction(aPrev);

        m_pageEdit = new QLineEdit;
        m_pageEdit->setFixedWidth(52);
        m_pageEdit->setAlignment(Qt::AlignCenter);
        m_pageEdit->setValidator(new QIntValidator(1, 999999, m_pageEdit));
        m_pageCountLabel = new QLabel(QStringLiteral(" / 0 "));
        connect(m_pageEdit, &QLineEdit::returnPressed, this, [this] {
            if (auto *v = currentView()) {
                v->goToPage(m_pageEdit->text().toInt() - 1);
                v->setFocus();
            }
        });
        m_toolbar->addWidget(m_pageEdit);
        m_toolbar->addWidget(m_pageCountLabel);
        m_toolbar->addAction(aNext);
        m_toolbar->addSeparator();
        m_toolbar->addAction(aZoomOut);

        m_zoomCombo = new QComboBox;
        m_zoomCombo->setEditable(true);
        m_zoomCombo->setInsertPolicy(QComboBox::NoInsert);
        m_zoomCombo->setFixedWidth(145);
        m_zoomCombo->addItems({kFitPage, kFitWidth, "50%", "75%", "100%", "125%", "150%", "200%", "300%", "400%"});
        connect(m_zoomCombo, &QComboBox::textActivated, this, [this](const QString &t) {
            auto *v = currentView();
            if (!v) return;
            if (t == kFitPage) v->setZoomMode(ZoomMode::FitPage);
            else if (t == kFitWidth) v->setZoomMode(ZoomMode::FitWidth);
            else {
                bool ok = false;
                const double pct = QString(t).remove('%').trimmed().toDouble(&ok);
                if (ok && pct > 0) v->setZoom(pct / 100.0);
            }
            v->setFocus();
        });
        m_toolbar->addWidget(m_zoomCombo);
        m_toolbar->addAction(aZoomIn);
        m_toolbar->addSeparator();
        m_toolbar->addAction(aFitWidth);
        m_toolbar->addAction(aFitPage);
        m_toolbar->addSeparator();
        m_aFacing->setIcon(facingIcon(palette()));
        m_aFacing->setToolTip(QStringLiteral("Doppelseitenansicht (Strg+7)"));
        m_toolbar->addAction(m_aFacing);

        auto *spacer = new QWidget;
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_toolbar->addWidget(spacer);
        m_toolbar->addWidget(m_findEdit);
        m_toolbar->addAction(aFindPrev);
        m_toolbar->addAction(aFindNext);
    }

    void openDialog()
    {
        QSettings s;
        const QStringList files = QFileDialog::getOpenFileNames(
            this, QStringLiteral("PDF öffnen"), s.value("lastDir", QDir::homePath()).toString(),
            QStringLiteral("PDF-Dokumente (*.pdf);;Alle Dateien (*)"));
        if (files.isEmpty()) return;
        s.setValue("lastDir", QFileInfo(files.first()).absolutePath());
        for (const QString &f : files) openFile(f);
    }

    void closeTab(int i)
    {
        DocumentView *v = view(i);
        if (!v) return;
        saveState(v);
        m_tabs->removeTab(i);
        v->deleteLater();
        if (m_tabs->count() == 0) {
            m_stack->setCurrentIndex(0);
            onTabChanged();
        }
    }

    void onTabChanged()
    {
        DocumentView *v = currentView();
        if (!v) {
            setWindowTitle(kAppName);
            updatePageUi(0, 0);
            m_tocTree->clear();
            return;
        }
        setWindowTitle(QFileInfo(v->path()).fileName() + QStringLiteral(" – ") + kAppName);
        v->emitState();
        {
            const QSignalBlocker b(m_aFacing);
            m_aFacing->setChecked(v->facing());
        }
        rebuildToc(v);
        if (!m_findEdit->text().isEmpty()) v->setSearchTerm(m_findEdit->text());
        v->setFocus();
    }

    void updatePageUi(int cur, int total)
    {
        if (!m_pageEdit->hasFocus()) m_pageEdit->setText(total > 0 ? QString::number(cur + 1) : QString());
        m_pageCountLabel->setText(QStringLiteral(" / %1 ").arg(total));
        if (auto *v = currentView()) setWindowTitle(QFileInfo(v->path()).fileName() + QStringLiteral(" – ") + kAppName);
    }

    void updateZoomUi(double zoom, int mode)
    {
        const QSignalBlocker b(m_zoomCombo);
        m_aFitPage->setChecked(mode == int(ZoomMode::FitPage));
        m_aFitWidth->setChecked(mode == int(ZoomMode::FitWidth));
        if (mode == int(ZoomMode::FitPage)) m_zoomCombo->setCurrentText(kFitPage);
        else if (mode == int(ZoomMode::FitWidth)) m_zoomCombo->setCurrentText(kFitWidth);
        else m_zoomCombo->setCurrentText(QString::number(qRound(zoom * 100)) + QLatin1Char('%'));
    }

    // --- Inhaltsverzeichnis ---------------------------------------------
    void rebuildToc(DocumentView *v)
    {
        m_tocTree->clear();
        const auto items = v->outline();
        if (items.isEmpty()) {
            auto *it = new QTreeWidgetItem(m_tocTree, QStringList(QStringLiteral("(kein Inhaltsverzeichnis)")));
            it->setFlags(Qt::NoItemFlags);
            return;
        }
        fillToc(v, items, nullptr);
    }

    void fillToc(DocumentView *v, const QVector<Poppler::OutlineItem> &items, QTreeWidgetItem *parentItem)
    {
        for (const Poppler::OutlineItem &o : items) {
            auto *item = parentItem ? new QTreeWidgetItem(parentItem) : new QTreeWidgetItem(m_tocTree);
            item->setText(0, o.name());
            item->setData(0, Qt::UserRole, v->outlineTargetPage(o));
            if (o.hasChildren()) fillToc(v, o.children(), item);
            if (o.isOpen()) item->setExpanded(true);
        }
    }

    // --- Suche -----------------------------------------------------------
    void focusSearch()
    {
        if (!m_toolbar->isVisible()) m_toolbar->show(); // z. B. im Vollbild
        m_findEdit->setFocus();
        m_findEdit->selectAll();
    }
    void findStep(bool forward)
    {
        auto *v = currentView();
        if (!v) return;
        if (m_findEdit->text().isEmpty()) { focusSearch(); return; }
        setFindState(v->findNext(forward));
    }
    void setFindState(bool ok)
    {
        m_findEdit->setStyleSheet(ok ? QString() : QStringLiteral("QLineEdit { background: #ffd6d6; color: black; }"));
    }

    void toggleFullscreen()
    {
        if (isFullScreen()) {
            showNormal();
            m_toolbar->show();
        } else {
            m_toolbar->hide();
            menuBar()->hide();
            showFullScreen();
        }
    }

    // --- Widgets ---------------------------------------------------------
    QStackedWidget *m_stack = nullptr;
    QTabWidget *m_tabs = nullptr;
    QListWidget *m_recent = nullptr;
    QToolBar *m_toolbar = nullptr;
    QLineEdit *m_pageEdit = nullptr;
    QLabel *m_pageCountLabel = nullptr;
    QComboBox *m_zoomCombo = nullptr;
    QAction *m_aFacing = nullptr;
    QAction *m_aFitPage = nullptr;
    QAction *m_aFitWidth = nullptr;
    QLineEdit *m_findEdit = nullptr;
    QDockWidget *m_tocDock = nullptr;
    QTreeWidget *m_tocTree = nullptr;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("minipdf"));
    QApplication::setOrganizationName(QStringLiteral("minipdf"));
    QApplication::setApplicationDisplayName(kAppName);

    MainWindow w;
    w.show();
    const QStringList args = app.arguments().mid(1);
    for (const QString &a : args) w.openFile(a);
    return app.exec();
}

#include "main.moc"
