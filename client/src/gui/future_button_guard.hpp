#pragma once

#include <QAbstractButton>
#include <QFuture>
#include <QFutureWatcher>
#include <QObject>
#include <QPointer>

#include <utility>

namespace pacsmith::gui {

template <typename Result, typename Finished>
void watchFutureWithDisabledButton(QAbstractButton *button, QObject *context,
                                   QFuture<Result> future, Finished &&finished) {
    const QPointer<QAbstractButton> buttonGuard(button);
    if (!buttonGuard.isNull()) buttonGuard->setEnabled(false);

    auto *watcher = new QFutureWatcher<Result>(context);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, context,
                     [watcher, buttonGuard,
                      finished = std::forward<Finished>(finished)]() mutable {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (!buttonGuard.isNull()) buttonGuard->setEnabled(true);
        finished(result);
    });
    watcher->setFuture(std::move(future));
}

} // namespace pacsmith::gui
