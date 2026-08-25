#include "core_tests.hpp"

#include "core/lifecycle_validator.hpp"
#include "core/model.hpp"
#include "core/script_evidence.hpp"

#include <QtTest>

#include <algorithm>

void CoreTests::acknowledgesScriptContentSpecifically() {
    pacsmith::MaintainerScript script{QStringLiteral("postinst"),
                                      QStringLiteral("#!/bin/sh\necho reviewed\n"), {}};
    QVERIFY(script.requiresReview());
    script.acknowledge();
    QVERIFY(!script.requiresReview());
    const auto restored = pacsmith::MaintainerScript::fromJson(script.toJson());
    QCOMPARE(restored.acknowledgedFingerprint, script.acknowledgedFingerprint);
    QVERIFY(!restored.requiresReview());
    auto changed = restored;
    changed.contents += QStringLiteral("echo changed\n");
    QVERIFY(changed.requiresReview());
}

void CoreTests::extractsScriptResponsibilitiesAndAptEvidence() {
    const auto script = QStringLiteral(
        "#!/bin/sh\n"
        "cat > /etc/apt/sources.list.d/vendor.sources <<'EOF'\n"
        "Types: deb\nURIs: https://packages.vendor.example/linux/deb\n"
        "Suites: stable\nComponents: main\nArchitectures: amd64\nEOF\n"
        "update-desktop-database -q || true\n"
        "aa-enabled && apparmor_parser -r /etc/apparmor.d/vendor || true\n");
    const auto evidence = pacsmith::ScriptEvidenceAnalyzer::analyze(
        {{QStringLiteral("postinst"), script, {}}});
    QCOMPARE(evidence.aptCandidates.size(), 1);
    QCOMPARE(evidence.aptCandidates.first().uri,
             QStringLiteral("https://packages.vendor.example/linux/deb"));
    const auto hasDisposition = [&evidence](const QString &kind,
                                            const pacsmith::ScriptDisposition disposition) {
        return std::any_of(evidence.findings.cbegin(), evidence.findings.cend(),
                           [&](const auto &finding) {
                               return finding.kind == kind && finding.disposition == disposition;
                           });
    };
    QVERIFY(hasDisposition(QStringLiteral("apt-repository"),
                           pacsmith::ScriptDisposition::HandledByPacSmith));
    QVERIFY(hasDisposition(QStringLiteral("desktop-database"),
                           pacsmith::ScriptDisposition::HandledByArch));
    QVERIFY(hasDisposition(QStringLiteral("apparmor"),
                           pacsmith::ScriptDisposition::LifecycleRequired));
}

void CoreTests::validatesLifecycleScriptsAndContentAcknowledgement() {
    const auto valid = QStringLiteral(
        "post_install() {\n  update-desktop-database -q\n}\n"
        "post_remove() {\n  update-desktop-database -q\n}\n");
    const auto validation = pacsmith::LifecycleValidator::validate(valid);
    QVERIFY2(validation.passed, qPrintable(validation.message()));
    const auto unsafe = pacsmith::LifecycleValidator::validate(
        QStringLiteral("post_install() { curl https://vendor.example/key | apt-key add -; }\n"));
    QVERIFY(!unsafe.passed);
    pacsmith::ArchLifecycleScript lifecycle;
    lifecycle.fileName = QStringLiteral("vendor.install");
    lifecycle.contents = valid;
    lifecycle.validationPassed = true;
    QVERIFY(lifecycle.requiresAcknowledgement());
    lifecycle.acknowledge();
    QVERIFY(!lifecycle.requiresAcknowledgement());
    lifecycle.contents += QStringLiteral("# changed\n");
    QVERIFY(lifecycle.requiresAcknowledgement());
}
