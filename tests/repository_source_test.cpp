// srcdeps: RepositorySource.cpp
#include <QtTest/QtTest>

#include <QVariantMap>

#include "RepositorySource.h"

// What a consent prompt is allowed to say about where a package comes from.
//
// The catalog URL used to stand here. For everything published through GitHub
// that is some path under the single hostname raw.githubusercontent.com —
// identical for the official catalog and for a repository the user added last
// week for something unrelated — so it answered the question in form and not in
// substance.
//
// The inputs are a listRepositories() row after PackageCoordinator's
// withDisplayLabels(): `displayLabel` plus the sourceOwner / sourceRepo /
// sourceHost that logos-package-downloader already parsed out of the URL.
// Nothing here re-parses a URL, and these tests exist to keep it that way.
class RepositorySourceTest : public QObject {
    Q_OBJECT

    static QVariantMap officialRepo()
    {
        return QVariantMap{
            {"url", "https://raw.githubusercontent.com/logos-co/"
                    "logos-modules-release/refs/heads/main/logos-repo.json"},
            {"displayLabel", "Logos Official Modules"},
            {"sourceOwner", "logos-co"},
            {"sourceRepo",  "logos-modules-release"},
            {"sourceHost",  "raw.githubusercontent.com"},
        };
    }

private slots:
    // The repository's own logos-repo.json can declare where it lives. That is
    // chosen rather than inferred, so it outranks anything we could work out.
    void a_declared_homepage_wins()
    {
        QVariantMap repo = officialRepo();
        repo.insert("homepage", "https://logos.co/modules");

        QCOMPARE(repositorySourceFor(repo).link, QStringLiteral("https://logos.co/modules"));
    }

    void whitespace_only_homepage_is_not_a_homepage()
    {
        QVariantMap repo = officialRepo();
        repo.insert("homepage", "   ");

        QCOMPARE(repositorySourceFor(repo).link,
                 QStringLiteral("https://github.com/logos-co/logos-modules-release"));
    }

    // No homepage — the common case today. owner and repo are only ever filled
    // in for GitHub-hosted catalogs, so composing github.com/owner/repo out of
    // them is a mapping onto the one host that produces them, not a guess.
    void without_a_homepage_the_owner_and_repo_name_the_page()
    {
        const auto src = repositorySourceFor(officialRepo());

        QCOMPARE(src.link, QStringLiteral("https://github.com/logos-co/logos-modules-release"));
        QVERIFY(!src.link.contains(QStringLiteral("raw.githubusercontent.com")));
    }

    // A catalog on a host the downloader does not recognise leaves owner and
    // repo empty. Inventing a repository page from the URL would be worse than
    // showing the URL: the reader opens the guess and is reassured by the wrong
    // thing.
    void an_unrecognised_host_keeps_its_own_url()
    {
        const QVariantMap repo{
            {"url", "https://packages.example.org/logos-repo.json"},
            {"displayLabel", "Example Packages"},
            {"sourceHost", "packages.example.org"},
        };

        QCOMPARE(repositorySourceFor(repo).link,
                 QStringLiteral("https://packages.example.org/logos-repo.json"));
    }

    // Half a pair names nothing. A repository URL with one path segment is not
    // a GitHub repository, whatever its host says.
    void an_owner_without_a_repo_names_no_page()
    {
        QVariantMap repo = officialRepo();
        repo.insert("sourceRepo", QString());

        QCOMPARE(repositorySourceFor(repo).link, repo.value("url").toString());
    }

    // The label is PackageCoordinator's, not ours: it is the repository's own
    // name, already qualified with its owner when a second repository claims
    // the same one. Recomputing it here would be a second answer to a question
    // that has one.
    void the_label_is_the_name_every_other_view_shows()
    {
        QCOMPARE(repositorySourceFor(officialRepo()).label,
                 QStringLiteral("Logos Official Modules"));

        QVariantMap contested = officialRepo();
        contested.insert("displayLabel", "Logos Official Modules (0x-r4bbit)");
        QCOMPARE(repositorySourceFor(contested).label,
                 QStringLiteral("Logos Official Modules (0x-r4bbit)"));
    }

    // An entry that never went through withDisplayLabels. Falling back to the
    // URL states an address rather than claiming a name it was not given.
    void an_unstamped_entry_falls_back_to_its_url()
    {
        QVariantMap repo = officialRepo();
        repo.remove("displayLabel");

        QCOMPARE(repositorySourceFor(repo).label, repo.value("url").toString());
    }

    // Nothing in, nothing out. The dialogs hide the source row on an empty
    // link, so this is what "do not show a source line at all" looks like.
    void an_empty_entry_yields_nothing_to_render()
    {
        const auto src = repositorySourceFor(QVariantMap{});

        QVERIFY(src.label.isEmpty());
        QVERIFY(src.link.isEmpty());
    }

    // ── repositoryLabelForEntry: the install/upgrade confirmation dep rows ──

    // The bug this fixes. A fork publishes the same `repositoryDisplayName` as
    // the catalog it forked, and the dep rows used that name directly — so both
    // rendered "Logos Official Modules" while the App Manager, which goes
    // through displayLabel, told them apart. Same-looking row, different
    // publisher, on the dialog that asks you to approve the install.
    void a_held_repository_is_named_by_its_owner_qualified_label()
    {
        const QVariantMap entry{
            {"name", "wallet_module"},
            {"repositoryUrl", "https://raw.githubusercontent.com/0x-r4bbit/"
                              "logos-modules-release/main/logos-repo.json"},
            {"repositoryDisplayName", "Logos Official Modules"},
        };
        const QVariantMap repo{
            {"url", entry.value("repositoryUrl")},
            {"displayLabel", "Logos Official Modules (0x-r4bbit)"},
        };

        QCOMPARE(repositoryLabelForEntry(entry, repo),
                 QStringLiteral("Logos Official Modules (0x-r4bbit)"));
    }

    // The catalog can arrive before listRepositories() comes back, and a repo
    // can be removed after. Its own name is not owner-qualified, but it beats
    // showing a raw URL in a dep row.
    void an_unheld_repository_falls_back_to_the_catalogs_own_name()
    {
        const QVariantMap entry{
            {"repositoryUrl", "https://packages.example.org/logos-repo.json"},
            {"repositoryDisplayName", "Example Packages"},
        };

        QCOMPARE(repositoryLabelForEntry(entry, QVariantMap{}),
                 QStringLiteral("Example Packages"));
    }

    void with_neither_a_row_nor_a_name_the_url_stands()
    {
        const QVariantMap entry{
            {"repositoryUrl", "https://packages.example.org/logos-repo.json"},
        };

        QCOMPARE(repositoryLabelForEntry(entry, QVariantMap{}),
                 QStringLiteral("https://packages.example.org/logos-repo.json"));
    }

    // A resolver error row carries neither. The dialog renders
    // `modelData.repository || ""`, so empty is the same as absent — which is
    // what those rows had before they were stamped unconditionally.
    void an_entry_with_nothing_to_say_says_nothing()
    {
        QVERIFY(repositoryLabelForEntry(QVariantMap{}, QVariantMap{}).isEmpty());
    }
};

QTEST_MAIN(RepositorySourceTest)
#include "repository_source_test.moc"
