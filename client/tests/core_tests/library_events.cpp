#include "core_tests.hpp"

#include "core/library_events.hpp"

#include <QtTest>

using namespace pacsmith;

void CoreTests::parsesChunkedServerEvents() {
    SseParser parser;
    QCOMPARE(parser.feed("id: 4\nevent: cha").size(), 0);
    const auto events = parser.feed(
        "nge\ndata: {\"sequence\":4,\"topics\":[\"projects\",\"jobs\"],"
        "\"project_id\":\"p1\",\"project_name\":\"Parsec\",\"package_name\":\"parsec-bin\","
        "\"job_id\":\"opaque-id\",\"job_kind\":\"build\",\"job_status\":\"running\"}\n\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().sequence, 4);
    QCOMPARE(events.first().name, QStringLiteral("change"));
    QCOMPARE(events.first().topics, QStringList({QStringLiteral("projects"), QStringLiteral("jobs")}));
    QCOMPARE(events.first().projectId, QStringLiteral("p1"));
    QCOMPARE(events.first().projectName, QStringLiteral("Parsec"));
    QCOMPARE(events.first().packageName, QStringLiteral("parsec-bin"));
    QCOMPARE(events.first().jobKind, QStringLiteral("build"));
    QCOMPARE(jobStatusMessage(events.first()), QStringLiteral("Building package Parsec…"));
    QVERIFY(!jobStatusMessage(events.first()).contains(QStringLiteral("opaque-id")));
}

void CoreTests::describesUnnamedServerJobsWithoutOpaqueIds() {
    ServerEvent event;
    event.jobId = QStringLiteral("34f142bf-a280-4f88-aee7-08ef765072a9");
    event.jobKind = QStringLiteral("build");
    event.jobStatus = QStringLiteral("running");
    QCOMPARE(jobStatusMessage(event), QStringLiteral("Building package…"));
    QVERIFY(!jobStatusMessage(event).contains(event.jobId));
}

void CoreTests::ignoresMalformedServerEventsAndHeartbeats() {
    SseParser parser;
    QCOMPARE(parser.feed(": keepalive\n\n").size(), 0);
    QCOMPARE(parser.feed("event: change\ndata: not-json\n\n").size(), 0);
    const auto events = parser.feed("id: 9\ndata: {\"topics\":[\"all\"]}\n\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().sequence, 9);
    QCOMPARE(events.first().topics, QStringList({QStringLiteral("all")}));
}
